"""Tests for fet.py with fake targets (no hardware): python3 -m unittest test_fet"""
import contextlib
import io
import json
import socket
import struct
import sys
import threading
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fet  # noqa: E402

ESP32_ECHO = '''{
"NAME":           "TARGET",        "ANGLE":           45,
"SENSOR":          232.00,         "SN":              1,
"Z_OFFSET":        13,             "WIFI_SSID":       "Range \\"A\\"",
"VERSION":          "6.4.2 July 19, 2026",
"OTA BUILD":        60301,
"LOCKED":            "No"
}'''

ARDUINO_ECHO = '''
{
"NAME":"TARGET",
"ANGLE": 45,
"DOPPLER": 50.000000,
"WIFI_PWD": "****",
"Z_OFFSET": 13,
}'''


def app_image(version):
    """The start of an ESP32 app image with the given version."""
    header = bytes([fet.IMAGE_MAGIC]) + bytes(23) + bytes(8)
    desc = struct.pack("<II8s32s", fet.APP_DESC_MAGIC, 0, bytes(8), version.encode())
    return header + desc + bytes(4000)


class ParsingTests(unittest.TestCase):
    def test_esp32_echo(self):
        settings = fet.parse_echo(ESP32_ECHO)
        self.assertEqual(settings["Z_OFFSET"], "13")
        self.assertEqual(settings["SENSOR"], "232.00")
        self.assertEqual(settings["WIFI_SSID"], 'Range "A"')
        self.assertEqual(settings["OTA BUILD"], "60301")
        self.assertEqual(settings["LOCKED"], "No")

    def test_arduino_echo(self):
        settings = fet.parse_echo(ARDUINO_ECHO)
        self.assertEqual((settings["Z_OFFSET"], settings["DOPPLER"], settings["WIFI_PWD"]), ("13", "50.000000", "****"))

    def test_assignments(self):
        self.assertEqual(fet.parse_assignment("Z_OFFSET=15"), ("Z_OFFSET", 15))
        self.assertEqual(fet.parse_assignment("SENSOR=232.5"), ("SENSOR", 232.5))
        self.assertEqual(fet.parse_assignment("X_OFFSET=-0.25"), ("X_OFFSET", -0.25))
        self.assertEqual(fet.parse_assignment('WIFI_SSID="Range 1"'), ("WIFI_SSID", "Range 1"))
        self.assertEqual(fet.parse_assignment("WIFI_SSID=0123"), ("WIFI_SSID", "0123"))   # Not a number
        for bad in ("Z_OFFSET", "=3", "Z OFFSET=3", '{"A":1}=2'):
            with self.assertRaises(fet.TargetError):
                fet.parse_assignment(bad)

    def test_image_version(self):
        self.assertEqual(fet.image_version(app_image("60302")), "60302")
        with self.assertRaises(fet.TargetError):
            fet.image_version(b"\x00" * 100)
        built = fet.ESP32_BUILD / "freeETarget.bin"
        if built.is_file():
            self.assertTrue(fet.image_version(built.read_bytes()))

    def test_version_rule_matches_the_firmware(self):
        # OTA.c compares character by character and refuses an equal version
        self.assertTrue(fet.firmware_accepts("60302", "60301"))
        self.assertTrue(fet.firmware_accepts("60402-mh1", "60301"))
        self.assertTrue(fet.firmware_accepts("60301a", "60301"))
        self.assertFalse(fet.firmware_accepts("60301", "60301"))
        self.assertFalse(fet.firmware_accepts("60300", "60301"))
        self.assertFalse(fet.firmware_accepts("6030", "60301"))


class FakeHttpTarget:
    """POST /json with a tiny settings table; the lock refuses changes."""

    def __init__(self, locked=False):
        self.settings = {"NAME": "TARGET", "Z_OFFSET": 13, "SENSOR": 232.0}
        self.locked = locked
        self.requests = []
        target = self

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_POST(self):
                body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                target.requests.append((self.headers["Content-Type"], body))
                if "ECHO" in body:
                    text = "{" + ", ".join(f'"{k}": {json.dumps(v)}' for k, v in target.settings.items())
                    text += f', "LOCKED": "{"Yes" if target.locked else "No"}"' + "}"
                else:
                    if not target.locked:
                        target.settings.update(body)
                    text = ""
                data = text.encode()
                self.send_response(200)
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def close(self):
        self.server.shutdown()
        self.server.server_close()


def run_fet(*argv):
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        code = fet.main(list(argv))
    return code, out.getvalue(), err.getvalue()


class SettingsTests(unittest.TestCase):
    def setUp(self):
        self.target = FakeHttpTarget()
        self.base = ["--host", "127.0.0.1", "--http-port", str(self.target.server.server_port)]

    def tearDown(self):
        self.target.close()

    def test_get(self):
        code, out, err = run_fet(*self.base, "get", "z_offset")
        self.assertEqual((code, out), (0, "Z_OFFSET = 13\n"), err)

    def test_set_posts_json_and_reads_back(self):
        code, out, err = run_fet(*self.base, "set", "Z_OFFSET=15")
        self.assertEqual(code, 0, err)
        self.assertIn("Z_OFFSET: 13 -> 15", out)
        self.assertIn(("application/json", {"Z_OFFSET": 15}), self.target.requests)

    def test_unknown_setting_is_not_sent(self):
        code, out, err = run_fet(*self.base, "set", "Z_OFSET=15")
        self.assertEqual(code, 1)
        self.assertIn("no setting Z_OFSET", err)
        self.assertNotIn({"Z_OFSET": 15}, [body for _, body in self.target.requests])

    def test_locked_target_is_reported(self):
        self.target.locked = True
        code, out, err = run_fet(*self.base, "set", "Z_OFFSET=15")
        self.assertEqual(code, 1)
        self.assertIn("not changed: Z_OFFSET", err)
        self.assertIn("locked", err)


class FakeOtaTarget:
    """The PC client port: ECHO, then {"DOWNLOAD":n} -> DOWNLOAD_BLOCK, n bytes, PASS.

    Any input after that restarts it with the new build (as OTA_halt_process()).
    """

    def __init__(self, running="60301", accept=True, image_build="60302"):
        self.listener = socket.socket()
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen(2)
        self.running, self.accept, self.image_build, self.received = running, accept, image_build, b""
        self.waiting = False
        threading.Thread(target=self.serve, daemon=True).start()

    def serve(self):
        while True:
            try:
                conn, _ = self.listener.accept()
            except OSError:
                return
            with conn:
                self.session(conn)

    def session(self, conn):
        conn.sendall(b'{"VERSION":"6.4.2"}\r\n')                     # Greeting on connect
        buffer = b""
        while True:
            data = conn.recv(65536)
            if not data:
                return
            if self.waiting:                                         # Waiting after a download: restart
                self.waiting, self.running = False, self.image_build
                return
            buffer += data
            if b'{"ECHO":0}' in buffer:
                buffer = b""
                conn.sendall(('{"NAME": "TARGET", "VERSION": "6.4.2", "OTA BUILD": %s, "LOCKED": "No"}\r\n'
                              % self.running).encode())
            elif b'"DOWNLOAD"' in buffer and b"}" in buffer:
                size = json.loads(buffer[buffer.index(b"{"):buffer.index(b"}") + 1])["DOWNLOAD"]
                conn.sendall(b'{"DOWNLOAD_BLOCK":992}')
                image = buffer[buffer.index(b"}") + 1:]
                while len(image) < size:
                    image += conn.recv(65536)
                self.received, buffer = image, b""
                verdict = b"PASS> Cycle power to start new firmware" if self.accept else b"FAIL> esp_ota_end failed"
                conn.sendall(b'{"RESPONSE":"' + verdict + b'"}\r\n')
                self.waiting = self.accept

    def close(self):
        self.listener.close()

    @property
    def port(self):
        return self.listener.getsockname()[1]


class UpdateTests(unittest.TestCase):
    def update(self, target, image, *extra):
        self.addCleanup(target.close)
        path = Path(self.tmp) / "freeETarget.bin"
        path.write_bytes(image)
        # --arduino: the fake has no web server, so check the build over the same port
        return run_fet("--host", "127.0.0.1", "--arduino", "--tcp-port", str(target.port), "update", "--image", str(path),
                       "-y", *extra)

    def setUp(self):
        import tempfile
        self._dir = tempfile.TemporaryDirectory()
        self.tmp = self._dir.name
        fet.RESTART_WAIT = fet.RESTART_NUDGE = 0

    def tearDown(self):
        self._dir.cleanup()

    def test_streams_the_image_over_tcp(self):
        target, image = FakeOtaTarget("60301"), app_image("60302") * 3
        code, out, err = self.update(target, image)
        self.assertEqual(code, 0, err)
        self.assertEqual(target.received, image)
        self.assertIn("running build 60302", out)

    def test_no_restart_leaves_the_target_waiting(self):
        target = FakeOtaTarget("60301")
        code, out, err = self.update(target, app_image("60302"), "--no-restart")
        self.assertEqual(code, 0, err)
        self.assertTrue(target.waiting)
        self.assertIn("Restart the target", out)

    def test_refuses_a_build_the_target_would_reject(self):
        target = FakeOtaTarget("60301")
        code, out, err = self.update(target, app_image("60301"))
        self.assertEqual(code, 1)
        self.assertIn("newer than 60301", err)
        self.assertEqual(target.received, b"")

    def test_reports_a_failed_load(self):
        target = FakeOtaTarget("60301", accept=False)
        code, out, err = self.update(target, app_image("60302"))
        self.assertEqual(code, 1)
        self.assertIn("refused the image: esp_ota_end failed", err)


class FlashTests(unittest.TestCase):
    """The USB commands, with the external tools replaced."""

    def setUp(self):
        self.commands = []
        self._run, self._installed, self._which = fet.run, fet.esptool_installed, fet.shutil.which
        fet.run = lambda command, cwd=None: self.commands.append((command, cwd))
        fet.esptool_installed = lambda: True

    def tearDown(self):
        fet.run, fet.esptool_installed, fet.shutil.which = self._run, self._installed, self._which

    def test_esp32_writes_the_build_with_esptool(self):
        import tempfile
        with tempfile.TemporaryDirectory() as build:
            (Path(build) / "flash_args").write_text("--flash_mode dio\n0x10000 freeETarget.bin\n")
            code, out, err = run_fet("--serial", "/dev/cu.usbserial-1", "flash", "esp32", "--build", build)
        self.assertEqual(code, 0, err)
        command, cwd = self.commands[0]
        self.assertEqual(command[1:3], ["-m", "esptool"])
        self.assertIn("/dev/cu.usbserial-1", command)
        self.assertEqual(command[-2:], ["write_flash", "@flash_args"])
        self.assertEqual(Path(cwd), Path(build))

    def test_esp32_needs_usb_and_a_build(self):
        self.assertIn("--serial", run_fet("--host", "1.2.3.4", "flash", "esp32")[2])
        self.assertIn("flash_args not found", run_fet("--serial", "COM3", "flash", "esp32", "--build", "/nonexistent")[2])

    def test_arduino_loads_a_hex_with_avrdude(self):
        fet.shutil.which = lambda name: "/usr/bin/avrdude" if name == "avrdude" else None
        code, out, err = run_fet("--serial", "COM3", "flash", "arduino", "--hex", "fw.hex")
        self.assertEqual(code, 0, err)
        command, _ = self.commands[0]
        self.assertEqual(command[0], "/usr/bin/avrdude")
        for expected in (["-p", "atmega2560"], ["-c", "wiring"], ["-P", "COM3"], ["-U", "flash:w:fw.hex:i"]):
            index = command.index(expected[0])
            self.assertEqual(command[index:index + 2], expected)


if __name__ == "__main__":
    unittest.main()
