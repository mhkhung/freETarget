"""Integration checks against the emulated CPU and the real firmware APIs."""
import json
import math
import re
import shutil
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

from runtime import Runtime
from prepare import ROOT, RUNTIME
from websocket import WebSocket


def wait_until(predicate, timeout=45, message="condition did not become true"):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        result = predicate()
        if result:
            return result
        time.sleep(0.05)
    raise AssertionError(message)


def collect(ws, done="caught_up"):
    events = []
    while True:
        event = ws.receive()
        if done in event:
            return events
        events.append(event)


def main():
    runtime = Runtime(fresh=True, offset=100, state="smoke")
    try:
        runtime.start()
        wait_until(runtime.ready.is_set, message="firmware API did not start; inspect .runtime/smoke-console.log")
        assert not runtime.board.error, runtime.board.error
        page = urllib.request.urlopen(runtime.page_url, timeout=5).read().decode()
        assert "Start match" in page and "newSession" in page
        print("PASS: CPU boot, Wi-Fi, HTTP page and WebSocket handshake", flush=True)
        if shutil.which("node"):
            subprocess.run(["node", str(ROOT / "test_coordinates.js")], check=True)
            resolved = subprocess.run(["node", str(ROOT / "test_browser_connection.js"), runtime.page_url],
                                      capture_output=True, text=True, timeout=10, check=True)
            browser_url = json.loads(resolved.stdout)["url"]
            assert browser_url == runtime.ws_url, browser_url
            with WebSocket(browser_url, timeout=10) as browser:
                assert browser.receive()["hello"] == "freETarget"
            print("PASS: served athlete page JavaScript chooses and connects to the correct port without ?ws=", flush=True)
            served_page = RUNTIME / "smoke-athlete-page.html"
            served_page.write_text(page)
            for test in ("test_series.js", "test_features.js"):
                result = subprocess.run(["node", str(ROOT.parent / "ESP32/test/host" / test), str(served_page)],
                                        capture_output=True, text=True, timeout=10, check=True)
                print(result.stdout.strip(), flush=True)
        else:
            print("SKIP: install Node.js to test the athlete page's JavaScript URL resolution", flush=True)
        browser_headers = {
            "User-Agent": "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/142.0.0.0 Safari/537.36",
            "Accept": "text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,*/*;q=0.8",
            "Accept-Language": "en-CA,en-US;q=0.9,en;q=0.8",
            "Sec-CH-UA": '\"Chromium\";v=\"142\", \"Google Chrome\";v=\"142\"',
            "Sec-Fetch-Site": "same-site",
            "Sec-Fetch-Mode": "navigate",
            "Sec-Fetch-Dest": "iframe",
            "Cookie": "other_local_app=" + "x" * 3072,
        }
        request = urllib.request.Request(runtime.page_url, headers=browser_headers)
        with urllib.request.urlopen(request, timeout=5) as response:
            assert response.status == 200 and b"newSession" in response.read()
        with WebSocket(runtime.ws_url, timeout=10, headers=browser_headers) as browser:
            assert browser.receive()["hello"] == "freETarget"
        print("PASS: browser-sized HTTP and WebSocket headers, including a 3 KiB localhost cookie", flush=True)

        def get_json(command):
            url = runtime.page_url + "json?" + urllib.parse.quote(json.dumps(command), safe="")
            try:
                with urllib.request.urlopen(url, timeout=10) as response:
                    return response.status, response.read().decode(errors="replace")
            except urllib.error.HTTPError as error:
                return error.code, error.read().decode(errors="replace")

        status, text = get_json({"X_OFFSET": 3})
        assert status == 405, (status, text)
        status, text = get_json({"ECHO": 0})
        # The listing's start: the emulator's fast virtual clock can end /json's wait before it is all sent.
        assert status == 200 and '"NAME"' in text, (status, text[:200])
        refused = urllib.request.Request(runtime.page_url + "json", data=b'{"X_OFFSET":3}', method="POST",
                                         headers={"Content-Type": "text/plain"})
        try:
            urllib.request.urlopen(refused, timeout=10)
            raise AssertionError("a text/plain POST must be refused")
        except urllib.error.HTTPError as error:
            assert error.code == 415, error.code
        runtime.firmware_command({"X_OFFSET": 1.25})
        with urllib.request.urlopen(runtime.page_url + "calibration", timeout=10) as response:
            assert response.headers["Cache-Control"] == "no-store"
            assert json.load(response) == {"x_offset": 1.25, "y_offset": 0}
        with WebSocket(runtime.ws_url, timeout=10) as ws:
            assert ws.receive()["x_offset"] == 1.25
        runtime.firmware_command({"X_OFFSET": 0})
        with urllib.request.urlopen(runtime.page_url + "calibration", timeout=10) as response:
            assert json.load(response) == {"x_offset": 0, "y_offset": 0}
        print("PASS: /json runs commands as a JSON POST; GET runs only ECHO, VERSION and MFS?", flush=True)
        # The command line tool, over /json and over the PC client port. The
        # settings it needs come last in {"ECHO":0}, which takes several seconds.
        fet = [sys.executable, str(ROOT.parent / "Utilities/fet.py"), "--host", "127.0.0.1",
               "--http-port", str(runtime.http_port), "--tcp-port", str(runtime.tcp_port)]
        for extra in ([], ["--arduino"]):
            result = subprocess.run(fet + extra + ["get", "Z_OFFSET", "OTA BUILD", "LOCKED"], capture_output=True,
                                    text=True, timeout=60)
            assert result.returncode == 0 and "Z_OFFSET = 13" in result.stdout and "LOCKED = No" in result.stdout, result
        result = subprocess.run(fet + ["set", "SEND_MISS=1"], capture_output=True, text=True, timeout=60)
        assert result.returncode == 0 and "SEND_MISS: 0 -> 1" in result.stdout, result
        runtime.firmware_command({"SEND_MISS": 0})
        print("PASS: fet.py reads the whole ECHO over /json and the PC client port, and changes a setting", flush=True)
        with socket.create_connection(("127.0.0.1", runtime.http_port), timeout=10) as stalled:
            stalled.sendall(b"POST /json HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\n"
                            b"Content-Length: 32\r\n\r\n{")
            assert stalled.recv(1) == b"", "an incomplete request was not closed on timeout"
        with urllib.request.urlopen(runtime.page_url + "calibration", timeout=10) as response:
            assert response.status == 200
        print("PASS: calibration reads fresh offsets; an incomplete POST times out and HTTP stays available", flush=True)
        shot_seqs = []
        pc_client = socket.create_connection(("127.0.0.1", runtime.tcp_port), timeout=10)  # The PC client's port
        with WebSocket(runtime.ws_url, timeout=10) as ws:
            hello = ws.receive()
            assert hello["hello"] == "freETarget" and not hello["session_open"]
            ws.send({"AFTER": 0})
            assert not [e for e in collect(ws) if "shot" in e]
            ws.send({"TIME": int(time.time())})
            cases = ((0, 0, None), (-30, -20, "N"), (30, -20, "E"), (30, 20, "S"), (-30, 20, "W"))
            for x, y, first_sensor in cases:
                wait_until(runtime.board.ready_to_fire)
                generated = runtime.board.fire(x, y)
                if first_sensor:
                    arrivals = generated["arrivals_ns"]
                    assert "NESW"[arrivals.index(min(arrivals))] == first_sensor
                deadline = time.monotonic() + 10
                while time.monotonic() < deadline:
                    event = ws.receive()
                    if "shot" in event:
                        # The stock wire protocol is positive-up. Compare in
                        # the dashboard/athlete convention: negative Y up.
                        error = math.hypot(event["x"] - x, -event["y"] - y)
                        print(f"Shot ({x}, {y}) -> ({event['x']}, {-event['y']}), error {error:.3f} mm", flush=True)
                        assert error < 1.0, event
                        shot_seqs.append(event["seq"])
                        break
                else:
                    raise AssertionError("no score for the virtual sensor signals")
                # The firmware deliberately ignores triggers until ringing settles.
                time.sleep(0.3)
            session = event["session"]
        assert len(shot_seqs) == len(cases) and len(set(shot_seqs)) == len(cases)
        print("PASS: mouse-coordinate physics -> GPIO -> pulse counters -> real scoring -> journal -> WebSocket", flush=True)
        received, deadline = b"", time.monotonic() + 10
        with pc_client:
            pc_client.settimeout(1)
            while time.monotonic() < deadline:
                seqs = [int(n) for n in re.findall(rb'"shot":\s*\d+,\s*"seq":\s*(\d+)', received)]
                if set(shot_seqs) <= set(seqs):
                    break
                try:
                    received += pc_client.recv(65536)
                except socket.timeout:
                    pass
        assert set(shot_seqs) <= set(seqs), f"PC client port sent seqs {seqs}, expected {shot_seqs}"
        print("PASS: the same shots reach the PC client's TCP port (1090 on the target)", flush=True)

        with WebSocket(runtime.ws_url, timeout=10) as ws:
            ws.receive()
            ws.send({"AFTER": shot_seqs[-2]})
            replay = [e for e in collect(ws) if "shot" in e]
            assert [e["seq"] for e in replay] == shot_seqs[-1:]
        print("PASS: reconnect replays only missing sequence numbers", flush=True)

        runtime.ready.clear()
        runtime.control("reset")
        wait_until(runtime.ready.is_set, message="firmware did not recover after reset")
        with WebSocket(runtime.ws_url, timeout=10) as ws:
            ws.receive()
            ws.send({"AFTER": 0})
            replay = [e for e in collect(ws) if "shot" in e]
            assert [e["seq"] for e in replay] == shot_seqs
            ws.send({"SESSION_END": 0})
        wait_until(lambda: any(e.get("change") == "end" for e in runtime.events))
        print("PASS: reset recovers saved shots; explicit session end arrives", flush=True)

        with WebSocket(runtime.ws_url, timeout=10) as ws:
            hello = ws.receive()
            assert not hello["session_open"]
            ws.send({"AFTER": 0})
            assert not [e for e in collect(ws) if "shot" in e]
            ws.send({"HISTORY": session})
            history = [e for e in collect(ws, "history_done") if "shot" in e]
            assert [e["seq"] for e in history] == shot_seqs
        print("PASS: ended session is absent from live replay and preserved in explicit history", flush=True)
        with WebSocket(runtime.ws_url, timeout=10) as ws:
            ws.receive()
            ws.send({"SESSION": 0})
        wait_until(lambda: any(e.get("change") == "new" and e.get("session", 0) > session for e in runtime.events))
        with WebSocket(runtime.ws_url, timeout=10) as ws:
            hello = ws.receive()
            assert hello["session_open"] and hello["session"] > session
            ws.send({"AFTER": 0})
            assert not [e for e in collect(ws) if "shot" in e]
        print("PASS: explicit session start opens a new empty session", flush=True)
        with WebSocket(runtime.ws_url, timeout=10) as ws:
            ws.receive()
            ws.send({"AFTER": 0})
            collect(ws)
            for name in ("Pistol Final", "Pistol Match"):
                runtime.firmware_command({"EVENT": name})
                runtime.firmware_command({"SESSION": 0})
                runtime.firmware_command({"SESSION": 2})
                while True:
                    change = ws.receive()
                    if change.get("change") == "mode":
                        assert change["event"] == name, change
                        break
            runtime.firmware_command({"EVENT": "Pistol Practice", "SESSION": 0})
        print("PASS: another client's session changes carry the new event before any shots", flush=True)
        assert not runtime.board.dropped, "emulator dropped scheduled sensor events"
        assert not runtime.board.broadcasts_dropped, "emulator dropped board notifications"
        assert runtime.board.max_lateness_ns <= 10000, "native event lateness exceeded 10 us"
        print(f"Timing: late={runtime.board.events_late}, max lateness={runtime.board.max_lateness_ns} ns, dropped={runtime.board.dropped}", flush=True)

        # Simulate flash saved by the earlier cardinal-sensor fixture. Startup
        # must restore corner geometry and the sensor depth without
        # discarding journal contents.
        runtime.firmware_command({"ANGLE": 0, "Z_OFFSET": 0})
        runtime.close()
        runtime = Runtime(fresh=False, offset=100, state="smoke")
        runtime.start()
        wait_until(runtime.ready.is_set, message="saved flash did not reboot")
        with WebSocket(runtime.ws_url, timeout=10) as ws:
            hello = ws.receive()
            assert hello["session_open"] and hello["session"] > session
            ws.send({"HISTORY": session})
            history = [e for e in collect(ws, "history_done") if "shot" in e]
            assert [e["seq"] for e in history] == shot_seqs
        print("PASS: stopping and restarting the emulator preserves flash and session history", flush=True)
        with WebSocket(runtime.ws_url, timeout=10) as ws:
            ws.receive()
            ws.send({"AFTER": 0})
            collect(ws)
            wait_until(runtime.board.ready_to_fire)
            runtime.board.fire(15, -10)
            while True:
                event = ws.receive()
                if "shot" in event:
                    assert math.hypot(event["x"] - 15, -event["y"] + 10) < 1.0, event
                    break
        print("PASS: existing flash with the old angle and depth is migrated before accepting shots", flush=True)
        print("All emulator integration checks passed.", flush=True)
    except Exception:
        print("\nRecent firmware console:\n" + "\n".join(runtime.logs)[-6000:], flush=True)
        status = runtime.status()
        print("Board state:", json.dumps({k: v for k, v in status.items() if k not in ("events", "logs")}), flush=True)
        raise
    finally:
        runtime.close()
