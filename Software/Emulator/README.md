# freETarget virtual ESP32-S3

Run the current firmware on Espressif's ESP32 emulator, click a virtual target,
and watch the real athlete page receive the calculated shot. No target hardware
or serial port is used. All emulator code lives in this folder; production
firmware files are untouched.

## Quick start

Requirements: Python 3.10+, Docker with the daemon running, and an Apple Silicon
Mac or Linux x86-64/ARM64 machine. No Python packages are required. The first
download/build needs internet access and several GB of free disk space.

From the repository root:

```sh
docker pull espressif/idf:v5.3.1
python3 Software/Emulator/emulator.py install
python3 Software/Emulator/emulator.py prepare --build
python3 Software/Emulator/emulator.py run --fresh
```

Open **http://127.0.0.1:18888/**. Boot takes roughly 10–30 seconds depending on
the host. Wait for “Connected · armed”, then click the target or “Fire centre”.
The green marks are requested positions; orange circles are firmware results.
The panel on the right is the actual athlete page served by the emulated ESP32.
The athlete page follows the newest 10-shot series and shows decimal and integer
totals, individual shot scores, and the average hit point as a cyan cross and
X/Y offsets in millimetres. Use the series selector, arrow buttons, or series
total buttons to review an earlier series. New shots do not interrupt that
review; **Latest series** resumes following. Zoom with +/− or select Auto fit,
Full target, or a scoring ring. Sighters remain separate from scored series.
The top overview lists every scored series with ten shot scores and both
totals. Select its series label or an individual score to review it. A clock
alongside shows local time and the live session timer (practice elapsed time,
or sighting/match time remaining), including while reviewing older shots.
All displayed positions and click coordinates use centre **(0, 0)**,
positive X to the right, and **negative Y above centre**. The firmware's existing
positive-up wire/journal coordinates are converted at the display boundary;
saved shots, the hardware protocol and the CSV export keep positive-up Y, so
exported files match the journal and the PC client.
You can also open **http://127.0.0.1:18000/** directly. The emulator build's page
connects to WebSockets on port 18080 automatically; no `?ws=` parameter is needed.

Use the athlete page or dashboard buttons to start/end sessions. “Reset ESP32”
reboots the virtual CPU while retaining its flash journal. Ctrl-C stops the
emulator and saves flash; running again without `--fresh` resumes that flash.
To stop an emulator running in the background or in another terminal
(including a smoke test), run:

```sh
python3 Software/Emulator/emulator.py stop
```

It finds this checkout's `esp-emu` processes and asks each owning `run`/`smoke`
process to shut down, which saves flash. It interrupts any `esp-emu` left
without an owner (also saving flash), and kills whatever is still running
after `--timeout` seconds (default 10).
`--fresh` resets only the emulator's `target.bin`, including saved shots and
configuration. The smoke test has its own separate flash and ports.

After editing firmware, run `prepare --build` again to refresh its isolated
source copy, then restart with `run --fresh` to use the new image. Stop the
running emulator before rebuilding. Plain `prepare` reuses the existing build.

The installer downloads **esp-emu v1.2.0** from Espressif's GitHub release and
checks its release SHA-256 checksum. It installs only in `.runtime/bin/`.
The build uses ESP-IDF **v5.3.1**, matching the firmware's development container.
The first build may download the firmware's managed ESP-IDF component
dependencies. Later builds reuse the isolated build/component caches.

## What runs end to end

```text
Mouse position (mm)
  -> distance to each of four sensors / speed of sound
  -> sensor RUN latch arrivals, quantized to 100 ns
  -> virtual GPIO gates + 10 MHz clock on GPIO8
  -> emulated ESP32-S3 native PCNT peripheral
  -> stock firmware acquisition ISR and position solver
  -> stock flash journal and WebSocket API
  -> stock athlete page
```

This is an independent forward acoustic model. It never sends calculated
coordinates directly to the firmware or invents score messages. The firmware
calculates positions from pulse counts. Small differences between requested
and reported coordinates are expected: integration tests currently allow
1 mm and typically observed approximately 0.4 mm, including the firmware's sequential
counter reads. This is a software regression fixture, not a calibration tool.

The board fixture selects revision 5.2, a 232 mm sensor circle, 45° sensor
rotation and zero target depth, no high-threshold latency correction, and disabled
paper feed. It provides the HDC3022 temperature/humidity sensor with valid CRCs,
acknowledges the reference DAC, and models RUN latch clearing from STOP_N.
The sensors sit at the corners: **N top-left, E top-right, S bottom-right,
W bottom-left**. The firmware `ANGLE` setting is 45°, matching the forward
model. With the 232 mm circle, each corner is about 82.0 mm from each axis.
The dashboard draws sensor positions supplied by the same geometry model.
Startup synchronizes the firmware's `ANGLE` through its normal command API,
so older emulator flash with angle zero is upgraded without erasing the journal.
Temperature/humidity affect both generated arrivals and the virtual I²C sensor;
allow the firmware to take its next sensor reading after changing them.

“Silence sensors” omits selected arrival signals to exercise incomplete hits.
All four silenced means no hit is detected. Whether an incomplete hit is
reported depends on the firmware's `SEND_MISS` setting, which defaults to off.
The wider view permits clicks outside the sensor circle; the firmware may
reject these as invalid geometry. The generator waits for the firmware to
rearm after ringing before accepting another shot.

## Simulation boundary

The emulator boots like hardware: the real ESP32-S3 ROM runs Espressif's
2nd-stage bootloader, which reads the partition table, validates the factory
image and starts the application.

`--batch-size 64` sets how many instructions each core runs between event
steps, which bounds how late a sensor edge is applied (about 2 µs observed).
Do not lower it to 16 or below: esp-emu v1.2.0 then faults while the ROM starts
the second core (`Bus fault: read16 unmapped 0x7FFFFFFF` in `start_other_core`,
right after "Loaded app"). With `--skip-rom` the fault only appears at batch
size 1, so it is an emulator race, not a firmware or page-size problem.

**The oscillator electrical startup self-test (`POST_counters`) is bypassed.**
The isolated build links `firmware/board_boot.c` to replace only that test.
This is printed in the firmware console and stated on the dashboard.
The board supplies finite native 10 MHz pulse bursts during shots rather than
a continuously running external oscillator. Acquisition, position calculation,
session handling, journal storage, HTTP and WebSockets use the production
sources unchanged.

The emulator snapshot also raises `CONFIG_HTTPD_MAX_REQ_HDR_LEN` from 512 to
8192 bytes. Normal browser headers and cookies from other localhost services
otherwise cause HTTP 431 on page loads or WebSocket connections. This changes
only the copied `sdkconfig`; the hardware firmware configuration is untouched.
The copied athlete page's WebSocket URL function is also adapted to the host
port mapping (HTTP port + 80). Explicit `?ws=host:port` overrides still work.
The build changes that URL function only in its source snapshot.

This does not validate microphone/comparator electronics, real noise, latch
propagation delays, high-threshold rise-time correction, power supply behavior,
paper motors, OTA, RF behavior, or electrical POST. Do not flash the generated
emulator image onto hardware.

`TimeClaim`/`TimeGrant` hold the virtual CPU between short scheduling windows,
so sensor timing uses virtual nanoseconds rather than Python sleep precision.
Pulse chunks stay within the emulator's bounded event queue. The dashboard
shows late/dropped event counts and maximum lateness. Native CPU/peripheral
stepping can apply clock edges slightly late even when they were queued ahead;
the integration test requires no dropped inputs or notifications, a maximum
lateness of 10 µs, and the position tolerance above. It does not claim exact
100 ns delivery inside the emulated CPU.
Host networking and thread scheduling still make a whole run non-deterministic.
Emulated elapsed time may run slower than wall time on a busy host.

## Validation

```sh
python3 -m unittest discover -s Software/Emulator -p 'test_*.py' -v
node Software/Emulator/test_browser_connection.js
node Software/Emulator/test_coordinates.js
python3 Software/Emulator/emulator.py smoke
```

Unit tests cover propagation geometry, environmental effects, malformed
protocol input, fragmented network reads, timeout recovery, sensor CRC, and
gate ordering. The integration test boots the CPU and checks real HTTP/WS
responses (including browser-sized headers and localhost cookies), centre and
off-centre scoring, reconnect replay, reset recovery,
explicit session end/start, historical replay, and flash persistence across
stopping/restarting the emulator process. It fails on excessive timing error
or dropped events. It uses ports offset by 100 and cannot erase the interactive target.
When Node.js is installed, smoke also evaluates the served page's actual
WebSocket URL function without query parameters and connects to that endpoint.
It also runs the athlete rendering tests against the HTML returned by the
emulated firmware: ten-shot grouping, both totals, average-point placement,
zoom, misses, and series browsing during live updates.

## Local files and ports

Everything generated is ignored under `.runtime/`: downloaded binary, isolated
source/build, seeded NVS, factory flash, mutable target/smoke flash, and logs.
The key logs are `target-console.log` and `target-emulator.log`.

| Purpose | Interactive port | Smoke port |
| --- | ---: | ---: |
| Dashboard | 18888 | — |
| Firmware HTTP | 18000 | 18100 |
| Firmware WebSocket | 18080 | 18180 |
| Board co-simulation | 18990 | 19090 |
| Emulator control | 18991 | 19091 |

The dashboard, firmware forwarding, board and control sockets bind to loopback.
Guest-initiated TCP/UDP is restricted; DNS points to localhost so the fixture
does not contact public time/name servers. No TAP device, bridge or root access
is needed. Dashboard HTTP mutations require JSON and reject foreign origins.
`run --port 18889` changes the dashboard port; it still uses the same single
interactive firmware instance/ports.

Implementation references:
[Espressif emulator](https://github.com/espressif/esp-emulator),
[v1.2.0 release](https://github.com/espressif/esp-emulator/releases/tag/v1.2.0),
[co-simulation protocol](https://github.com/espressif/esp-emulator/blob/main/proto/esp_cosim.proto),
[host interfaces](https://github.com/espressif/esp-emulator/blob/main/docs/guides/HOST-INTERFACES.md).
