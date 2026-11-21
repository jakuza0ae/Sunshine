#!/usr/bin/env python3
"""Forward Sunshine's GameStream endpoints to a USB-attached StreamDesk client."""

import argparse
import asyncio
from contextlib import asynccontextmanager, suppress
import ctypes
from ctypes.util import find_library
import socket
import struct
import subprocess
import sys


MAGIC = b"SDU1"
USB_PORT = 41589
CONTROL, TCP, UDP = range(3)
LANE_HEADER = struct.Struct("!4sBI")
BASE_PORT = struct.Struct("!H")
OPEN_LANE = struct.Struct("!BIb")
UDP_LENGTH = struct.Struct("!H")
TCP_OFFSETS = frozenset((-5, 0, 21))
UDP_OFFSETS = frozenset((9, 10, 11))
MAX_LANES = 24
MAX_DATAGRAM = 16384
SETUP_TIMEOUT = 5


class UsbDeviceInfo(ctypes.Structure):
    """Device information from libusbmuxd's public C header."""

    _fields_ = [
        ("handle", ctypes.c_uint32),
        ("product_id", ctypes.c_uint32),
        ("udid", ctypes.c_char * 44),
        ("connection_type", ctypes.c_int),
        ("connection_data", ctypes.c_uint8 * 200),
    ]


class UsbDevice:
    """Open TCP channels through a physical USB connection only."""

    def __init__(self, udid):
        if udid is None:
            result = subprocess.run(
                ["idevice_id", "-l"], capture_output=True, text=True, check=True, timeout=SETUP_TIMEOUT
            )
            devices = result.stdout.splitlines()
            if len(devices) != 1:
                raise RuntimeError("Connect one iPad over USB, or pass --udid.")
            udid = devices[0]

        library = find_library("usbmuxd-2.0")
        if not library:
            raise RuntimeError("libusbmuxd is required.")
        self.mux = ctypes.CDLL(library)
        self.mux.usbmuxd_get_device.argtypes = [
            ctypes.c_char_p, ctypes.POINTER(UsbDeviceInfo), ctypes.c_int
        ]
        self.mux.usbmuxd_get_device.restype = ctypes.c_int
        self.mux.usbmuxd_connect.argtypes = [ctypes.c_uint32, ctypes.c_ushort]
        self.mux.usbmuxd_connect.restype = ctypes.c_int
        device = UsbDeviceInfo()
        lookup_usb = 1 << 1
        result = self.mux.usbmuxd_get_device(udid.encode(), ctypes.byref(device), lookup_usb)
        if result != 1 or device.connection_type != 1:
            raise RuntimeError("The selected device is unavailable over USB.")
        self.handle = device.handle

    async def open(self):
        """Return streams for a fresh USB lane."""
        # Keep a reference to the socket in the worker result so cancellation
        # before the result is consumed also releases its file descriptor.
        sock = await asyncio.to_thread(self.connect)
        try:
            return await asyncio.open_connection(sock=sock)
        except BaseException:
            sock.close()
            raise

    def connect(self):
        """Connect through usbmuxd to the client listener."""
        fd = self.mux.usbmuxd_connect(self.handle, USB_PORT)
        if fd < 0:
            raise OSError("Cannot open the USB lane. Open StreamDesk's USB listener on the iPad.")
        return socket.socket(fileno=fd)


@asynccontextmanager
async def closing_writer(writer):
    """Close a stream and await its transport on every exit path."""
    try:
        yield writer
    finally:
        writer.close()
        with suppress(OSError, TimeoutError):
            await asyncio.wait_for(writer.wait_closed(), SETUP_TIMEOUT)


async def copy_tcp(reader, writer):
    """Copy one half of a TCP stream, preserving end-of-stream."""
    while data := await reader.read(16384):
        writer.write(data)
        await writer.drain()
    writer.write_eof()


async def tcp_lane(reader, writer, port):
    """Relay a single TCP connection to Sunshine's IPv4 loopback listener."""
    target_reader, target_writer = await asyncio.wait_for(
        asyncio.open_connection("127.0.0.1", port), SETUP_TIMEOUT
    )
    async with closing_writer(target_writer), asyncio.TaskGroup() as tasks:
        tasks.create_task(copy_tcp(reader, target_writer))
        tasks.create_task(copy_tcp(target_reader, writer))


async def receive_datagrams(reader, sock):
    """Decode length-prefixed datagrams, preserving each datagram's boundary."""
    loop = asyncio.get_running_loop()
    while size_bytes := await reader.read(UDP_LENGTH.size):
        size_bytes += await reader.readexactly(UDP_LENGTH.size - len(size_bytes))
        (size,) = UDP_LENGTH.unpack(size_bytes)
        if not 0 < size <= MAX_DATAGRAM:
            raise ValueError("Invalid UDP datagram length.")
        payload = await reader.readexactly(size)
        try:
            await loop.sock_sendall(sock, payload)
        except ConnectionRefusedError:
            # A ping can arrive before Sunshine has opened the stream port.
            continue


async def send_datagrams(writer, sock):
    """Send the UDP replies belonging to this lane back over the cable."""
    loop = asyncio.get_running_loop()
    while True:
        try:
            data = await loop.sock_recv(sock, MAX_DATAGRAM + 1)
        except ConnectionRefusedError:
            continue
        if 0 < len(data) <= MAX_DATAGRAM:
            writer.write(UDP_LENGTH.pack(len(data)) + data)
            await writer.drain()


async def udp_lane(reader, writer, port):
    """Relay a single UDP endpoint using its own USB TCP connection."""
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.setblocking(False)
        sock.connect(("127.0.0.1", port))
        async with asyncio.TaskGroup() as tasks:
            outgoing = tasks.create_task(send_datagrams(writer, sock))
            await receive_datagrams(reader, sock)
            outgoing.cancel()


async def relay_lane(open_usb, kind, channel, port):
    """Open one independent cable lane and relay until it closes."""
    reader, writer = await open_usb()
    async with closing_writer(writer):
        writer.write(LANE_HEADER.pack(MAGIC, kind, channel))
        await writer.drain()
        await (tcp_lane if kind == TCP else udp_lane)(reader, writer, port)


async def serve_relay(open_usb, base, online=lambda: None):
    """Offer only mapped Sunshine ports and tear down every lane on unplug."""
    reader, writer = await open_usb()
    lanes = {}
    async with closing_writer(writer):
        writer.write(LANE_HEADER.pack(MAGIC, CONTROL, 0) + BASE_PORT.pack(base))
        await writer.drain()
        if await asyncio.wait_for(reader.readexactly(len(MAGIC)), SETUP_TIMEOUT) != MAGIC:
            raise ValueError("The iPad listener uses an incompatible USB protocol.")
        online()
        try:
            while first := await reader.read(1):
                kind, channel, offset = OPEN_LANE.unpack(
                    first + await reader.readexactly(OPEN_LANE.size - 1)
                )
                allowed = {TCP: TCP_OFFSETS, UDP: UDP_OFFSETS}.get(kind, ())
                if not channel or channel in lanes or offset not in allowed or len(lanes) >= MAX_LANES:
                    raise ValueError("Invalid USB lane request.")

                async def run_lane(lane_kind, lane_channel, port):
                    try:
                        await relay_lane(open_usb, lane_kind, lane_channel, port)
                    except* (OSError, ValueError, asyncio.IncompleteReadError) as error:
                        print(f"USB lane {lane_channel} closed: {error}", file=sys.stderr)
                    finally:
                        lanes.pop(lane_channel, None)

                lanes[channel] = asyncio.create_task(run_lane(kind, channel, base + offset))
        finally:
            remaining = list(lanes.values())
            for lane in remaining:
                lane.cancel()
            await asyncio.gather(*remaining, return_exceptions=True)


def main():
    """Run a foreground Lightning relay for the local StreamDesk Host."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--udid", help="select a physical iPad when several devices are attached")
    parser.add_argument("--port", type=int, default=48189, help="Sunshine GameStream base port")
    args = parser.parse_args()
    if not 6 <= args.port <= 65535 - max(TCP_OFFSETS):
        parser.error("--port leaves a GameStream endpoint outside the valid port range")
    try:
        device = UsbDevice(args.udid)
        asyncio.run(serve_relay(device.open, args.port, lambda: print("StreamDesk USB connected.", flush=True)))
    except KeyboardInterrupt:
        return
    except (OSError, RuntimeError, ValueError, asyncio.IncompleteReadError, subprocess.SubprocessError) as error:
        parser.exit(1, f"streamdesk-usb-relay: {error}\n")


if __name__ == "__main__":
    main()
