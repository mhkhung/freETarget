# freETarget ESP32 firmware

Firmware for the freETarget V5/V6 boards (ESP32-S3, 8 MB flash), built with **ESP-IDF 5.3.1**.

## Building

With ESP-IDF 5.3.1 installed:

```sh
idf.py build
idf.py -p <port> flash monitor
```

Without a local ESP-IDF, use the official Docker image:

```sh
docker run --rm -v "$PWD":/project -w /project espressif/idf:v5.3.1 idf.py build
```

Docker on macOS cannot reach USB serial ports, so flash from the host with
`esptool` (`pip install esptool`):

```sh
cd build
python -m esptool --chip esp32s3 -p <port> -b 460800 write_flash @flash_args
```

Run `idf.py reconfigure` after adding or removing a `.c` file under `main/`.

## Host unit tests

Run the journal, command stream, timed fire and athlete-page regression tests
on the development computer with a C compiler, Python 3 and Node.js installed:

```sh
cd test/host
make
```

## Shot journal

Every scored shot is saved to flash before it is reported, so shots survive a
power loss.

- **Storage:** the `shots` partition in [partitions.csv](partitions.csv) (256 KB
  at 0x610000) holds about 4,000 shots as an append-only ring of 64-byte
  records. Each record has a CRC, so a write or erase that is cut off by a power
  loss is detected and skipped. See [main/journal.h](main/journal.h).
- **Numbering:**
  - `"shot"` counts the shots in the current session, starting at 1. It no
    longer wraps at 100.
  - `"seq"` is a sequence number that is unique for every shot and is never
    reset. Clients can use it to avoid duplicates.
- **Sessions:** `{"SESSION":0}` starts a new session but does not erase the
  saved shots. After a restart the target resumes the last session, including
  sighters or match, and reconnecting clients receive that session's shots.
- **Ending a session:** `{"SESSION_END":0}` (the page's **End session**), a new
  session, or an hour without shots ends the current session. The next shot
  then starts a new one, so the next athlete never continues it. An ended
  session is not resumed at power-up and not replayed to clients that connect.
- **Privacy:** the athlete's name (`{"ATHLETE":...}`) is kept in RAM only. It is
  not saved to flash or the journal, and is cleared when a session ends.
- **Status:** `{"ECHO":0}` shows a `JOURNAL` line.

**Upgrading existing units:** an OTA update cannot change the partition table.
A unit updated only over the air has no `shots` partition. It keeps working,
but it keeps shots in RAM only and reports this at start-up. Flash once over USB
(`idf.py flash`, or `write_flash @flash_args`) to add the partition. That flash
does not move any existing partition, so NVS settings and calibration are kept.

## Time of day

The board has no battery-backed clock, so the time of day is set again after
every power-up. Each shot then carries `"utc"` (Unix seconds) and `"clock"`,
which says where the time came from:

- `"ntp"`: in station mode (`WIFI_SSID` set) the target asks a time server
  itself: the one the router announces over DHCP, or `pool.ntp.org`. NTP always
  takes priority.
- `"client"`: a client sent `{"TIME": <Unix seconds>}`. This is only accepted
  if the clock has not been set yet since power-up, and only if it is not
  earlier than the firmware's build date or the newest NTP time already in the
  journal. In access point mode this is the only source.

`{"TIME":0}` reports the clock without setting it. `{"ECHO":0}` shows a `CLOCK`
line.

Shots fired before the clock is set are sent without `"utc"`. The journal
records when the clock is set, so those shots get their time when they are
replayed later (for example to a client that connects afterwards).

## Ignoring shots on other lanes

On older hardware the sensors also pick up shots on the next lane, and these
are scored far outside the target. The target ignores any shot that lands
farther from the centre than a limit: the shot is not saved, not sent and does
not advance the witness paper. The status LED shows a miss and the console logs
the distance.

`{"MAX_RADIUS": <mm>}` sets the limit:

- `0` (the default): 1.5 times the radius of the sensor ring (`SENSOR` / 2),
  which surrounds the paper. That is 174 mm with the standard 232 mm sensor
  ring.
- A positive number: the limit in mm.
- A negative number: no limit.

## Web page

`http://<target>/` is the athlete page. It works on phones, tablets and
laptops, with no internet needed:

- Live target with the newest shot in red, zoom, and the group centre. Sighter
  targets have a marked corner.
- **New session**: athlete, discipline (10 m air pistol or rifle, 50 m pistol
  or rifle), practice, match or final. Matches start with sighters; **Start
  match** switches to record shots. Sighting and match countdowns (15 and 75
  minutes by default) come from the journal, so they survive a reload.
- **Finals** follow the ISSF 10 m format, as in the PC client: sighters (5
  minutes by default), then **Start final** runs two 5-shot series of 250 s and
  single shots of 50 s up to 24, with a clock for each step. Shots are grouped
  as 5, 5 and then pairs.
- **Range commands**: with this on, the browser speaks the range officer's
  commands ("For the first competition series, load", "Start", "Stop", and
  for matches the preparation, 10 and 5 minute calls). The script is timed from
  the journal, so every browser keeps the same time and a reload carries on.
- Scores as in the PC client: decimal and integer, inner tens (*), totals per
  series of 10, sighters kept apart, score breakdown, group size and mean
  radius. Each shot's time and the gap since the one before, with the average,
  shortest and longest gap.
- **Calibrate**: move the shots (0.1 to 2 mm steps) until they match the holes
  in the card, then save. This sets `X_OFFSET`/`Y_OFFSET` on the target, so it
  applies to new shots for every client; a locked target refuses it. Opening
  calibration reads the current offsets. If another client changes them while
  the editor is open, saving asks you to close and reopen it first.
- **Report (PDF)**: a printable A4 report with eight series per page (target
  plot, scores, gaps, times, totals and running totals), laid out like
  cvLiveScore's. Choose *Save as PDF* in the print dialog.
- Spoken scores (Sound), CSV export (with gaps), and **History** to open any
  session still in the journal.

Match length, times and which sessions were finals are kept by each browser,
not by the target. Speech needs one tap on the page first (a browser rule).

## WebSocket API

Browsers and other network clients can use `ws://<target>:8080/ws`. The
protocol is described in [main/TCPIP/ws_api.h](main/TCPIP/ws_api.h). In short:

- On connecting, the target sends `{"hello": ...}` with its name, firmware
  version, current session, `next_seq` and its calibration offsets
  (`x_offset`, `y_offset`).
- The client sends `{"AFTER": 0}` to receive the current session, or
  `{"AFTER": <last seq seen>}` after reconnecting. The target sends those
  shots from the journal, then `{"caught_up": n}`, then new shots as they are
  fired. Nothing is lost or repeated across a dropped connection.
- Shots look like the TCP ones (`"shot"`, `"seq"`, `"x"`, `"y"`, `"r"`, `"a"`,
  `"utc"`, ...) plus `"session"`. Session changes
  arrive as `{"session": ..., "change": "new"}` or `"mode"`. Changes in the
  current session include `"event"` so all clients recognize a match or final
  before its first shot. Historical event names are not stored in the journal.
- Any other message is an ordinary JSON command (`{"TIME": ...}`,
  `{"ECHO":0}`, ...). Command output comes back as plain text.
- `{"SESSIONS": n}` lists the sessions in the journal and `{"HISTORY": s}`
  replays one, tagged with `"history"`.

The main page (`http://<target>/`) and `http://<target>:8080/live`, a small
test page with a command line, both use this API. It is on port 8080 so that a
`/json` request on port 80, which waits for its command to finish, never holds
up the shots.

## JSON commands over HTTP

`http://<target>/json` runs one JSON command and returns its output as text.
Commands that change the target must be a POST with the command as the body:

```sh
curl -H 'Content-Type: application/json' -d '{"X_OFFSET":0.5}' http://<target>/json
```

A web page from another site cannot send such a request without the target's
permission, which it never gives, so a link or a page cannot change settings.
For typing into a browser, a GET still works for the commands that only report:
`/json?{"ECHO":0}`, `/json?{"VERSION":0}` and `/json?{"MFS?":0}`. Other commands
sent as a GET get `405 Method Not Allowed`. A request waits until the previous
command's output has finished (up to 5 seconds, then `503`), and collects its
own output for up to 15 seconds (`{"ECHO":0}` takes about 6).

## Settings and firmware from the command line

[Software/Utilities/fet.py](../Utilities/fet.py) changes settings that are
not on the athlete page, such as the sensor depth `Z_OFFSET`, and loads
firmware. It needs Python 3; USB needs `pip install pyserial`.

```sh
python3 Software/Utilities/fet.py --host 192.168.10.9 get Z_OFFSET
python3 Software/Utilities/fet.py --host 192.168.10.9 set Z_OFFSET=15
python3 Software/Utilities/fet.py --serial /dev/cu.usbserial-0001 get
```

`--host` is the target's address (192.168.10.9 when you join the target's own
network); `--serial` a USB port (`COM3` on Windows). `set` reads the settings
back and says if the target is locked. Add `--arduino` for the Arduino target,
which has no web server.

**Loading firmware.**
- `flash esp32` writes the bootloader, partition table and app from
  `build/` over USB (`pip install esptool`). Settings and saved shots are kept.
  Use it the first time, and on units updated only over the air (it adds the
  `shots` partition).
- `update` loads `build/freeETarget.bin` over Wi-Fi (or USB) while the target
  runs, then restarts it and checks the new build is running. Settings and saved
  shots are kept. About 15 seconds for the image over Wi-Fi.

  The target only accepts a build whose `CONFIG_APP_PROJECT_VER` (menuconfig,
  "Application manager") compares higher than the running one, and stops until it
  is restarted if sent another; `update` checks this first.

  Older firmware: 6.3.5 to 6.4.2 read the download size as 0, so the download
  fails and the target stops until it is restarted. Load this version over USB
  once. Before 6.3.5, `update` works over USB, and over Wi-Fi with `--rate 2`
  (that firmware takes in only 2.5 KB/s over Wi-Fi and loses what does not fit,
  so it takes about 10 minutes).

`GET /calibration` returns the current `x_offset` and `y_offset` as JSON with
`Cache-Control: no-store`. The athlete page uses it before editing and saving,
and to verify the saved offsets. Incomplete `/json` bodies close on receive
timeout; an overall receive budget also limits clients sending very slowly.
