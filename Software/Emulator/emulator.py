#!/usr/bin/env python3
"""Local freETarget emulator. All state and build output stays in .runtime/."""
import argparse
import hashlib
import io
import os
import platform
import signal
import subprocess
import tarfile
import tempfile
import time
import urllib.request

from prepare import RUNTIME, prepare

VERSION = "1.2.0"


def install():
    system, machine = platform.system(), platform.machine()
    targets = {("Darwin", "arm64"): "aarch64-apple-darwin",
               ("Linux", "aarch64"): "aarch64-unknown-linux-gnu",
               ("Linux", "x86_64"): "x86_64-unknown-linux-gnu"}
    if (system, machine) not in targets:
        raise RuntimeError("Use Linux x86_64/arm64 or an Apple Silicon Mac")
    asset = f"esp-emu-{VERSION}-{targets[system, machine]}.tar.gz"
    base = f"https://github.com/espressif/esp-emulator/releases/download/v{VERSION}/"
    print(f"Downloading esp-emu {VERSION} from Espressif…", flush=True)
    with urllib.request.urlopen(base + "SHA256SUMS", timeout=60) as response:
        sums = response.read().decode()
    expected = next((line.split()[0] for line in sums.splitlines() if line.split()[-1].lstrip("*") == asset), None)
    if expected is None:
        raise RuntimeError("release has no checksum for this platform")
    with urllib.request.urlopen(base + asset, timeout=60) as response:
        archive = response.read()
    if hashlib.sha256(archive).hexdigest() != expected:
        raise RuntimeError("emulator checksum mismatch")
    with tarfile.open(fileobj=io.BytesIO(archive), mode="r:gz") as bundle:
        member = next(m for m in bundle.getmembers() if m.isfile() and m.name.split("/")[-1] == "esp-emu")
        binary = bundle.extractfile(member).read()
    target = RUNTIME / "bin/esp-emu"
    target.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=target.parent, delete=False) as stream:
        stream.write(binary)
        temporary = stream.name
    from pathlib import Path
    temporary = Path(temporary)
    temporary.chmod(0o755)
    temporary.replace(target)
    print(f"Installed and SHA-256 verified: {target}")


def emulator_processes():
    """This checkout's esp-emu processes, with their parents, as {pid: (ppid, command)}."""
    binary = str(RUNTIME / "bin/esp-emu")
    listing = subprocess.run(["ps", "ax", "-o", "pid=,ppid=,command="], capture_output=True, text=True, check=True).stdout
    table = {}
    for line in listing.splitlines():
        fields = line.split(None, 2)
        if len(fields) == 3 and fields[0].isdigit() and fields[1].isdigit():
            table[int(fields[0])] = (int(fields[1]), fields[2])
    emulators = {pid: entry for pid, entry in table.items() if entry[1] == binary or entry[1].startswith(binary + " ")}
    # The emulator.py run/smoke process that owns each one, if it is still there.
    owners = {ppid: table[ppid] for ppid, _ in emulators.values()
              if ppid in table and "emulator.py" in table[ppid][1] and ppid != os.getpid()}
    return emulators, owners


def alive(pid):
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False
    except PermissionError:
        return True


def wait_gone(pids, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline and any(alive(pid) for pid in pids):
        time.sleep(0.1)
    return [pid for pid in pids if alive(pid)]


def stop(timeout=10.0):
    """Stop every emulator of this checkout, saving its flash where possible."""
    emulators, owners = emulator_processes()
    if not emulators:
        print("No emulator is running.")
        return
    for pid, (_, command) in owners.items():
        words = command.split()
        name = next((w for w in words[words.index(next(w for w in words if w.endswith("emulator.py"))) + 1:]
                     if not w.startswith("-")), "emulator")
        print(f"Stopping emulator.py {name} (pid {pid})")
        os.kill(pid, signal.SIGTERM)              # Closes its esp-emu, which saves flash
    wait_gone(list(owners), timeout)
    for pid in wait_gone(list(emulators), timeout if owners else 0):
        print(f"Stopping esp-emu (pid {pid})")
        os.kill(pid, signal.SIGINT)               # esp-emu saves --save-state flash on SIGINT
    for pid in wait_gone(list(emulators) + list(owners), timeout):
        print(f"Killing unresponsive pid {pid}; its flash may not be saved")
        os.kill(pid, signal.SIGKILL)
    wait_gone(list(emulators) + list(owners), timeout)
    print("All emulators stopped.")


def interruptible():
    """Let Ctrl-C and `emulator.py stop` (SIGTERM) run the normal clean shutdown.

    A process started in the background by a non-interactive shell inherits
    SIGINT as ignored, so restore it explicitly.
    """
    signal.signal(signal.SIGINT, signal.default_int_handler)
    signal.signal(signal.SIGTERM, signal.default_int_handler)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("install", help="install pinned esp-emu into .runtime/bin")
    build = commands.add_parser("prepare", help="build an isolated firmware copy and seed disposable flash")
    build.add_argument("--build", action="store_true", help="refresh and rebuild the firmware source copy")
    run = commands.add_parser("run", help="start the virtual board and click-to-fire dashboard")
    run.add_argument("--fresh", action="store_true", help="start with a copy of the prepared factory flash")
    run.add_argument("--port", type=int, default=18888, help="dashboard port")
    run.add_argument("--lan", action="store_true",
                     help="let other machines reach the dashboard, athlete page, WebSocket and PC client port (1090)")
    commands.add_parser("smoke", help="exercise real firmware APIs, shot calculation and restart recovery")
    halt = commands.add_parser("stop", help="stop running emulators (including background ones), saving flash")
    halt.add_argument("--timeout", type=float, default=10.0, help="seconds to wait before escalating")
    args = parser.parse_args()
    if args.command == "install":
        install()
    elif args.command == "prepare":
        print(prepare(args.build))
    elif args.command == "stop":
        stop(args.timeout)
    elif args.command == "run":
        interruptible()
        from runtime import serve
        serve(args.port, args.fresh, args.lan)
    else:
        interruptible()
        from smoke import main as smoke
        smoke()


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
