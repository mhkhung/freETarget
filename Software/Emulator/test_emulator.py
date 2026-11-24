"""Fast checks; the separate smoke command exercises the emulated CPU."""
import collections
import math
import socket
import struct
import tempfile
import unittest
from pathlib import Path

import protocol as wire
from board import ARM_SETTLE_NS, Board, CLOCK_PIN, crc8
from physics import Target, speed_of_sound
from prepare import partition, refresh_firmware
from runtime import dashboard_host_ok
from websocket import WebSocket


class PhysicsTests(unittest.TestCase):
    def test_centre_symmetry_and_units(self):
        arrivals = Target().arrivals(0, 0)
        self.assertEqual(len(set(arrivals)), 1)
        self.assertAlmostEqual(arrivals[0] / 1000, 116 / speed_of_sound(), delta=0.05)
        self.assertTrue(0.343 < speed_of_sound() < 0.345)

    def test_cardinal_directions_and_rotation(self):
        north, east, south, west = Target(angle_deg=0).arrivals(30, 0)
        self.assertEqual(north, south)
        self.assertLess(east, north)
        self.assertGreater(west, north)
        self.assertEqual(Target(angle_deg=90).arrivals(0, -30), [north, east, south, west])
        self.assertLess(Target().arrivals(0, -30)[0], Target().arrivals(0, -30)[2])

    def test_default_sensors_are_at_corners(self):
        target = Target()
        corner = 116 / math.sqrt(2)
        expected = ((-corner, -corner), (corner, -corner), (corner, corner), (-corner, corner))
        for sensor, name, (x, y) in zip(target.sensors(), "NESW", expected):
            self.assertEqual(sensor["name"], name)
            self.assertAlmostEqual(sensor["x"], x)
            self.assertAlmostEqual(sensor["y"], y)
            arrivals = target.arrivals(x, y)
            self.assertEqual(arrivals.index(min(arrivals)), "NESW".index(name))
            self.assertEqual(min(arrivals), 0)

    def test_top_and_right_edge_arrivals_match_corner_layout(self):
        n, e, s, w = Target().arrivals(0, -30)
        self.assertEqual(n, e)
        self.assertEqual(s, w)
        self.assertLess(n, s)
        n, e, s, w = Target().arrivals(30, 0)
        self.assertEqual(e, s)
        self.assertEqual(n, w)
        self.assertLess(e, n)

    def test_mirror_and_depth(self):
        n, e, s, w = Target().arrivals(25, -40)
        self.assertEqual(Target().arrivals(-25, 40), [s, w, n, e])
        self.assertGreater(Target(depth_mm=10).arrivals(0, 0)[0], Target().arrivals(0, 0)[0])

    def test_environment_changes_propagation(self):
        self.assertGreater(speed_of_sound(30), speed_of_sound(10))
        self.assertGreater(speed_of_sound(20, 90), speed_of_sound(20, 10))
        self.assertLess(Target(temperature=30).arrivals(0, 0)[0], Target(temperature=10).arrivals(0, 0)[0])

    def test_reject_invalid_inputs(self):
        for value in (math.inf, math.nan, 1e308):
            with self.assertRaises(ValueError):
                Target().arrivals(value, 0)
        for temperature, humidity in ((41, 50), (20, -1), (math.nan, 0)):
            with self.assertRaises(ValueError):
                speed_of_sound(temperature, humidity)


class FakeSocket:
    def __init__(self, chunks):
        self.chunks = collections.deque(chunks)
        self.sent = bytearray()

    def recv(self, size):
        chunk = self.chunks.popleft() if self.chunks else b""
        if isinstance(chunk, Exception):
            raise chunk
        if len(chunk) > size:
            self.chunks.appendleft(chunk[size:])
        return chunk[:size]

    def sendall(self, data):
        self.sent.extend(data)


class ProtocolTests(unittest.TestCase):
    def test_fragmented_transport_and_nested_message(self):
        data = wire.gpio(6, True, 123456789012)
        frame = struct.pack("<H", 4) + b"GPIO" + data
        stream = struct.pack("<I", len(frame)) + frame
        topic, result = wire.receive(FakeSocket(bytes([b]) for b in stream))
        self.assertEqual(topic, "GPIO")
        self.assertEqual(result[1], 123456789012)
        self.assertEqual(wire.fields(result[6]), {1: 6, 2: 1})

    def test_refuse_malformed_or_unbounded_data(self):
        for data in (b"\x00", b"\x80" * 11, b"\x12\x05a", b"\x0b"):
            with self.assertRaises(ValueError):
                wire.fields(data)
        with self.assertRaises(ValueError):
            wire.receive(FakeSocket([struct.pack("<I", wire.MAX_FRAME + 1)]))
        for widths in ([], [0], [50] * 513):
            with self.assertRaises(ValueError):
                wire.pulses(8, 100, widths)

    def test_timeout_does_not_lose_partial_websocket_frame(self):
        ws = WebSocket.__new__(WebSocket)
        ws.buffer, ws.fragments = bytearray(), bytearray()
        ws.sock = FakeSocket([b'\x81\x0a{"sh', socket.timeout(), b'ot":1}'])
        with self.assertRaises(socket.timeout):
            ws.receive()
        self.assertEqual(ws.receive(), {"shot": 1})

    def test_fragmented_message_with_ping(self):
        ws = WebSocket.__new__(WebSocket)
        ws.buffer, ws.fragments = bytearray(), bytearray()
        ws.sock = FakeSocket([b'\x01\x05{"x":\x89\x01p\x80\x021}'])
        self.assertEqual(ws.receive(), {"x": 1})
        self.assertTrue(ws.sock.sent)  # A masked pong was sent.

    def test_pong_is_counted_not_returned(self):
        ws = WebSocket.__new__(WebSocket)
        ws.buffer, ws.fragments = bytearray(), bytearray()
        ws.sock = FakeSocket([b'\x8a\x03lab\x81\x0a{"shot":1}'])
        self.assertEqual(ws.receive(), {"shot": 1})
        self.assertEqual(ws.pongs, 1)

    def test_close_sends_a_close_frame(self):
        ws = WebSocket.__new__(WebSocket)
        ws.sock, ws.opened = FakeSocket([]), True
        ws.sock.shutdown = ws.sock.close = lambda *args: None
        ws.close()
        ws.close()
        self.assertEqual(ws.sock.sent[0], 0x88)   # FIN + close, sent only once
        self.assertEqual(len(ws.sock.sent), 2 + 4 + 2)


class BoardTests(unittest.TestCase):
    def test_gate_order_and_clock_queue_budget(self):
        board = Board(0)
        board.attached.set()
        board.armed = True
        board.now_ns = 1000000
        sent = []
        board.send = lambda data: sent.append(wire.fields(data))
        shot = board.fire(30, -20)
        board.next_quantum()
        gates = [(m[1], wire.fields(m[6])[1]) for m in sent if 6 in m]
        self.assertEqual([pin for _, pin in sorted(gates)], [6, 7, 5, 15])
        self.assertTrue(all(at > board.now_ns for at, _ in gates))
        self.assertTrue(all(n % 100 == 0 for n in shot["arrivals_ns"]))
        self.assertTrue(any(14 in m and wire.fields(m[14])[1] == CLOCK_PIN for m in sent))
        with self.assertRaises(RuntimeError):
            board.fire(0, 0)

    def test_waits_for_second_rearm_before_firing(self):
        board = Board(0)
        board.attached.set()
        board.send = lambda data: None
        board.armed, board.now_ns = True, 5_000_000_000
        board.armed_at_ns = board.now_ns - ARM_SETTLE_NS // 2  # Only the ISR has re-armed
        self.assertFalse(board.ready_to_fire())
        with self.assertRaises(RuntimeError):
            board.fire(0, 0)
        board.now_ns += ARM_SETTLE_NS
        self.assertTrue(board.ready_to_fire())
        board.fire(0, 0)

    def test_sensor_crc_known_vector(self):
        self.assertEqual(crc8(bytes.fromhex("beef")), 0x92)

    def test_missing_sensor_omits_gate_signal(self):
        board = Board(0)
        board.attached.set()
        board.armed = True
        sent = []
        board.send = lambda data: sent.append(wire.fields(data))
        board.fire(0, 0, missing=("E",))
        board.next_quantum()
        self.assertEqual({wire.fields(m[6])[1] for m in sent if 6 in m}, {5, 7, 15})


class DashboardHostTests(unittest.TestCase):
    def test_loopback_accepts_only_127_0_0_1(self):
        self.assertTrue(dashboard_host_ok("127.0.0.1:18888", 18888, False))
        for host in ("localhost:18888", "192.168.1.20:18888", "127.0.0.1:18889", None):
            self.assertFalse(dashboard_host_ok(host, 18888, False), host)

    def test_lan_accepts_ip_addresses_but_not_names(self):
        for host in ("127.0.0.1:18888", "192.168.1.20:18888", "10.0.0.5:18888"):
            self.assertTrue(dashboard_host_ok(host, 18888, True), host)
        for host in ("evil.example:18888", "localhost:18888", "192.168.1.20:80", "192.168.1.20", "[::1]:18888", "", None):
            self.assertFalse(dashboard_host_ok(host, 18888, True), host)


class FlashTests(unittest.TestCase):
    TABLE = ("# Name, Type, SubType, Offset, Size\n"
             "nvs,      data, nvs,     0x9000,  0x4000,\n"
             "ota_data, data, ota,     0xd000,  0x2000\n"
             "factory,  app,  factory, 0x10000, 64K,\n"
             "ota_0,    app,  ota_0,   ,        64K,\n"
             "shots,    data, 0x40,    0x40000, 16K,\n")

    def test_partition_sizes(self):
        with tempfile.TemporaryDirectory() as folder:
            table = Path(folder, "partitions.csv")
            table.write_text(self.TABLE)
            self.assertEqual(partition(table, "factory"), (0x10000, 0x10000))
            self.assertEqual(partition(table, "ota_data"), (0xd000, 0x2000))
            with self.assertRaises(ValueError):
                partition(table, "ota_0")              # No explicit offset

    def test_refresh_keeps_settings_and_journal(self):
        with tempfile.TemporaryDirectory() as folder:
            folder = Path(folder)
            (folder / "partitions.csv").write_text(self.TABLE)
            size = 0x50000
            (folder / "factory.bin").write_bytes(b"N" * size)           # The new build
            saved = bytearray(b"O" * size)                              # Old app, settings and shots
            (folder / "target.bin").write_bytes(saved)
            refresh_firmware(folder / "target.bin", folder / "factory.bin", folder / "partitions.csv")
            flash = (folder / "target.bin").read_bytes()
            self.assertEqual(set(flash[:0x9000]), {ord("N")})          # Bootloader and partition table
            self.assertEqual(set(flash[0x9000:0xd000]), {ord("O")})    # NVS settings kept
            self.assertEqual(set(flash[0xd000:0xf000]), {ord("N")})    # Boot the factory app
            self.assertEqual(set(flash[0xf000:0x10000]), {ord("O")})
            self.assertEqual(set(flash[0x10000:0x20000]), {ord("N")})  # The new app
            self.assertEqual(set(flash[0x20000:]), {ord("O")})         # OTA slots and the shot journal kept


if __name__ == "__main__":
    unittest.main()
