"""Prepare disposable flash from the current ESP-IDF build; never flash a device."""
import csv
import json
import re
import subprocess
import shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parent
FIRMWARE = ROOT.parent / "ESP32"
RUNTIME = ROOT / ".runtime"
IDF_IMAGE = "espressif/idf:v5.3.1"


def prepare(build=False):
    RUNTIME.mkdir(parents=True, exist_ok=True)
    source = RUNTIME / "source"
    if build or not (source / "build/freeETarget.bin").exists():
        # Refresh sources, including deletions, while retaining expensive build
        # and component caches. Never modify the production firmware checkout.
        if source.exists():
            for child in source.iterdir():
                if child.name not in ("build", "managed_components"):
                    if child.is_dir() and not child.is_symlink():
                        shutil.rmtree(child)
                    else:
                        child.unlink()
        shutil.copytree(FIRMWARE, source, dirs_exist_ok=True,
                        ignore=shutil.ignore_patterns("build", "OTA_binary", ".vscode", ".devcontainer", "test", "__pycache__"))
        shutil.copyfile(ROOT / "firmware/board_boot.c", source / "main/emulator_board_boot.c")
        # Browser headers (including localhost cookies shared across ports)
        # exceed the hardware build's 512-byte limit. Keep this override local
        # to the emulator snapshot, for both HTTP and WebSocket handshakes.
        sdkconfig = source / "sdkconfig"
        config, count = re.subn(r"^CONFIG_HTTPD_MAX_REQ_HDR_LEN=.*$",
                                "CONFIG_HTTPD_MAX_REQ_HDR_LEN=8192",
                                sdkconfig.read_text(), flags=re.MULTILINE)
        if count != 1:
            raise RuntimeError("HTTP header limit setting missing or duplicated in sdkconfig")
        sdkconfig.write_text(config)
        # Adapt only the copied page to the emulator's forwarded ports. A bare
        # HTTP URL must work just as the dashboard's embedded page does.
        page = source / "main/HTML/app.html"
        html, count = re.subn(r"^function wsUrl\(\) \{.*?^\}",
                              lambda match: (ROOT / "firmware/browser_connection.js").read_text().strip(),
                              page.read_text(), flags=re.MULTILINE | re.DOTALL)
        if count != 1:
            raise RuntimeError("athlete page WebSocket URL function missing or duplicated")
        page.write_text(html)
        cmake = source / "main/CMakeLists.txt"
        cmake.write_text(cmake.read_text() + '\ntarget_compile_definitions(${COMPONENT_LIB} PRIVATE FT_EMULATOR_BUILD=1)\n'
                         + 'target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=POST_counters")\n')
        subprocess.run(["docker", "run", "--rm", "-v", f"{source}:/project", "-w", "/project",
                        IDF_IMAGE, "idf.py", "build"], check=True)
    build_dir = source / "build"
    args_file = build_dir / "flasher_args.json"
    if not args_file.exists():
        raise RuntimeError("Build firmware first: python3 emulator.py prepare --build")
    with (RUNTIME / "nvs.csv").open("w", newline="") as stream:
        rows = csv.writer(stream)
        rows.writerow(("key", "type", "encoding", "value"))
        rows.writerow(("freETarget", "namespace", "", ""))
        settings = {"NONVOL_INIT": 0xabcd, "PS_VERSION": 18, "SERIAL_NO": 1,
                    "SENSOR_ANGLE": 45, "Z_OFFSET": 0, "PCNT_LATENCY": 0,
                    "PAPER_TIME": 0, "WIFI_DHCP": 1, "WIFI_RESET": 0,
                    "WIFI_SSID": "freetarget-emulator", "WIFI_PWD": "emulator123",
                    "TARGET_NAME": "freETarget.targets.AirPistol", "EVENT": "Pistol Practice"}
        for key, value in settings.items():
            rows.writerow((key, "data", "string" if isinstance(value, str) else "i32", value))
    subprocess.run(["docker", "run", "--rm", "--network", "none", "-v", f"{RUNTIME}:/emulator",
                    IDF_IMAGE, "python", "/opt/esp/idf/components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py",
                    "generate", "/emulator/nvs.csv", "/emulator/nvs.bin", "0x4000"], check=True)
    image = bytearray(b"\xff") * (8 * 1024 * 1024)
    for address, filename in json.loads(args_file.read_text())["flash_files"].items():
        data = (build_dir / filename).read_bytes()
        start = int(address, 0)
        if start < 0 or start + len(data) > len(image):
            raise ValueError("firmware partition outside 8 MiB flash")
        image[start:start + len(data)] = data
    nvs = (RUNTIME / "nvs.bin").read_bytes()
    if len(nvs) != 0x4000:
        raise ValueError("wrong NVS partition size")
    image[0x9000:0xd000] = nvs
    (RUNTIME / "factory.bin").write_bytes(image)
    return RUNTIME / "factory.bin"
