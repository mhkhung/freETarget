"""Virtual V5.2 board connected to the emulated ESP32-S3 through co-simulation."""
import collections
import socket
import threading

import protocol as wire
from physics import Target

RUN_PINS = (5, 6, 7, 15)
HIGH_PINS = (16, 9, 10, 11)
STOP_PIN, TRIGGER_PIN, OSC_PIN, CLOCK_PIN = 21, 47, 48, 8
QUANTUM_NS = 100_000
# After a shot the firmware re-arms twice: the 1 ms timer ISR when the ring
# timer expires, then the foreground arm() up to one 10 ms-tick vTaskDelay(10)
# (100 ms at CONFIG_FREERTOS_HZ=100) later. Its stop_timers() clears any
# shot latched in between, so only fire once STOP has stayed high this long.
ARM_SETTLE_NS = 200_000_000


def crc8(data):
    value = 0xff
    for byte in data:
        value ^= byte
        for _ in range(8):
            value = ((value << 1) ^ 0x31) & 0xff if value & 0x80 else (value << 1) & 0xff
    return value


class Board(threading.Thread):
    def __init__(self, port, target=None):
        super().__init__(name="virtual-board", daemon=True)
        self.port = port
        self.target = target or Target()
        self.stop_event = threading.Event()
        self.attached = threading.Event()
        self.lock = threading.RLock()
        self.commands = collections.deque()
        self.drives = collections.deque()
        self.now_ns = 0
        self.levels = (1 << 49) - 1
        self.armed = False
        self.armed_at_ns = -ARM_SETTLE_NS  # Virtual time of the last STOP edge
        self.busy = False
        self.clock_until_ns = 0
        self.next_clock_ns = 0
        self.expected = collections.deque(maxlen=5000)
        self.next_shot_id = 1
        self.error = None
        self.events_late = self.dropped = 0
        self.max_lateness_ns = self.broadcasts_dropped = 0
        self.sensor_reads = self.dac_writes = 0
        self.sock = None
        self._sensor_offset = 0

    def send(self, data):
        wire.send(self.sock, data)

    def fire(self, x, y, missing=()):
        arrivals = self.target.arrivals(x, y)
        if not set(missing) <= {"N", "E", "S", "W"}:
            raise ValueError("missing sensors must be N, E, S or W")
        with self.lock:
            if self.error or not self.attached.is_set():
                raise RuntimeError(self.error or "board not attached")
            if not self.ready_to_fire():
                raise RuntimeError("target is booting or waiting for the previous shot to settle")
            shot = {"id": self.next_shot_id, "x": x, "y": y,
                    "arrivals_ns": arrivals, "missing": list(missing), "status": "queued"}
            self.expected.append(shot)
            self.next_shot_id += 1
            self.commands.append(shot)
            self.busy = True
            return dict(shot)

    def ready_to_fire(self):
        """Armed, the previous shot settled, and STOP stable past the second re-arm."""
        with self.lock:
            return self.armed and not self.busy and self.now_ns - self.armed_at_ns >= ARM_SETTLE_NS

    def close(self):
        self.stop_event.set()
        if self.sock:
            try:
                self.sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass

    def clear_latches(self):
        for pin in RUN_PINS + HIGH_PINS:
            self.drives.append((pin, False))

    def initialize(self):
        # These values select V5.2 and a present 12 V supply via the stock ADC driver.
        self.send(wire.analog(4, 3300, self.now_ns + 1000))
        self.send(wire.analog(1, 1250, self.now_ns + 1000))
        for pin in (35, 36, 37, 38):
            self.drives.append((pin, True))
        self.drives.append((40, False))
        self.clear_latches()
        self.drives.append((CLOCK_PIN, False))

    def bus(self, data):
        request = wire.fields(data)
        correlation = request.get(1, 0)
        transaction = wire.fields(request.get(2, b""))
        address, count = transaction.get(2, 0), transaction.get(4, 0)
        if address == 0x44:
            # Match the quantized values the actual HDC3022 driver reports.
            raw_t = round((self.target.temperature + 45) / 175 * 65535)
            raw_h = round(self.target.humidity / 100 * 65535)
            temp, humid = raw_t.to_bytes(2, "big"), raw_h.to_bytes(2, "big")
            reading = temp + bytes([crc8(temp)]) + humid + bytes([crc8(humid)])
            if not transaction.get(5, 0):
                self._sensor_offset = 0
            result = reading[self._sensor_offset:self._sensor_offset + count]
            self._sensor_offset += count
            if count:
                self.sensor_reads += 1
            self.send(wire.reply(correlation, result))
        elif address == 0x60:
            self.dac_writes += 1
            self.send(wire.reply(correlation, bytes(count)))
        else:
            self.send(wire.reply(correlation, ack=False))

    def next_quantum(self):
        with self.lock:
            while self.drives:
                pin, value = self.drives.popleft()
                self.send(wire.gpio(pin, value, self.now_ns + 1000))
            if self.commands:
                shot = self.commands.popleft()
                if not self.armed:
                    shot["status"] = "not armed"
                else:
                    start = self.now_ns + 100_000
                    for name, pin, delta in zip("NESW", RUN_PINS, shot["arrivals_ns"]):
                        if name not in shot["missing"]:
                            self.send(wire.gpio(pin, True, start + delta))
                    # Longer than acquisition, bounded so no free-running 20 MHz
                    # stream is needed while the target sits idle.
                    self.clock_until_ns = start + max(shot["arrivals_ns"]) + 4_000_000
                    self.next_clock_ns = (start // 100) * 100
                    shot.update(status="fired", at_ns=start)
            if (self.busy and self.expected and len(self.expected[-1]["missing"]) == 4
                    and self.clock_until_ns <= self.now_ns):
                self.busy = False
            # Keep one short window queued ahead. At 10 MHz two windows
            # contain at most 4000 edges, below the emulator's bounded queue.
            end = self.now_ns + 2 * QUANTUM_NS
            if self.clock_until_ns > self.now_ns:
                at = self.next_clock_ns
                stop = min(end, self.clock_until_ns)
                while at + 100 <= stop:
                    cycles = min(256, (stop - at) // 100)
                    self.send(wire.pulses(CLOCK_PIN, at, [50, 50] * cycles))
                    at += cycles * 100
                self.next_clock_ns = at
        self.send(wire.grant(QUANTUM_NS))

    def run(self):
        try:
            for _ in range(200):
                if self.stop_event.is_set():
                    return
                try:
                    self.sock = socket.create_connection(("127.0.0.1", self.port), timeout=1)
                    self.sock.settimeout(None)
                    self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                    break
                except OSError:
                    self.stop_event.wait(0.05)
            else:
                raise RuntimeError("emulator co-simulation port did not open")
            drives = sum(1 << pin for pin in RUN_PINS + HIGH_PINS + (CLOCK_PIN, 35, 36, 37, 38, 40))
            watches = sum(1 << pin for pin in (STOP_PIN, TRIGGER_PIN, OSC_PIN, 12, 35, 36))
            self.send(wire.register(drives, watches, (4, 1), (0x44, 0x60)))
            while not self.stop_event.is_set():
                topic, msg = wire.receive(self.sock)
                self.now_ns = max(self.now_ns, msg.get(1, self.now_ns))
                if 5 in msg:
                    ack = wire.fields(msg[5])
                    if not ack.get(5):
                        raise RuntimeError("board registration refused: " + ack.get(6, b"").decode())
                    self.levels = ack.get(7, self.levels)
                    self.initialize()
                    self.attached.set()
                if 9 in msg:
                    self.now_ns = msg.get(1, 0)
                    self.clock_until_ns = self.next_clock_ns = 0
                    self.armed = self.busy = False
                    self.commands.clear()
                    self.drives.clear()
                    self.initialize()
                if 3 in msg:
                    level = wire.fields(msg[3])
                    pin, high = level.get(1, 0), bool(level.get(2, 0))
                    self.levels = self.levels | (1 << pin) if high else self.levels & ~(1 << pin)
                    if pin == STOP_PIN:
                        self.armed = high
                        self.armed_at_ns = self.now_ns
                        if high and self.clock_until_ns <= self.now_ns:
                            self.busy = False
                        if not high:
                            self.clear_latches()
                    elif pin == TRIGGER_PIN and high and self.armed:
                        for run_pin in RUN_PINS:
                            self.drives.append((run_pin, True))
                if 7 in msg:
                    self.bus(msg[7])
                if 2 in msg:
                    clock = wire.fields(msg[2])
                    self.events_late = clock.get(1, 0)
                    self.max_lateness_ns = clock.get(2, 0)
                    self.broadcasts_dropped = clock.get(3, 0)
                    self.dropped = clock.get(4, 0)
                if 41 in msg:
                    self.next_quantum()
                    # Yield to network clients while idle. Sensor edges use
                    # virtual time; this wait never controls their spacing.
                    if self.clock_until_ns <= self.now_ns:
                        self.stop_event.wait(0.00001)
        except (EOFError, OSError, ValueError, RuntimeError) as error:
            if not self.stop_event.is_set():
                self.error = str(error)
        finally:
            if self.sock:
                self.sock.close()
