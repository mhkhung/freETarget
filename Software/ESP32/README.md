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

Modules without ESP-IDF dependencies are tested on the development computer:

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
- **Status:** `{"ECHO":0}` shows a `JOURNAL` line.

**Upgrading existing units:** an OTA update cannot change the partition table.
A unit updated only over the air has no `shots` partition. It keeps working,
but it keeps shots in RAM only and reports this at start-up. Flash once over USB
(`idf.py flash`, or `write_flash @flash_args`) to add the partition. That flash
does not move any existing partition, so NVS settings and calibration are kept.
