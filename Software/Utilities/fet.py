#!/usr/bin/env python3
"""freETarget settings and firmware from the command line.

Read and change target settings, and load new firmware, without the PC client.
Works with the ESP32 (V5/V6) and the Arduino targets.

Connect over Wi-Fi with --host (the target's address) or over USB with --serial
(the port name, for example /dev/cu.usbserial-0001 or COM3).

  fet.py --host 192.168.10.9 get Z_OFFSET SENSOR
  fet.py --host 192.168.10.9 set Z_OFFSET=15
  fet.py --serial COM3 send '{"ECHO":0}'
  fet.py --host 192.168.10.9 update                       ESP32 firmware over Wi-Fi
  fet.py --serial /dev/cu.usbserial-0001 flash esp32      ESP32, everything, over USB
  fet.py --serial /dev/cu.usbmodem1101 flash arduino      Arduino, over USB

Commands over Wi-Fi use the ESP32 web server (/json); with --arduino they use
the PC client port (1090) instead, as the Arduino's ESP-01 has no web server.
USB needs pyserial (pip install pyserial); "flash esp32" needs esptool
(pip install esptool) and "flash arduino" avrdude (installed with the Arduino
IDE or arduino-cli).
"""
import argparse
import glob
import json
import os
import re
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
SOFTWARE = HERE.parent
ESP32_BUILD = SOFTWARE / "ESP32" / "build"
ARDUINO_SKETCH = SOFTWARE / "Arduino" / "freETarget"
BAUD = 115200
RESTART_WAIT = 5                   # Seconds before reconnecting to a restarting target
RESTART_NUDGE = 2                  # Seconds between a loaded image and the input that restarts the target

IMAGE_MAGIC = 0xE9                 # First byte of an ESP32 app image
APP_DESC_MAGIC = 0xABCD5432        # esp_app_desc_t.magic_word, right after the image and segment headers
APP_DESC_OFFSET = 24 + 8           # esp_image_header_t + esp_image_segment_header_t
VERSION_OFFSET = APP_DESC_OFFSET + 16


class TargetError(Exception):
    pass


# ---------------------------------------------------------------------------
# Talking to the target

class Link:
    """A JSON command connection: write a command, collect what comes back."""

    def command(self, text, quiet=1.0, limit=15.0):
        raise NotImplementedError

    def close(self):
        pass


class HttpLink(Link):
    """ESP32 web server: POST /json returns the command's output."""

    def __init__(self, host, port):
        self.url = f"http://{host}:{port}/json"

    def command(self, text, quiet=1.0, limit=15.0):
        request = urllib.request.Request(self.url, data=text.encode(), method="POST",
                                         headers={"Content-Type": "application/json"})
        try:
            with urllib.request.urlopen(request, timeout=limit) as response:
                return response.read().decode(errors="replace")
        except urllib.error.HTTPError as error:
            raise TargetError(f"{self.url}: HTTP {error.code} {error.reason}") from None
        except (urllib.error.URLError, OSError) as error:
            raise TargetError(f"cannot reach {self.url}: {getattr(error, 'reason', error)}") from None


class StreamLink(Link):
    """The PC client port (TCP) or USB serial: everything the target says."""

    def __init__(self, read, write, close):
        self._read, self._write, self._close = read, write, close
        self.drain(0.8)                     # The target may greet a new connection

    def drain(self, quiet, limit=15.0, until=None):
        """Read until nothing arrives for `quiet` seconds (or `until` appears)."""
        data, start, last = b"", time.monotonic(), time.monotonic()
        while time.monotonic() - start < limit:
            chunk = self._read()
            if chunk:
                data += chunk
                last = time.monotonic()
                if until is not None and re.search(until, data):
                    break
            elif time.monotonic() - last >= quiet:
                break
        return data

    def write(self, data):
        self._write(data)

    def command(self, text, quiet=1.0, limit=15.0):
        self.drain(0.1, limit=0.5)          # Anything left over (a shot, a keep-alive)
        self.write(text.encode())
        return self.drain(quiet, limit).decode(errors="replace")

    def close(self):
        self._close()


def tcp_link(host, port):
    try:
        sock = socket.create_connection((host, port), timeout=10)
    except OSError as error:
        raise TargetError(f"cannot connect to {host}:{port}: {error}") from None
    sock.settimeout(0.1)

    def read():
        try:
            data = sock.recv(4096)
        except socket.timeout:
            return b""
        if not data:
            raise TargetError("the target closed the connection")
        return data

    def write(data):
        sock.settimeout(60)                 # The target may hold us back while it writes flash
        try:
            sock.sendall(data)
        except socket.timeout:
            raise TargetError("the target stopped taking data") from None
        finally:
            sock.settimeout(0.1)

    link = StreamLink(read, write, sock.close)
    link.socket = sock
    return link


def serial_link(port):
    try:
        import serial                       # pyserial
    except ImportError:
        raise TargetError("USB needs pyserial: pip install pyserial") from None
    try:
        device = serial.Serial(port, BAUD, timeout=0.1)
    except serial.SerialException as error:
        raise TargetError(f"cannot open {port}: {error}") from None
    return StreamLink(lambda: device.read(4096), device.write, device.close)


def open_link(args, stream=False):
    """A link for commands; stream=True for a raw byte stream (firmware)."""
    if args.serial:
        return serial_link(args.serial)
    if not args.host:
        raise TargetError("say how to reach the target: --host ADDRESS or --serial PORT")
    if stream or args.arduino:
        return tcp_link(args.host, args.tcp_port)
    return HttpLink(args.host, args.http_port)


# ---------------------------------------------------------------------------
# Settings

SETTING = re.compile(r'"([A-Za-z0-9_? ]+)"\s*:\s*("(?:[^"\\]|\\.)*"|-?[0-9][0-9.]*)')


def parse_echo(text):
    """{"ECHO":0} output -> {name: value} in the order shown (text unquoted)."""
    settings = {}
    for name, value in SETTING.findall(text):
        settings[name] = json.loads(value) if value.startswith('"') else value
    return settings


def parse_assignment(text):
    """NAME=VALUE -> (NAME, value as a JSON number when it is one)."""
    name, sep, value = text.partition("=")
    name = name.strip()
    if not sep or not re.fullmatch(r"[A-Za-z0-9_]+", name):
        raise TargetError(f"expected NAME=VALUE, not {text!r}")
    value = value.strip()
    if re.fullmatch(r"-?(0|[1-9][0-9]*)", value):
        return name, int(value)
    if re.fullmatch(r"-?[0-9]+\.[0-9]*|-?\.[0-9]+", value):
        return name, float(value)
    if len(value) >= 2 and value[0] == value[-1] == '"':
        value = value[1:-1]
    return name, value


def read_settings(link):
    settings = parse_echo(link.command('{"ECHO":0}', quiet=1.5))
    if not settings:
        raise TargetError("the target did not answer {\"ECHO\":0}")
    return settings


def do_get(args):
    link = open_link(args)
    try:
        settings = read_settings(link)
    finally:
        link.close()
    names = [name.upper() for name in args.names] or list(settings)
    missing = [name for name in names if name not in settings]
    for name in names:
        if name in settings:
            print(f"{name} = {settings[name]}")
    if missing:
        raise TargetError("not reported by the target: " + ", ".join(missing))


def do_set(args):
    changes = dict(parse_assignment(item) for item in args.assignments)
    changes = {name.upper(): value for name, value in changes.items()}
    link = open_link(args)
    try:
        before = read_settings(link)
        unknown = [name for name in changes if name not in before]
        if unknown and not args.force:
            raise TargetError("the target has no setting " + ", ".join(unknown) + " (--force sends it anyway)")
        output = link.command(json.dumps(changes, separators=(",", ":")))
        after = read_settings(link)
    finally:
        link.close()
    unchanged = []
    for name, wanted in changes.items():
        old, new = before.get(name, "?"), after.get(name, "?")
        print(f"{name}: {old} -> {new}")
        if not same_value(new, wanted):
            unchanged.append(name)
    if unchanged:
        hint = " The target is locked: unlock it first." if str(after.get("LOCKED", "")).lower() == "yes" else ""
        if output.strip():
            hint += "\nTarget said: " + output.strip()[:400]
        raise TargetError("not changed: " + ", ".join(unchanged) + "." + hint)


def same_value(shown, wanted):
    if isinstance(wanted, (int, float)):
        try:
            return abs(float(shown) - float(wanted)) < 1e-3
        except ValueError:
            return False
    return str(shown) == str(wanted) or set(str(shown)) == {"*"}    # Passwords are shown as ****


def do_send(args):
    try:
        json.loads(args.json)
    except ValueError as error:
        raise TargetError(f"not JSON: {error}") from None
    link = open_link(args)
    try:
        print(link.command(args.json, quiet=args.wait).strip())
    finally:
        link.close()


# ---------------------------------------------------------------------------
# ESP32 firmware over the air (Wi-Fi) or the USB console, keeping settings and shots

def image_version(image):
    """The app version (CONFIG_APP_PROJECT_VER) stored in an ESP32 app image."""
    if len(image) < VERSION_OFFSET + 32 or image[0] != IMAGE_MAGIC:
        raise TargetError("not an ESP32 app image (freeETarget.bin)")
    magic, = struct.unpack_from("<I", image, APP_DESC_OFFSET)
    if magic != APP_DESC_MAGIC:
        raise TargetError("the image has no app description; is this freeETarget.bin?")
    return image[VERSION_OFFSET:VERSION_OFFSET + 32].split(b"\0")[0].decode(errors="replace")


def firmware_accepts(new, running):
    """The target's own rule (OTA.c): only a version that compares higher, character by character."""
    for i, ch in enumerate(new):
        current = running[i] if i < len(running) else "\0"
        if ch < current:
            return False
        if ch > current:
            return True
    return False


def do_update(args):
    path = Path(args.image) if args.image else ESP32_BUILD / "freeETarget.bin"
    try:
        image = path.read_bytes()
    except OSError as error:
        raise TargetError(f"cannot read {path}: {error}") from None
    new = image_version(image)

    link = open_link(args, stream=True)
    try:
        settings = read_settings(link)
        running = str(settings.get("OTA BUILD", ""))
        print(f"Target: {settings.get('NAME', '?')}, firmware {settings.get('VERSION', '?')}, build {running or '?'}")
        print(f"Image:  {path} ({len(image)} bytes), build {new}")
        if running and not firmware_accepts(new, running):
            raise TargetError(f"the target only accepts a build newer than {running}, and stops if sent another. "
                              "Raise CONFIG_APP_PROJECT_VER and rebuild, or use 'flash esp32' over USB.")
        if str(settings.get("LOCKED", "")).lower() == "yes":
            raise TargetError("the target is locked: unlock it first")
        if not args.yes and input("Load it? [y/N] ").strip().lower() not in ("y", "yes"):
            raise TargetError("not loaded")

        link.drain(0.2, limit=1)
        link.write(json.dumps({"DOWNLOAD": len(image)}).encode())
        reply = link.drain(2, limit=15, until=rb'"DOWNLOAD_BLOCK"\s*:\s*\d+|FAIL>[^"]*')
        block = re.search(rb'"DOWNLOAD_BLOCK"\s*:\s*(\d+)', reply)
        if not block:
            raise TargetError("the target did not start the download: " + reply.decode(errors="replace").strip()[-300:])
        block = int(block.group(1))
        send_image(link, image, block, args.rate)
        reply = link.drain(60, limit=300, until=rb'(PASS|FAIL)>[^"]*')
        result = re.search(rb'(PASS|FAIL)>([^"]*)', reply)
        if result and result.group(1) == b"FAIL":
            raise TargetError("the target refused the image: " + result.group(2).decode(errors="replace").strip())
        if args.no_restart:
            print("Loaded. Restart the target to run it." if result else
                  "No reply after the download (older firmware only reports it on USB). Restart the target and check its build.")
            return
        # Any input restarts a target waiting after a download, but it first
        # throws away what has already arrived: give it time to get there.
        for _ in range(2):
            time.sleep(RESTART_NUDGE)
            link.write(b"\r")
    finally:
        link.close()
    print("Loaded. Restarting the target ..." if result else "No reply after the download; restarting the target to check ...")
    running = wait_for_build(args)
    if running != new:
        raise TargetError(f"the target restarted with build {running}, not {new}")
    print(f"The target is running build {new}.")


def wait_for_build(args, limit=120):
    """Reconnect after a restart and read the running build."""
    time.sleep(RESTART_WAIT)
    deadline, last = time.monotonic() + limit, None
    while time.monotonic() < deadline:
        try:
            link = open_link(args)
            try:
                return str(read_settings(link).get("OTA BUILD", "?"))
            finally:
                link.close()
        except TargetError as error:
            last = error
            time.sleep(3)
    raise TargetError(f"the target did not come back after restarting: {last}")


def send_image(link, image, block, rate_kb_s):
    start, sent, shown = time.monotonic(), 0, -1
    while sent < len(image):
        chunk = image[sent:sent + block]
        link.write(chunk)
        sent += len(chunk)
        if rate_kb_s:                       # Older firmware drops what it has no room for
            delay = sent / (rate_kb_s * 1024) - (time.monotonic() - start)
            if delay > 0:
                time.sleep(delay)
        percent = sent * 100 // len(image)
        if percent != shown:
            shown = percent
            end = "\n" if sent == len(image) else ""
            print(f"\r  {percent:3d}%  {sent // 1024} of {len(image) // 1024} KB", end=end, file=sys.stderr, flush=True)
    print(f"  Sent in {time.monotonic() - start:.0f} s", file=sys.stderr)


# ---------------------------------------------------------------------------
# Flashing over USB (replaces everything but settings and saved shots)

def do_flash_esp32(args):
    if not args.serial:
        raise TargetError("flash esp32 works over USB: give --serial PORT (or use 'update' over Wi-Fi)")
    build = Path(args.build) if args.build else ESP32_BUILD
    if not (build / "flash_args").is_file():
        raise TargetError(f"{build}/flash_args not found: build the firmware first (idf.py build)")
    command = [sys.executable, "-m", "esptool", "--chip", "esp32s3", "-p", args.serial, "-b", "460800",
               "--before", "default_reset", "--after", "hard_reset", "write_flash", "@flash_args"]
    if not esptool_installed():
        raise TargetError("esptool is not installed: pip install esptool")
    run(command, cwd=build)


def find_avrdude():
    """avrdude and its configuration, from PATH or an Arduino IDE / arduino-cli install."""
    found = shutil.which("avrdude")
    if found:
        return found, None
    homes = [Path.home() / "Library/Arduino15", Path.home() / ".arduino15",
             Path(os.environ.get("LOCALAPPDATA", "")) / "Arduino15"]
    for home in homes:
        for binary in sorted(glob.glob(str(home / "packages/arduino/tools/avrdude/*/bin/avrdude*")), reverse=True):
            conf = Path(binary).parent.parent / "etc/avrdude.conf"
            return binary, str(conf) if conf.is_file() else None
    raise TargetError("avrdude not found: install the Arduino IDE or arduino-cli (arduino-cli core install arduino:avr)")


def do_flash_arduino(args):
    if not args.serial:
        raise TargetError("flash arduino works over USB: give --serial PORT")
    with tempfile.TemporaryDirectory(prefix="fet-") as scratch:
        hex_file = args.hex
        if not hex_file:
            if not shutil.which("arduino-cli"):
                raise TargetError("give --hex FILE, or install arduino-cli to build the sketch")
            print(f"Building {ARDUINO_SKETCH} ...")
            run(["arduino-cli", "compile", "-b", "arduino:avr:mega", "--output-dir", scratch, str(ARDUINO_SKETCH)])
            hex_file = str(Path(scratch) / "freETarget.ino.hex")
        avrdude, conf = find_avrdude()
        command = [avrdude] + (["-C", conf] if conf else []) + [
            "-p", "atmega2560", "-c", "wiring", "-P", args.serial, "-b", "115200", "-D", "-U", f"flash:w:{hex_file}:i"]
        run(command)


def run(command, cwd=None):
    print("$ " + " ".join(command))
    try:
        result = subprocess.run(command, cwd=cwd)
    except FileNotFoundError:
        raise TargetError(f"{command[0]} not found") from None
    if result.returncode != 0:
        raise TargetError(f"{Path(command[0]).name} failed (exit {result.returncode})")


def esptool_installed():
    return subprocess.run([sys.executable, "-c", "import esptool"], capture_output=True).returncode == 0


# ---------------------------------------------------------------------------

def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                     epilog=__doc__.split("\n\n", 1)[1], formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", help="target address on Wi-Fi (192.168.10.9 when you join its own network)")
    parser.add_argument("--serial", help="USB serial port")
    parser.add_argument("--arduino", action="store_true", help="the target is an Arduino (Wi-Fi commands use port 1090)")
    parser.add_argument("--http-port", type=int, default=80, help=argparse.SUPPRESS)
    parser.add_argument("--tcp-port", type=int, default=1090, help=argparse.SUPPRESS)
    commands = parser.add_subparsers(dest="command", required=True)

    get = commands.add_parser("get", help="show settings (all, or the ones named)")
    get.add_argument("names", nargs="*", metavar="NAME")
    get.set_defaults(handler=do_get)

    put = commands.add_parser("set", help="change settings, then read them back")
    put.add_argument("assignments", nargs="+", metavar="NAME=VALUE")
    put.add_argument("--force", action="store_true", help="send names the target does not report")
    put.set_defaults(handler=do_set)

    send = commands.add_parser("send", help="send one JSON command and show the reply")
    send.add_argument("json")
    send.add_argument("--wait", type=float, default=1.5, help="seconds of quiet that end the reply")
    send.set_defaults(handler=do_send)

    update = commands.add_parser("update", help="load new ESP32 firmware over Wi-Fi or USB, keeping settings and shots")
    update.add_argument("--image", help="app image (default: Software/ESP32/build/freeETarget.bin)")
    update.add_argument("--rate", type=float, default=0,
                        help="limit to KB/s (2 over Wi-Fi for firmware before 6.3.5; 6.3.5 to 6.4.2 cannot load at all)")
    update.add_argument("-y", "--yes", action="store_true", help="do not ask before loading")
    update.add_argument("--no-restart", action="store_true", help="leave the target waiting after loading")
    update.set_defaults(handler=do_update)

    flash = commands.add_parser("flash", help="write firmware over USB")
    board = flash.add_subparsers(dest="board", required=True)
    esp32 = board.add_parser("esp32", help="bootloader, partition table and app (settings and shots are kept)")
    esp32.add_argument("--build", help="ESP-IDF build folder (default: Software/ESP32/build)")
    esp32.set_defaults(handler=do_flash_esp32)
    arduino = board.add_parser("arduino", help="the Arduino Mega sketch (builds it unless --hex is given)")
    arduino.add_argument("--hex", help="Intel hex file to load")
    arduino.set_defaults(handler=do_flash_arduino)

    args = parser.parse_args(argv)
    try:
        args.handler(args)
    except TargetError as error:
        print(f"fet: {error}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        return 130
    return 0


if __name__ == "__main__":
    sys.exit(main())
