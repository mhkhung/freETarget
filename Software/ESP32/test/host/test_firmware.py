"""Run firmware regressions with fake serial I/O and hardware on the host.

The generated includes use the actual firmware source. Include directives are
omitted; the tests supply standard headers, platform types and I/O instead.
The JSON test uses the stream task with a command-handler stub so it can test
queued input independently of NVS and the command implementations.
"""

import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile


HERE = Path(__file__).resolve().parent
MAIN = HERE.parent.parent / "main"


def without_includes(source):
    return re.sub(r"^#include[^\n]*", "", source, flags=re.MULTILINE)


def check_command_table():
    """A command that takes a number must end its token with ':'.

    handle_json() reads the value just after the token.  Without the colon it
    reads ":1234" and gets 0: {"DOWNLOAD":size} then started a firmware
    download of 0 bytes, which failed and stopped the target.
    """
    source = (MAIN / "json.c").read_text()
    table = source[source.index("const json_message_t JSON[] = {"):]
    table = table[:table.index("\n};")]
    takes_value = {"DOWNLOAD", "FLASH"}              # Handlers that use the value they are given
    bad = []
    for show, token, value, convert, handler in re.findall(
            r'\{\s*([A-Z +]+),\s*"((?:[^"\\]|\\.)*)",\s*([^,]+),\s*([A-Z_0-9 +]+),\s*([^,]+),', table):
        name = token.strip('\\"').rstrip(':').strip('\\"')
        if not token.endswith(":") and (value.strip() != "0" or name in takes_value):
            bad.append(name)
    if bad:
        raise RuntimeError("JSON commands with a value but no ':' after the name: " + ", ".join(bad))
    print("JSON commands: every command that takes a value has ':' after its name")


def main():
    check_command_table()
    compiler = shlex.split(os.environ.get("CC", "cc"))
    flags = shlex.split(os.environ.get("CFLAGS", "-std=c11 -Wall -Wextra -g -fsanitize=address,undefined"))
    # Keep warnings as errors except for existing diagnostics in the source
    # included here (also emitted by the ESP-IDF build).
    flags += ["-Wno-error=unused-parameter", "-Wno-error=unused-function",
              "-Wno-error=incompatible-pointer-types", "-Wno-error=sign-compare"]
    with tempfile.TemporaryDirectory(prefix="freetarget-tests-") as directory:
        build = Path(directory)
        (build / "freETarget_host.h").write_text(without_includes((MAIN / "freETarget.h").read_text()))
        (build / "timed_fire_host.inc").write_text(without_includes((MAIN / "timed_fire.c").read_text()))
        source = (MAIN / "json.c").read_text()
        task = re.search(r"^void freeETarget_json\(void \*pvParameters\)\n\{.*?^\}", source, re.MULTILINE | re.DOTALL)
        if task is None:
            raise RuntimeError("Could not find the JSON stream task")
        (build / "json_stream_host.inc").write_text(task.group())
        source = (MAIN / "freETarget.c").read_text()
        reduction = re.search(r"^unsigned int reduce\(void\)\n\{.*?^\}", source, re.MULTILINE | re.DOTALL)
        if reduction is None:
            raise RuntimeError("Could not find the shot reduction function")
        (build / "reduction_host.inc").write_text(source[source.index("#define FORCE_PAPER_MOVE"):reduction.end()])
        source = (MAIN / "helpers.c").read_text()
        helpers = []
        for pattern in (r"^bool contains\(.*?^\}", r"^int http_target_type\(void\)\n\{.*?^\}",
                        r"^bool json_read_only\(const char \*command\)\n\{.*?^\}"):
            found = re.search(pattern, source, re.MULTILINE | re.DOTALL)
            if found is None:
                raise RuntimeError("Could not find " + pattern)
            helpers.append(found.group())
        (build / "helpers_host.inc").write_text("\n\n".join(helpers) + "\n")
        (build / "json_host.h").write_text(without_includes((MAIN / "json.h").read_text()))
        source = (MAIN / "json.c").read_text()
        parts = []
        for pattern in (r"^static bool good_input\(unsigned int conversion, // What kind of input is it\?.*?^\}",
                        r"^static void handle_json\(void\)\n\{.*?^\}"):
            found = re.search(pattern, source, re.MULTILINE | re.DOTALL)
            if found is None:
                raise RuntimeError("Could not find " + pattern)
            parts.append(found.group())
        (build / "json_parse_host.inc").write_text("\n\n".join(parts) + "\n")
        (build / "shot_journal_host.h").write_text(without_includes((MAIN / "shot_journal.h").read_text()))
        source = (MAIN / "shot_journal.c").read_text()
        parts = []
        for name in ("note_activity", "entry_to_shot", "shot_journal_record", "shot_journal_end_session",
                     "shot_journal_session_open", "shot_journal_tick", "shot_journal_clock_set",
                     "shot_journal_cursor_start", "shot_journal_cursor_start_all", "shot_journal_cursor_next"):
            found = re.search(r"^(?:static )?(?:void|bool) " + name + r"\(.*?^\}", source, re.MULTILINE | re.DOTALL)
            if found is None:
                raise RuntimeError("Could not find " + name)
            parts.append(found.group())
        (build / "shot_journal_host.inc").write_text("\n\n".join(parts) + "\n")
        source = (MAIN / "TCPIP/http_services.c").read_text()
        parts = re.findall(r"^#define JSON_(?:POST_MAX|BODY_MS)\s+[^\n]+", source, re.MULTILINE)
        for name in ("service_post_json", "service_get_calibration"):
            found = re.search(r"^static esp_err_t " + name + r"\(httpd_req_t \*req\)\n\{.*?^\}", source, re.MULTILINE | re.DOTALL)
            if found is None:
                raise RuntimeError("Could not find " + name)
            parts.append(found.group())
        (build / "http_services_host.inc").write_text("\n\n".join(parts) + "\n")
        source = (MAIN / "TCPIP/ws_api.c").read_text()
        found = re.search(r"^static bool send_event\(.*?^\}", source, re.MULTILINE | re.DOTALL)
        if found is None:
            raise RuntimeError("Could not find send_event")
        (build / "ws_event_host.inc").write_text(found.group() + "\n")
        source = (MAIN / "compute_hit.c").read_text()
        parts = [source[source.index("#define THRESHOLD"):source.index("static void remap_target(shot_record_t *s);")]]
        for pattern in (r"^void init_sensors\(void\)\n\{.*?^\}", r"^unsigned int compute_hit\(.*?^\}",
                        r"^static sensor_t \*const sensor_list = s;.*?^bool find_xy_3D\(.*?^\}"):
            found = re.search(pattern, source, re.MULTILINE | re.DOTALL)
            if found is None:
                raise RuntimeError("Could not find " + pattern)
            parts.append(found.group())
        # GCC accepts 1.0d for a double; the host compiler may not
        (build / "compute_hit_host.inc").write_text(re.sub(r"(\d)d\b", r"\1", "\n\n".join(parts)) + "\n")
        for name in ("test_json_stream", "test_timed_fire", "test_helpers", "test_json_text", "test_shot_journal",
                     "test_http_services", "test_ws_event", "test_compute_hit"):
            binary = build / name
            sources = [str(HERE / (name + ".c"))]
            if name == "test_shot_journal":
                sources.append(str(MAIN / "journal.c"))
            subprocess.run(compiler + flags + ["-I" + str(build), "-I" + str(MAIN), "-I" + str(MAIN / "drivers")]
                           + sources + ["-o", str(binary), "-lm"], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
