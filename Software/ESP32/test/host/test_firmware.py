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


def main():
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
        for name in ("test_json_stream", "test_timed_fire"):
            binary = build / name
            subprocess.run(compiler + flags + ["-I" + str(build), "-I" + str(MAIN / "drivers"),
                           str(HERE / (name + ".c")), "-o", str(binary), "-lm"], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
