#!/usr/bin/env python3
"""Local freETarget emulator. All state and build output stays in .runtime/."""
import argparse
import hashlib
import io
import platform
import tarfile
import tempfile
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("install", help="install pinned esp-emu into .runtime/bin")
    build = commands.add_parser("prepare", help="build an isolated firmware copy and seed disposable flash")
    build.add_argument("--build", action="store_true", help="refresh and rebuild the firmware source copy")
    run = commands.add_parser("run", help="start the virtual board and click-to-fire dashboard")
    run.add_argument("--fresh", action="store_true", help="start with a copy of the prepared factory flash")
    run.add_argument("--port", type=int, default=18888, help="dashboard port (loopback only)")
    commands.add_parser("smoke", help="exercise real firmware APIs, shot calculation and restart recovery")
    args = parser.parse_args()
    if args.command == "install":
        install()
    elif args.command == "prepare":
        print(prepare(args.build))
    elif args.command == "run":
        from runtime import serve
        serve(args.port, args.fresh)
    else:
        from smoke import main as smoke
        smoke()


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
