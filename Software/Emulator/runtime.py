"""Own emulator process lifetime, virtual board, and local dashboard."""
import collections
import dataclasses
import ipaddress
import json
import shutil
import signal
import socket
import subprocess
import threading
import time
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from board import Board
from prepare import RUNTIME, ROOT, refresh_firmware
from websocket import WebSocket
from physics import Target, speed_of_sound

# The sensors sit this far in front of the paper, as on the real target. It is
# also the firmware's Z_OFFSET default; startup keeps the two the same.
SENSOR_DEPTH_MM = 13.0


class Runtime:
    def __init__(self, fresh=False, offset=0, state="target", lan=False):
        self.http_port = 18000 + offset
        self.ws_port = 18080 + offset
        self.tcp_port = 1090 + offset         # The PC client's TCP port (its default is 1090)
        # Only the firmware's own services (athlete page, WebSocket, PC client
        # TCP) may be offered to the network. The dashboard, co-simulation and
        # control sockets can fire shots and reset the CPU: always loopback.
        # The dashboard reaches them on this computer; serve() puts it on the LAN.
        self.lan = lan
        self.firmware_bind = "0.0.0.0" if lan else "127.0.0.1"
        self.cosim_port = 18990 + offset
        self.control_port = 18991 + offset
        self.ws_url = f"ws://127.0.0.1:{self.ws_port}/ws"
        self.page_url = f"http://127.0.0.1:{self.http_port}/"
        self.flash = RUNTIME / (state + ".bin")
        self.state = state
        self.fresh = fresh
        self.board = Board(self.cosim_port, Target(depth_mm=SENSOR_DEPTH_MM))
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
        """POST to the stock synchronous /json endpoint so setup is complete before firing."""
        request = urllib.request.Request(f"http://127.0.0.1:{self.http_port}/json", method="POST",
                                         data=json.dumps(obj, separators=(",", ":")).encode(),
                                         headers={"Content-Type": "application/json"})
        with urllib.request.urlopen(request, timeout=10) as response:
            return response.read()

    def start(self):
        binary = RUNTIME / "bin/esp-emu"
        if not binary.is_file():
            raise RuntimeError("Run python3 emulator.py install first")
        factory = RUNTIME / "factory.bin"
        if not factory.is_file():
            raise RuntimeError("Run python3 emulator.py prepare first")
        for address, port in ((self.firmware_bind, self.http_port), (self.firmware_bind, self.ws_port),
                              (self.firmware_bind, self.tcp_port),
                              ("127.0.0.1", self.cosim_port), ("127.0.0.1", self.control_port)):
            with socket.socket() as probe:
                probe.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                try:
                    probe.bind((address, port))
                except OSError as error:
                    raise RuntimeError(f"port {port} is in use ({error}); is another emulator running? "
                                       "Stop it with: python3 Software/Emulator/emulator.py stop") from None
        if self.fresh or not self.flash.exists():
            shutil.copyfile(factory, self.flash)
        else:
            refresh_firmware(self.flash, factory)   # Run the current build; keep settings and saved shots
        config = RUNTIME / "settings.json"
        config.write_text('{"autoupdate":false}\n')
        (RUNTIME / (self.state + "-emulator.log")).write_text("")
        command = [str(binary), "--chip", "esp32s3", "--firmware", str(self.flash),
                   # Boot through the real ROM and 2nd-stage bootloader.
                   # Instructions per core between event steps: small enough
                   # for sub-3 us sensor edges, but esp-emu v1.2.0 faults
                   # starting CPU1 from the ROM at 16 or fewer (read16 unmapped
                   # 0x7FFFFFFF in start_other_core). 32 boots; 64 adds margin.
                   "--batch-size", "64",
                   "--elf", str(RUNTIME / "source/build/freeETarget.elf"),
                   "--config", str(config), "--save-state", "--log-color", "never",
                   "--log-file", str(RUNTIME / (self.state + "-emulator.log")),
                   "--wifi-ssid", "freetarget-emulator", "--wifi-password", "emulator123",
                   "--net", "user,net=10.0.2.0/24,restrict=yes,dns=127.0.0.1," + ",".join(
                       f"hostfwd=tcp:{self.firmware_bind}:{host}-:{guest}"
                       for host, guest in ((self.http_port, 80), (self.ws_port, 8080), (self.tcp_port, 1090))),
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
                        # Give the firmware the modelled geometry (earlier emulator
                        # NVS had ANGLE=0) without erasing saved shots or
                        # unrelated target settings.
                        self.firmware_command({"ANGLE": int(self.board.target.angle_deg),
                                               "Z_OFFSET": int(round(self.board.target.depth_mm))})
                        self.geometry_configured = True
                    ws.send({"AFTER": 0})
                    self.ready.set()
                    self.monitor_error = None
                    silence, pongs = 0, ws.pongs
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
                            # The firmware sends nothing while idle; its pong
                            # to our ping shows it is still there.
                            silence = 0 if ws.pongs != pongs else silence + 1
                            pongs = ws.pongs
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
                    "page_url": self.page_url, "ws_url": self.ws_url, "http_port": self.http_port,
                    "tcp_port": self.tcp_port, "lan": self.lan,
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


def lan_address():
    """This machine's address on its default network (no packet is sent)."""
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        try:
            probe.connect(("192.0.2.1", 9))   # TEST-NET-1: only selects the outgoing interface
            return probe.getsockname()[0]
        except OSError:
            return None


def dashboard_host_ok(host, port, lan):
    """Accept the dashboard only under a name that cannot be a DNS rebinding.

    Loopback: exactly 127.0.0.1.  LAN: any IPv4 address, typed as a number.
    A web site could point its own domain name at this computer and then send
    requests as if from the same origin, so host names are refused.
    """
    if not lan:
        return host == f"127.0.0.1:{port}"
    name, _, number = (host or "").rpartition(":")
    try:
        return number == str(port) and ipaddress.ip_address(name).version == 4
    except ValueError:
        return False


def serve(port, fresh, lan=False):
    runtime = Runtime(fresh=fresh, lan=lan)

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

        def host_refused(self):
            if dashboard_host_ok(self.headers.get("Host"), port, lan):
                return False
            self.respond({"error": "open the dashboard by IP address, e.g. "
                          f"http://{'<this computer>' if lan else '127.0.0.1'}:{port}/"}, 403)
            return True

        def do_GET(self):
            if self.host_refused():
                return
            if self.path == "/":
                self.respond((ROOT / "dashboard.html").read_bytes(), kind="text/html; charset=utf-8")
            elif self.path == "/api/status":
                self.respond(runtime.status())
            else:
                self.respond({"error": "not found"}, 404)

        def do_POST(self):
            try:
                if self.host_refused():
                    return
                origin = self.headers.get("Origin")
                if origin and origin != f"http://{self.headers['Host']}":
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
                        runtime.board.target = dataclasses.replace(runtime.board.target, temperature=temperature,
                                                                   humidity=humidity)  # Keep the geometry
                    result = {"ok": True}
                else:
                    self.respond({"error": "not found"}, 404)
                    return
                self.respond(result)
            except (ValueError, TypeError, KeyError, RuntimeError, OSError) as error:
                self.respond({"error": str(error)}, 400)

    with ThreadingHTTPServer(("0.0.0.0" if lan else "127.0.0.1", port), Handler) as server:
        try:
            runtime.start()
            host = (lan_address() or "<this computer's IP>") if lan else "127.0.0.1"
            print(f"Virtual target: http://{host}:{port}/", flush=True)
            print(f"Firmware console: {RUNTIME / 'target-console.log'}", flush=True)
            print(f"Athlete page: http://{host}:{runtime.http_port}/", flush=True)
            print(f"PC client (TCP): {host} port {runtime.tcp_port}", flush=True)
            if lan:
                print("LAN mode: the dashboard, athlete page, WebSocket and PC client port accept connections "
                      "from other machines. Anyone on this network can fire shots and reset the target.", flush=True)
            print("Stop with Ctrl-C, or from any terminal: python3 Software/Emulator/emulator.py stop", flush=True)
            server.serve_forever()
        finally:
            runtime.close()
