/*----------------------------------------------------------------
 *
 * journal.h
 *
 * Power-loss safe, append-only shot journal.
 *
 *----------------------------------------------------------------
 *
 * This file and journal.c are plain C with no ESP-IDF dependencies
 * so they can be unit tested on a host computer
 * (see Software/ESP32/test/host).  The ESP32 binding lives in
 * shot_journal.c.
 *
 * Layout
 *
 *   The storage is a ring of fixed 64 byte entries grouped into
 *   erase sectors (4096 bytes = 64 entries).  Entries are only ever
 *   appended.  Before the first entry of a sector is written the
 *   whole sector is erased, which discards the oldest 64 entries.
 *
 *   Every entry carries a CRC32.  An entry that was cut short by a
 *   power loss, or a sector whose erase was interrupted, fails the
 *   CRC and is skipped.  Nothing is ever rewritten in place.
 *
 * Numbering
 *
 *   seq      - Increases by one for every entry and is never reset,
 *              not even by a new session.  Clients use it to ask for
 *              "everything after seq N".
 *   session  - Increases by one for every new session.
 *   shot     - Shot number within the session, starting at 0.
 *
 * Time
 *
 *   Every entry is stamped with the wall clock time when it was
 *   written (the "time" field):
 *
 *     JRNL_TIME_UNKNOWN    no clock was available
 *     below JRNL_TIME_MIN  seconds since power up; the clock had not
 *                          been set yet
 *     otherwise            Unix time (seconds since 1970, UTC)
 *
 *   The source of the time (NTP or a client) is kept in the flags.
 *
 *   When the clock is set, a JRNL_CLOCK_SET entry records the Unix
 *   time and the seconds since power up at that moment.  This lets
 *   journal_entry_time() turn the "seconds since power up" stamps of
 *   shots fired before the clock was set into Unix time.
 *
 *--------------------------------------------------------------*/
#ifndef _JOURNAL_H_
#define _JOURNAL_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define JRNL_ENTRY_SIZE 64     // Bytes per entry
#define JRNL_MAGIC      0x4A53 // 'JS'
#define JRNL_VERSION    1      // Entry layout version

/*
 * Entry kinds
 */
#define JRNL_SHOT    1         // A scored shot (or a reported miss)
#define JRNL_SESSION 2         // A new session was started
#define JRNL_MODE    3         // The session type (sighters / match) changed
#define JRNL_CLOCK_SET 4       // The wall clock was set

/*
 * Time stamps
 */
#define JRNL_TIME_UNKNOWN 0xFFFFFFFFu // No clock
#define JRNL_TIME_MIN     1000000000u // Sept 2001. Smaller values are seconds since power up

/*
 * Flags
 */
#define JRNL_FLAG_MISS    0x01 // The shot was reported as a miss
#define JRNL_SOURCE_SHIFT 1    // Where the time came from, JRNL_SOURCE_xxx
#define JRNL_SOURCE_MASK  (0x03 << JRNL_SOURCE_SHIFT)

#define JRNL_SOURCE_NONE   0   // The clock has not been set
#define JRNL_SOURCE_CLIENT 1   // Set by a client with {"TIME":...}
#define JRNL_SOURCE_NTP    2   // Set by an NTP server

#define JRNL_CLOCKS 16         // Clock settings remembered by the journal (one per power up is enough)

/*
 * Return codes
 */
#define JRNL_OK          0
#define JRNL_ERR_PARAM   -1    // Bad argument or geometry
#define JRNL_ERR_FLASH   -2    // The storage driver reported an error
#define JRNL_ERR_FULL    -3    // No writable slot found
#define JRNL_ERR_NOT_OPEN -4   // journal_open() has not succeeded

/*
 * One journal entry.  The layout is fixed at 64 bytes so that entries
 * never straddle an erase sector.
 */
typedef struct __attribute__((packed))
{
  uint16_t magic;          // JRNL_MAGIC
  uint8_t  kind;           // JRNL_SHOT, JRNL_SESSION, JRNL_MODE, JRNL_CLOCK_SET
  uint8_t  version;        // JRNL_VERSION
  uint32_t seq;            // Global sequence number, never reset
  uint32_t session;        // Session number
  uint16_t shot;           // Shot number within the session (0 based)
  uint8_t  session_type;   // SESSION_xxx bits at the time of the entry
  uint8_t  flags;          // JRNL_FLAG_MISS and the time source
  uint32_t shot_time;      // Time of the shot in ms (since the start of the run)
  union
  {
    struct                 // JRNL_SHOT
    {
      float    x_mm;       // Location on the target face
      float    y_mm;
      float    radius;     // Polar location
      float    angle;      // Radians
      int32_t  timer_count[4]; // N, E, S, W counter values
      uint16_t face_strike;    // Face strike count
      uint16_t sensor_status;  // Sensor status bits
    };
    struct                 // JRNL_CLOCK_SET
    {
      uint32_t boot_seq;   // seq of the first entry written since power up
      uint32_t uptime;     // Seconds since power up when the clock was set to "time"
    } clock;
  };
  uint32_t time;           // Wall clock time stamp, see above
  uint32_t crc;            // CRC32 of the preceding 60 bytes
} jrnl_entry_t;

/*
 * Storage driver.  Offsets are relative to the start of the journal area.
 */
typedef struct
{
  int (*read)(void *ctx, uint32_t offset, void *buffer, size_t length);
  int (*write)(void *ctx, uint32_t offset, const void *buffer, size_t length);
  int (*erase)(void *ctx, uint32_t offset, size_t length); // Sector aligned
  void    *ctx;                                           // Passed to the driver functions
  uint32_t size;                                          // Bytes, a multiple of sector_size
  uint32_t sector_size;                                   // Erase sector size, a multiple of JRNL_ENTRY_SIZE
} jrnl_flash_t;

/*
 * Clock source.  Returns the current time stamp (see "Time" above)
 * and sets *source to JRNL_SOURCE_xxx.
 */
typedef uint32_t (*jrnl_now_t)(void *ctx, uint8_t *source);

typedef struct
{
  uint32_t boot_seq;               // Covers entries from this seq
  uint32_t seq;                    // up to (not including) the JRNL_CLOCK_SET entry
  uint32_t offset;                 // Unix time - seconds since power up
  uint8_t  source;                 // JRNL_SOURCE_xxx
} jrnl_clock_t;

/*
 * Journal state
 */
typedef struct
{
  jrnl_flash_t flash;              // Storage driver
  bool         is_open;            // true after a successful journal_open()
  uint32_t     slots;              // Number of entries that fit in the storage
  uint32_t     slots_per_sector;   // Entries per erase sector
  uint32_t     head;               // Next slot to write
  uint32_t     next_seq;           // Sequence number of the next entry
  uint32_t     session;            // Current session (0 = none yet)
  uint32_t     session_start_seq;  // seq of the first entry of the current session
  uint16_t     session_shots;      // Shots recorded in the current session
  uint8_t      session_type;       // Session type from the latest entry
  uint32_t     entries;            // Valid entries found by journal_open()
  uint32_t     bad_entries;        // Entries that failed the CRC in journal_open()
  jrnl_now_t   now;                // Time stamps entries, NULL = JRNL_TIME_UNKNOWN
  void        *now_ctx;
  uint32_t     boot_seq;           // seq of the first entry written since journal_open()
  uint32_t     last_ntp_time;      // Newest NTP time stamp in the journal (0 = none)
  uint32_t     clock_count;        // Valid entries in clocks[]
  jrnl_clock_t clocks[JRNL_CLOCKS];// Newest clock settings
} journal_t;

/*
 * Iterator used to read entries back in seq order
 */
typedef struct
{
  uint32_t slot;                   // Next slot to inspect
  uint32_t visited;                // Slots inspected so far
  uint32_t last_seq;               // Only entries after this seq are returned
  uint32_t session;                // Only this session (0 = any)
  uint8_t  kind;                   // Only this kind (0 = any)
} jrnl_iter_t;

/*
 * Functions
 */
int  journal_open(journal_t *j, const jrnl_flash_t *flash);       // Scan the storage and recover the state
int  journal_append_shot(journal_t *j, jrnl_entry_t *entry);      // Append a shot, fills in seq / session / shot
int  journal_new_session(journal_t *j, uint8_t session_type);     // Start a new session
int  journal_set_mode(journal_t *j, uint8_t session_type);        // Record a session type change
void journal_set_time_source(journal_t *j, jrnl_now_t now, void *ctx); // Call after journal_open()
int  journal_clock_set(journal_t *j, uint32_t unix_time, uint32_t uptime, uint8_t source); // The clock was set
uint32_t journal_entry_time(const journal_t *j, const jrnl_entry_t *entry, uint8_t *source); // Unix time of an entry, or JRNL_TIME_UNKNOWN
void journal_iter_session(const journal_t *j, jrnl_iter_t *it);   // Iterate over the shots of the current session
void journal_iter_after(const journal_t *j, jrnl_iter_t *it, uint32_t after_seq); // Iterate over all entries after a seq
bool journal_next(const journal_t *j, jrnl_iter_t *it, jrnl_entry_t *entry);      // Next matching entry, false when done
bool journal_entry_valid(const jrnl_entry_t *entry);              // Check the magic, version and CRC
uint32_t journal_crc32(const void *data, size_t length);          // CRC32 (IEEE 802.3)

#endif
