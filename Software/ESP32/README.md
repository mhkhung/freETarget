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
  or rifle), practice or match. Matches start with sighters; **Start match**
  switches to record shots. Sighting and match countdowns (15 and 75 minutes by
  default) come from the journal, so they survive a reload.
- Scores as in the PC client: decimal and integer, inner tens (*), totals per
  series of 10, sighters kept apart, score breakdown, group size and mean
  radius.
- Spoken scores (Sound), CSV export, a printable page, and **History** to open
  any session still in the journal.

Match length and times are kept by each browser, not by the target.

## WebSocket API

Browsers and other network clients can use `ws://<target>:8080/ws`. The
protocol is described in [main/TCPIP/ws_api.h](main/TCPIP/ws_api.h). In short:

- On connecting, the target sends `{"hello": ...}` with its name, firmware
  version, current session and `next_seq`.
- The client sends `{"AFTER": 0}` to receive the current session, or
  `{"AFTER": <last seq seen>}` after reconnecting. The target sends those
  shots from the journal, then `{"caught_up": n}`, then new shots as they are
  fired. Nothing is lost or repeated across a dropped connection.
- Shots look like the TCP ones (`"shot"`, `"seq"`, `"x"`, `"y"`, `"r"`, `"a"`,
  `"utc"`, ...) plus `"session"`. Session changes
  arrive as `{"session": ..., "change": "new"}` or `"mode"`.
- Any other message is an ordinary JSON command (`{"TIME": ...}`,
  `{"ECHO":0}`, ...). Command output comes back as plain text.
- `{"SESSIONS": n}` lists the sessions in the journal and `{"HISTORY": s}`
  replays one, tagged with `"history"`.

The main page (`http://<target>/`) and `http://<target>:8080/live`, a small
test page with a command line, both use this API. It is on port 8080 so that a
`/json` request on port 80, which waits for its command to finish, never holds
up the shots.
