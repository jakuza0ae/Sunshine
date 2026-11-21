#!/usr/bin/env python3
"""Exercise the USB relay with emulated cable lanes and real loopback sockets."""

import asyncio
from pathlib import Path
import runpy
import socket
import unittest


relay = runpy.run_path(str(Path(__file__).with_name("streamdesk-usb-relay.py")))
MAGIC = relay["MAGIC"]
CONTROL, TCP, UDP = (relay[key] for key in ("CONTROL", "TCP", "UDP"))
LANE_HEADER = relay["LANE_HEADER"]
OPEN_LANE = relay["OPEN_LANE"]
UDP_LENGTH = relay["UDP_LENGTH"]


class UsbRelayTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.cable_lanes = asyncio.Queue()
        self.writers = []
        self.cable = await asyncio.start_server(self.accept_cable, "127.0.0.1", 0)
        self.addCleanup(self.cable.close)
        self.cable_address = self.cable.sockets[0].getsockname()

    def accept_cable(self, reader, writer):
        self.writers.append(writer)
        self.cable_lanes.put_nowait((reader, writer))

    async def asyncTearDown(self):
        for writer in self.writers:
            writer.close()
            await writer.wait_closed()

    async def wait(self, awaitable):
        return await asyncio.wait_for(awaitable, 2)

    async def start_relay(self, port):
        async def open_usb():
            return await asyncio.open_connection(*self.cable_address)

        job = asyncio.create_task(relay["serve_relay"](open_usb, port))
        self.addAsyncCleanup(self.cleanup_job, job)
        reader, writer = await self.lane(CONTROL, 0)
        self.assertEqual(await self.wait(reader.readexactly(2)), relay["BASE_PORT"].pack(port))
        writer.write(MAGIC)
        await writer.drain()
        return job, reader, writer

    async def cleanup_job(self, job):
        job.cancel()
        await asyncio.gather(job, return_exceptions=True)

    async def lane(self, kind, channel):
        reader, writer = await self.wait(self.cable_lanes.get())
        self.assertEqual(await self.wait(reader.readexactly(LANE_HEADER.size)),
                         LANE_HEADER.pack(MAGIC, kind, channel))
        return reader, writer

    async def test_tcp_keeps_half_close_and_udp_lanes_are_independent(self):
        async def echo_at_eof(reader, writer):
            async with relay["closing_writer"](writer):
                writer.write(await reader.read())
                await writer.drain()

        server = await asyncio.start_server(echo_at_eof, "127.0.0.1", 0)
        self.addCleanup(server.close)
        base = server.sockets[0].getsockname()[1]
        video = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.addCleanup(video.close)
        video.bind(("127.0.0.1", base + 9))
        video.setblocking(False)
        control = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.addCleanup(control.close)
        control.bind(("127.0.0.1", base + 10))
        control.setblocking(False)

        job, _, bootstrap = await self.start_relay(base)
        request = OPEN_LANE.pack(TCP, 51, 0)
        bootstrap.write(request[:3])
        await bootstrap.drain()
        await asyncio.sleep(0)
        bootstrap.write(request[3:])
        await bootstrap.drain()
        tcp_reader, tcp_writer = await self.lane(TCP, 51)
        tcp_writer.write(b"GET /serverinfo")
        await tcp_writer.drain()
        tcp_writer.write_eof()
        self.assertEqual(await self.wait(tcp_reader.read()), b"GET /serverinfo")

        bootstrap.write(OPEN_LANE.pack(UDP, 52, 9))
        await bootstrap.drain()
        video_reader, video_writer = await self.lane(UDP, 52)
        bootstrap.write(OPEN_LANE.pack(UDP, 53, 10))
        await bootstrap.drain()
        control_reader, control_writer = await self.lane(UDP, 53)

        payloads = (b"one", b"longer-datagram")
        video_writer.write(b"".join(UDP_LENGTH.pack(len(data)) + data for data in payloads))
        control_writer.write(UDP_LENGTH.pack(3) + b"key")
        await asyncio.gather(video_writer.drain(), control_writer.drain())

        loop = asyncio.get_running_loop()
        control_packet, control_client = await self.wait(loop.sock_recvfrom(control, 100))
        self.assertEqual(control_packet, b"key")
        await self.wait(loop.sock_sendto(control, b"ack", control_client))
        self.assertEqual(await self.wait(control_reader.readexactly(5)), UDP_LENGTH.pack(3) + b"ack")

        for expected in payloads:
            packet, client = await self.wait(loop.sock_recvfrom(video, 100))
            self.assertEqual(packet, expected)
            await self.wait(loop.sock_sendto(video, packet, client))
            self.assertEqual(await self.wait(video_reader.readexactly(2 + len(packet))),
                             UDP_LENGTH.pack(len(packet)) + packet)

        bootstrap.close()
        await self.wait(job)
        self.assertEqual(await self.wait(video_reader.read()), b"")
        self.assertEqual(await self.wait(control_reader.read()), b"")

    async def test_rejects_request_outside_sunshine_endpoints(self):
        job, reader, writer = await self.start_relay(48189)
        writer.write(OPEN_LANE.pack(TCP, 1, 11))
        await writer.drain()
        with self.assertRaisesRegex(ValueError, "Invalid USB lane request"):
            await self.wait(job)
        self.assertEqual(await self.wait(reader.read()), b"")
        self.assertTrue(self.cable_lanes.empty())


if __name__ == "__main__":
    unittest.main()
