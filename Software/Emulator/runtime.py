"""Own emulator process lifetime, virtual board, and local dashboard."""
import collections
import json
import shutil
import signal
import socket
import subprocess
import threading
import time
import urllib.request
from urllib.parse import quote
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from board import Board
from prepare import RUNTIME, ROOT
from websocket import WebSocket
from physics import Target, speed_of_sound


class Runtime:
    def __init__(self, fresh=False, offset=0, state="target"):
        self.http_port = 18000 + offset
        self.ws_port = 18080 + offset
        self.cosim_port = 18990 + offset
        self.control_port = 18991 + offset
        self.ws_url = f"ws://127.0.0.1:{self.ws_port}/ws"
        self.page_url = f"http://127.0.0.1:{self.http_port}/"
        self.flash = RUNTIME / (state + ".bin")
        self.state = state
        self.fresh = fresh
        self.board = Board(self.cosim_port)
        self.process = None
        self.logs = collections.deque(maxlen=300)
        self.events = collections.deque(maxlen=5000)
        self.closed = threading.Event()
        self.ready = threading.Event()
        self.monitor_error = None
        self.monitor_ws = None
        self.seen = set()
        self.seen_order = collections.deque()
        self.geometry_configured = False

    def firmware_command(self, obj):
        """Use the stock synchronous endpoint so setup is complete before firing."""
        command = quote(json.dumps(obj, separators=(",", ":")), safe="")
        with urllib.request.urlopen(f"http://127.0.0.1:{self.http_port}/json?{command}", timeout=10) as response:
            return response.read()

    def start(self):
        binary = RUNTIME / "bin/esp-emu"
        if not binary.is_file():
            raise RuntimeError("Run python3 emulator.py install first")
        factory = RUNTIME / "factory.bin"
        if not factory.is_file():
            raise RuntimeError("Run python3 emulator.py prepare first")
        for port in (self.http_port, self.ws_port, self.cosim_port, self.control_port):
            with socket.socket() as probe:
                probe.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                probe.bind(("127.0.0.1", port))
        if self.fresh or not self.flash.exists():
            shutil.copyfile(factory, self.flash)
        config = RUNTIME / "settings.json"
        config.write_text('{"autoupdate":false}\n')
        (RUNTIME / (self.state + "-emulator.log")).write_text("")
        command = [str(binary), "--chip", "esp32s3", "--firmware", str(self.flash),
                   "--skip-rom", "--skip-bootloader",
                   "--batch-size", "16",
                   "--elf", str(RUNTIME / "source/build/freeETarget.elf"),
                   "--config", str(config), "--save-state", "--log-color", "never",
                   "--log-file", str(RUNTIME / (self.state + "-emulator.log")),
                   "--wifi-ssid", "freetarget-emulator", "--wifi-password", "emulator123",
                   "--net", f"user,net=10.0.2.0/24,restrict=yes,dns=127.0.0.1,hostfwd=tcp:127.0.0.1:{self.http_port}-:80,hostfwd=tcp:127.0.0.1:{self.ws_port}-:8080",
                   "--cosim-tcp", f"127.0.0.1:{self.cosim_port}",
                   "--control-tcp", f"127.0.0.1:{self.control_port}"]
        self.process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                        stdin=subprocess.DEVNULL, text=True, bufsize=1)
        threading.Thread(target=self.read_logs, daemon=True).start()
        self.board.start()
        deadline = time.monotonic() + 15
        while not self.board.attached.wait(0.05):
            if self.board.error or self.process.poll() is not None or time.monotonic() > deadline:
                self.close()
                raise RuntimeError(self.board.error or "emulator did not attach: " + "\n".join(self.logs)[-4000:])
        # Registration holds virtual time. Reset once with the board attached
        # so early ADC/GPIO initialization cannot race model connection. No
        # debugger listener is needed (esp-emu's GDB listener binds all NICs).
        self.control("reset")
        threading.Thread(target=self.monitor, daemon=True).start()
        return self

    def read_logs(self):
        with (RUNTIME / (self.state + "-console.log")).open("w") as logfile:
            for line in self.process.stdout:
                self.logs.append(line.rstrip())
                logfile.write(line)
                logfile.flush()

    def monitor(self):
        while not self.closed.is_set():
            try:
                with WebSocket(self.ws_url, timeout=2) as ws:
                    self.monitor_ws = ws
                    hello = ws.receive()
                    self.events.append(hello)
                    if not self.geometry_configured:
                        # Upgrade earlier emulator NVS (ANGLE=0) without erasing
                        # saved shots or unrelated target settings.
                        self.firmware_command({"ANGLE": int(self.board.target.angle_deg)})
                        self.geometry_configured = True
                    ws.send({"AFTER": 0})
                    self.ready.set()
                    self.monitor_error = None
                    silence = 0
                    while not self.closed.is_set():
                        try:
                            event = ws.receive()
                            silence = 0
                            key = (event.get("session"), event.get("seq"), "shot" if "shot" in event else event.get("change"))
                            if "seq" not in event or key not in self.seen:
                                self.events.append(event)
                                if "shot" in event and "time" in event:
                                    with self.board.lock:
                                        candidates = [shot for shot in self.board.expected
                                                      if shot["status"] == "fired" and "result" not in shot
                                                      and abs(shot["at_ns"] / 1e6 - event["time"]) < 100]
                                        if candidates:
                                            min(candidates, key=lambda shot: abs(shot["at_ns"] / 1e6 - event["time"]))["result"] = {
                                                **event, "y": -event["y"]}  # Public target coordinates, negative Y up.
                                if "seq" in event:
                                    self.seen.add(key)
                                    self.seen_order.append(key)
                                    if len(self.seen_order) > 5000:
                                        self.seen.discard(self.seen_order.popleft())
                        except socket.timeout:
                            silence += 1
                            if silence >= 3:
                                raise ConnectionError("firmware heartbeat timed out")
                            ws.frame(9, b"lab")
            except (OSError, EOFError, ValueError) as error:
                self.ready.clear()
                self.monitor_error = str(error)
                self.closed.wait(0.2)
            finally:
                self.monitor_ws = None

    def command(self, obj):
        if not self.ready.is_set():
            raise RuntimeError("firmware API is not ready")
        with WebSocket(self.ws_url, timeout=5) as ws:
            ws.receive()
            ws.send(obj)

    def control(self, text):
        with socket.create_connection(("127.0.0.1", self.control_port), timeout=5) as sock:
            sock.sendall((text + "\n").encode())
            data = bytearray()
            while not data.endswith(b"\n"):
                block = sock.recv(4096)
                if not block:
                    raise EOFError("control channel closed")
                data.extend(block)
        result = data.decode().strip()
        if not result.startswith("ok"):
            raise RuntimeError(result)
        return result

    def status(self):
        with self.board.lock:
            return {"ready": self.ready.is_set() and not self.board.error,
                    "error": self.board.error or self.monitor_error,
                    "time_s": self.board.now_ns / 1e9, "armed": self.board.ready_to_fire(),
                    "sensor_reads": self.board.sensor_reads, "dac_writes": self.board.dac_writes,
                    "late_events": self.board.events_late, "dropped_events": self.board.dropped,
                    "max_lateness_ns": self.board.max_lateness_ns, "broadcasts_dropped": self.board.broadcasts_dropped,
                    "speed_m_s": speed_of_sound(self.board.target.temperature, self.board.target.humidity) * 1000,
                    "sensor_positions": self.board.target.sensors(),
                    "sensor_diameter_mm": self.board.target.diameter_mm,
                    "page_url": self.page_url, "ws_url": self.ws_url,
                    "expected": list(self.board.expected), "events": list(self.events), "logs": list(self.logs)[-60:]}

    def close(self):
        self.closed.set()
        if self.monitor_ws:
            self.monitor_ws.close()
        self.board.close()
        if self.process and self.process.poll() is None:
            # esp-emu's Ctrl-C handler writes --save-state before exiting.
            self.process.send_signal(signal.SIGINT)
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()


def serve(port, fresh):
    runtime = Runtime(fresh=fresh)

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def respond(self, data, status=200, kind="application/json"):
            if not isinstance(data, bytes):
                data = json.dumps(data, allow_nan=False).encode()
            self.send_response(status)
            self.send_header("Content-Type", kind)
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(data)

        def do_GET(self):
            if self.headers.get("Host") != f"127.0.0.1:{port}":
                self.respond({"error": "use the loopback dashboard URL"}, 403)
                return
            if self.path == "/":
                self.respond((ROOT / "dashboard.html").read_bytes(), kind="text/html; charset=utf-8")
            elif self.path == "/api/status":
                self.respond(runtime.status())
            else:
                self.respond({"error": "not found"}, 404)

        def do_POST(self):
            try:
                if self.headers.get("Host") != f"127.0.0.1:{port}":
                    self.respond({"error": "use the loopback dashboard URL"}, 403)
                    return
                origin = self.headers.get("Origin")
                if origin and origin != f"http://127.0.0.1:{port}":
                    self.respond({"error": "cross-origin control request refused"}, 403)
                    return
                if self.headers.get("Content-Type", "").split(";")[0] != "application/json":
                    raise ValueError("Content-Type must be application/json")
                length = int(self.headers.get("Content-Length", "0"))
                if not 0 < length <= 4096:
                    raise ValueError("invalid request size")
                obj = json.loads(self.rfile.read(length))
                if not isinstance(obj, dict):
                    raise ValueError("request body must be an object")
                if self.path == "/api/shot":
                    if not runtime.ready.is_set():
                        raise RuntimeError("firmware is still booting")
                    result = runtime.board.fire(float(obj["x"]), float(obj["y"]), obj.get("missing", ()))
                elif self.path == "/api/command":
                    runtime.command(obj)
                    result = {"ok": True}
                elif self.path == "/api/reset":
                    runtime.ready.clear()
                    if runtime.monitor_ws:
                        runtime.monitor_ws.close()
                    result = {"result": runtime.control("reset")}
                elif self.path == "/api/environment":
                    temperature, humidity = float(obj["temperature"]), float(obj["humidity"])
                    speed_of_sound(temperature, humidity)
                    with runtime.board.lock:
                        runtime.board.target = Target(temperature=temperature, humidity=humidity)
                    result = {"ok": True}
                else:
                    self.respond({"error": "not found"}, 404)
                    return
                self.respond(result)
            except (ValueError, TypeError, KeyError, RuntimeError, OSError) as error:
                self.respond({"error": str(error)}, 400)

    with ThreadingHTTPServer(("127.0.0.1", port), Handler) as server:
        try:
            runtime.start()
            print(f"Virtual target: http://127.0.0.1:{port}/", flush=True)
            print(f"Firmware console: {RUNTIME / 'target-console.log'}", flush=True)
            server.serve_forever()
        finally:
            runtime.close()
