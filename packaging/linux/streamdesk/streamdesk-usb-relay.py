#!/usr/bin/env python3
"""Forward Sunshine's GameStream endpoints to a USB-attached StreamDesk client."""

import argparse
import asyncio
from contextlib import asynccontextmanager, contextmanager, suppress
import ctypes
from ctypes.util import find_library
import fcntl
import os
from pathlib import Path
import socket
import struct
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
RETRY_DELAY = 2
USBMUX_LOOKUP_USB = 1 << 1
USB_CONNECTION = 1


class UsbDeviceInfo(ctypes.Structure):
    """Device information from libusbmuxd's public C header."""

    _fields_ = [
        ("handle", ctypes.c_uint32),
        ("product_id", ctypes.c_uint32),
        ("udid", ctypes.c_char * 44),
        ("connection_type", ctypes.c_int),
        ("connection_data", ctypes.c_uint8 * 200),
    ]


class UsbUnavailable(OSError):
    """No unique physical USB receiver is currently available."""


class UsbDevice:
    """Open TCP channels through a physical USB connection only."""

    def __init__(self):
        library = find_library("usbmuxd-2.0")
        if not library:
            raise RuntimeError("libusbmuxd is required.")
        self.mux = ctypes.CDLL(library)
        device_list = ctypes.POINTER(UsbDeviceInfo)
        self.mux.usbmuxd_get_device_list.argtypes = [ctypes.POINTER(device_list)]
        self.mux.usbmuxd_get_device_list.restype = ctypes.c_int
        self.mux.usbmuxd_device_list_free.argtypes = [ctypes.POINTER(device_list)]
        self.mux.usbmuxd_device_list_free.restype = ctypes.c_int
        self.mux.usbmuxd_get_device.argtypes = [ctypes.c_char_p, device_list, ctypes.c_int]
        self.mux.usbmuxd_get_device.restype = ctypes.c_int
        self.mux.usbmuxd_connect.argtypes = [ctypes.c_uint32, ctypes.c_ushort]
        self.mux.usbmuxd_connect.restype = ctypes.c_int

    def select(self, udid):
        """Resolve a fresh handle after disconnect or device replacement."""
        if udid is not None:
            device = UsbDeviceInfo()
            result = self.mux.usbmuxd_get_device(
                udid.encode("ascii"), ctypes.byref(device), USBMUX_LOOKUP_USB
            )
            if result == 1 and device.connection_type == USB_CONNECTION:
                return device.handle
            raise UsbUnavailable("The selected device is unavailable over USB.")

        devices = ctypes.POINTER(UsbDeviceInfo)()
        count = self.mux.usbmuxd_get_device_list(ctypes.byref(devices))
        try:
            if count < 0:
                raise UsbUnavailable("usbmuxd is not available.")
            handles = {devices[index].handle for index in range(count)
                       if devices[index].connection_type == USB_CONNECTION}
        finally:
            if devices:
                self.mux.usbmuxd_device_list_free(ctypes.byref(devices))
        if len(handles) != 1:
            raise UsbUnavailable("Connect one iPad over USB, or pass --udid.")
        return handles.pop()

    async def open(self, handle):
        """Return streams for a fresh USB lane."""
        # Keep a reference to the socket in the worker result so cancellation
        # before the result is consumed also releases its file descriptor.
        sock = await asyncio.to_thread(self.connect, handle)
        try:
            return await asyncio.open_connection(sock=sock)
        except BaseException:
            sock.close()
            raise

    def connect(self, handle):
        """Connect through usbmuxd to the client listener."""
        fd = self.mux.usbmuxd_connect(handle, USB_PORT)
        if fd < 0:
            raise UsbUnavailable("Open Connect → USB in StreamDesk on the iPad.")
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


@contextmanager
def relay_lock(port):
    """Allow one managed relay per Sunshine base port."""
    runtime = Path(os.environ.get("XDG_RUNTIME_DIR") or f"/run/user/{os.getuid()}")
    with (runtime / f"streamdesk-usb-{port}.lock").open("a") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise RuntimeError("A StreamDesk USB relay is already running for this port.") from None
        yield


async def connect_loop(device, udid, port, wait, status=print):
    """Serve one session, or wait for a new cable session after disconnect."""
    waiting_message = None

    def online():
        nonlocal waiting_message
        waiting_message = None
        status("StreamDesk USB connected.")

    while True:
        try:
            handle = await asyncio.to_thread(device.select, udid)

            async def open_usb():
                return await device.open(handle)

            await serve_relay(open_usb, port, online)
            if not wait:
                return
            message = "StreamDesk USB disconnected. Waiting for the next session."
        except (OSError, ValueError, asyncio.IncompleteReadError) as error:
            if not wait:
                raise
            message = str(error) or "Waiting for StreamDesk USB."
        if message != waiting_message:
            status(message)
            waiting_message = message
        await asyncio.sleep(RETRY_DELAY)


def main():
    """Run a foreground Lightning relay for the local StreamDesk Host."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--udid", help="select a physical iPad when several devices are attached")
    parser.add_argument("--port", type=int, default=48189, help="Sunshine GameStream base port")
    parser.add_argument("--wait", action="store_true", help="wait for the iPad and reconnect after unplug")
    args = parser.parse_args()
    if not 6 <= args.port <= 65535 - max(TCP_OFFSETS):
        parser.error("--port leaves a GameStream endpoint outside the valid port range")
    try:
        with relay_lock(args.port):
            asyncio.run(connect_loop(UsbDevice(), args.udid, args.port, args.wait,
                                     lambda message: print(message, flush=True)))
    except KeyboardInterrupt:
        return
    except (OSError, RuntimeError, ValueError, asyncio.IncompleteReadError) as error:
        parser.exit(1, f"streamdesk-usb-relay: {error}\n")


if __name__ == "__main__":
    main()
