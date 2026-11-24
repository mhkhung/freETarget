"""Write the parts of the sketch a host test needs, ready for a host compiler.

The Arduino IDE adds prototypes for every function and accepts GCC's "1.0d"
literals; this does the same for the code it takes.  Usage:
    python3 extract.py <sketch folder> > json_host.inc
    python3 extract.py <sketch folder> compute_hit > compute_hit_host.inc
"""
import re
import sys
from pathlib import Path

part = sys.argv[2] if len(sys.argv) > 2 else "json"
source = (Path(sys.argv[1]) / (part + ".ino")).read_text()
source = re.sub(r"^#include[^\n]*", "", source, flags=re.MULTILINE)
source = re.sub(r"(\d)d\b", r"\1", source)                    # 180.0d -> 180.0

if part == "compute_hit":
    names = ("init_sensors", "compute_hit", "doppler_fade", "adjust_clocks", "target_geometry",
             "distance_error", "find_xy_3D", "check_for_inside")
    head = source[source.index("#define THRESHOLD"):source.index("/*", source.index("static bool check_for_inside"))]
    print(head)
    print("\n".join(d for d in re.findall(r"^#define [^\n]*", source, re.MULTILINE) if d not in head))
    bodies = []
    for name in names:
        found = re.search(r"^(?:static )?[A-Za-z][\w \*]*\b%s\s*\([^;{]*?\)\s*\{.*?^\}" % name, source, re.MULTILINE | re.DOTALL)
        if found is None and name != "distance_error":
            sys.exit("compute_hit.ino: %s() not found" % name)
        if found:
            bodies.append(found.group())
            print(re.sub(r"//[^\n]*", "", found.group()[:found.group().index("{")]).strip() + ";")   # Prototype
    print("\n\n".join(bodies))
    sys.exit(0)

table_end = re.search(r"^const json_message JSON\[\] = \{.*?^\};\n", source, re.MULTILINE | re.DOTALL)
if table_end is None:
    sys.exit("json.ino: command table not found")
statics = re.findall(r"^static (?:uint16_t|int16_t|bool|char) [^(\n]*;[^\n]*$", source[table_end.end():], re.MULTILINE)


def function(name, required=True):
    match = re.search(r"^[A-Za-z][\w \*]*\b%s\s*\([^)]*\)\s*\n\{.*?^\}" % name, source, re.MULTILINE | re.DOTALL)
    if match is None and required:
        sys.exit("json.ino: %s() not found" % name)
    return match.group() if match else ""


# The sketch is built with -fpermissive: handlers with other signatures and
# unsigned values go into the table as they are.  Cast them for the host.
table = source[table_end.start():table_end.end()]
table = re.sub(r"(?<=,)(\s*)&(?!json_|shot_number)(\w+)(?=\s*,)", r"\1(void (*)(int))&\2", table)  # Handlers
table = table.replace("&shot_number", "(int *)&shot_number")                                      # unsigned

print("static void diag_delay(int x);")
print(source[:table_end.start()])
print(table)
print("\n".join(statics))
for name in ("to_int", "instr", "read_JSON"):
    print(function(name))
print(function("json_text_size", required=False))   # Not in sketches before the text bound
