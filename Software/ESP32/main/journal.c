/*----------------------------------------------------------------
 *
 * journal.c
 *
 * Power-loss safe, append-only shot journal.  See journal.h for the
 * storage layout and numbering.
 *
 *----------------------------------------------------------------
 *
 * Recovery rules (journal_open)
 *
 *  - A slot that is all 0xFF is blank.
 *  - A slot that is not blank but fails the CRC is damaged (a write
 *    or an erase was cut short) and is ignored.
 *  - The valid entry with the highest seq is the newest.  Writing
 *    resumes in the slot after it.
 *
 * Writing rules
 *
 *  - The first slot of a sector is never written until the sector
 *    has been erased.
 *  - Any other slot is only written if it is blank.  A damaged slot
 *    is skipped, never rewritten.
 *
 * Together these mean that the storage can be cut off at any point
 * and the journal will still recover every entry that was completely
 * written.
 *
 *--------------------------------------------------------------*/
#include <string.h>

#include "journal.h"

_Static_assert(sizeof(jrnl_entry_t) == JRNL_ENTRY_SIZE, "jrnl_entry_t must be 64 bytes");

#define CRC_LENGTH (offsetof(jrnl_entry_t, crc)) // The CRC covers everything before it

/*----------------------------------------------------------------
 *
 * @function: journal_crc32
 *
 * @brief:    CRC32 (IEEE 802.3, reflected, polynomial 0xEDB88320)
 *
 *--------------------------------------------------------------*/
uint32_t journal_crc32(const void *data, size_t length)
{
  const uint8_t *p   = (const uint8_t *)data;
  uint32_t       crc = 0xFFFFFFFFu;

  while ( length-- != 0 )
  {
    crc ^= *p++;
    for ( int bit = 0; bit != 8; bit++ )
    {
      crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
  }

  return ~crc;
}

/*----------------------------------------------------------------
 *
 * Helpers
 *
 *--------------------------------------------------------------*/
static bool is_blank(const jrnl_entry_t *entry)
{
  const uint8_t *p = (const uint8_t *)entry;

  for ( size_t i = 0; i != sizeof(*entry); i++ )
  {
    if ( p[i] != 0xFF )
    {
      return false;
    }
  }
  return true;
}

bool journal_entry_valid(const jrnl_entry_t *entry)
{
  return (entry->magic == JRNL_MAGIC)                                 // Looks like an entry
         && (entry->version == JRNL_VERSION)                          // that we understand
         && (entry->crc == journal_crc32(entry, CRC_LENGTH));         // and it was completely written
}

static int read_slot(const journal_t *j, uint32_t slot, jrnl_entry_t *entry)
{
  return j->flash.read(j->flash.ctx, slot * JRNL_ENTRY_SIZE, entry, sizeof(*entry));
}

static uint8_t entry_source(const jrnl_entry_t *entry)
{
  return (uint8_t)((entry->flags & JRNL_SOURCE_MASK) >> JRNL_SOURCE_SHIFT);
}

static bool is_unix_time(uint32_t time)
{
  return (time >= JRNL_TIME_MIN) && (time != JRNL_TIME_UNKNOWN);
}

/*
 * Remember a clock setting.  The newest JRNL_CLOCKS (by seq) are kept.
 */
static void remember_clock(journal_t *j, const jrnl_entry_t *entry)
{
  jrnl_clock_t clock;
  uint32_t     i;
  uint32_t     oldest = 0;

  if ( !is_unix_time(entry->time) || (entry->clock.uptime >= JRNL_TIME_MIN) || (entry->clock.boot_seq > entry->seq) )
  {
    return;
  }

  clock.boot_seq = entry->clock.boot_seq;
  clock.seq      = entry->seq;
  clock.offset   = entry->time - entry->clock.uptime;
  clock.source   = entry_source(entry);

  if ( j->clock_count < JRNL_CLOCKS )
  {
    j->clocks[j->clock_count++] = clock;
    return;
  }

  for ( i = 1; i != JRNL_CLOCKS; i++ )                                // Replace the oldest one if this is newer
  {
    if ( j->clocks[i].seq < j->clocks[oldest].seq )
    {
      oldest = i;
    }
  }
  if ( clock.seq > j->clocks[oldest].seq )
  {
    j->clocks[oldest] = clock;
  }
}

static void note_time(journal_t *j, const jrnl_entry_t *entry)       // Track the newest NTP time
{
  if ( (entry_source(entry) == JRNL_SOURCE_NTP) && is_unix_time(entry->time) && (entry->time > j->last_ntp_time) )
  {
    j->last_ntp_time = entry->time;
  }
}

static uint32_t oldest_slot(const journal_t *j)                       // First slot of the sector after the head
{
  uint32_t sector = (j->head / j->slots_per_sector) + 1;

  return (sector * j->slots_per_sector) % j->slots;
}

/*----------------------------------------------------------------
 *
 * @function: journal_open
 *
 * @brief:    Scan the storage and recover the journal state
 *
 * @return:   JRNL_OK, or an error code
 *
 *--------------------------------------------------------------*/
int journal_open(journal_t *j, const jrnl_flash_t *flash)
{
  jrnl_entry_t entry;
  uint32_t     slot;
  bool         found     = false;
  uint32_t     max_seq   = 0;
  uint32_t     max_slot  = 0;
  uint16_t     max_shot  = 0;
  bool         any_shots = false;
  uint32_t     target_seq = 0;
  bool         ended      = false;

  if ( (j == NULL) || (flash == NULL) || (flash->read == NULL) || (flash->write == NULL) || (flash->erase == NULL) )
  {
    return JRNL_ERR_PARAM;
  }

  memset(j, 0, sizeof(*j));
  j->flash = *flash;

  if ( (flash->sector_size == 0) || ((flash->sector_size % JRNL_ENTRY_SIZE) != 0) // Entries must not straddle a sector
       || ((flash->size % flash->sector_size) != 0)                             // Whole sectors only
       || ((flash->size / flash->sector_size) < 2) )                           // One sector is always being recycled
  {
    return JRNL_ERR_PARAM;
  }

  j->slots            = flash->size / JRNL_ENTRY_SIZE;
  j->slots_per_sector = flash->sector_size / JRNL_ENTRY_SIZE;
  j->next_seq         = 1;

  /*
   * Pass 1: find the newest entry
   */
  for ( slot = 0; slot != j->slots; slot++ )
  {
    if ( read_slot(j, slot, &entry) != 0 )
    {
      return JRNL_ERR_FLASH;
    }

    if ( is_blank(&entry) )
    {
      continue;
    }

    if ( !journal_entry_valid(&entry) )
    {
      j->bad_entries++;
      continue;
    }

    j->entries++;
    note_time(j, &entry);
    if ( entry.kind == JRNL_CLOCK_SET )
    {
      remember_clock(j, &entry);
    }
    if ( !found || (entry.seq > max_seq) )
    {
      found    = true;
      max_seq  = entry.seq;
      max_slot = slot;
    }
  }

  if ( !found )                                                       // Empty journal
  {
    j->boot_seq = j->next_seq;
    j->is_open  = true;
    return JRNL_OK;
  }

  /*
   * Recover the state from the newest entry
   */
  read_slot(j, max_slot, &entry);
  j->newest            = entry;
  j->head              = (max_slot + 1) % j->slots;
  j->next_seq          = max_seq + 1;
  j->session           = entry.session;
  j->session_type      = entry.session_type;
  j->session_start_seq = entry.seq;

  /*
   * Pass 2: count the shots belonging to the current session, and find
   * its target type (newest session or mode entry)
   */
  for ( slot = 0; slot != j->slots; slot++ )
  {
    if ( (read_slot(j, slot, &entry) != 0) || !journal_entry_valid(&entry) || (entry.session != j->session) )
    {
      continue;
    }

    if ( entry.seq < j->session_start_seq )
    {
      j->session_start_seq = entry.seq;
    }

    if ( ((entry.kind == JRNL_SESSION) || (entry.kind == JRNL_MODE)) && (entry.seq >= target_seq) )
    {
      target_seq = entry.seq;
      j->target  = entry.info.target;
    }

    if ( entry.kind == JRNL_END )                                     // Clock entries may follow it, so look for it anywhere
    {
      ended = true;
    }

    if ( entry.kind == JRNL_SHOT )
    {
      if ( !any_shots || (entry.shot > max_shot) )
      {
        max_shot = entry.shot;
      }
      any_shots = true;
    }
  }

  j->session_shots = any_shots ? (uint16_t)(max_shot + 1) : 0;
  j->session_open  = (j->session != 0) && !ended;
  j->boot_seq      = j->next_seq;
  j->is_open       = true;

  return JRNL_OK;
}

/*----------------------------------------------------------------
 *
 * @function: write_entry
 *
 * @brief:    Seal an entry with seq, time + CRC and write it at the head
 *
 * @return:   JRNL_OK, or an error code
 *
 *----------------------------------------------------------------
 *
 * The seq is consumed even if the write fails so that a seq handed
 * to a client is never given to a different entry later on in this
 * power cycle.
 *
 *--------------------------------------------------------------*/
static int write_entry(journal_t *j, jrnl_entry_t *entry)
{
  jrnl_entry_t check;
  uint32_t     tries;
  uint32_t     slot;
  uint8_t      source = JRNL_SOURCE_NONE;

  if ( entry->kind != JRNL_CLOCK_SET )                                // A clock entry carries its own time
  {
    entry->time = (j->now != NULL) ? j->now(j->now_ctx, &source) : JRNL_TIME_UNKNOWN;
    if ( !is_unix_time(entry->time) )
    {
      source = JRNL_SOURCE_NONE;
    }
    entry->flags = (uint8_t)((entry->flags & ~JRNL_SOURCE_MASK) | ((source << JRNL_SOURCE_SHIFT) & JRNL_SOURCE_MASK));
  }

  entry->magic   = JRNL_MAGIC;
  entry->version = JRNL_VERSION;
  entry->seq     = j->next_seq++;
  entry->crc     = journal_crc32(entry, CRC_LENGTH);
  note_time(j, entry);
  j->newest      = *entry;

  for ( tries = 0; tries != j->slots; tries++ )
  {
    slot    = j->head;
    j->head = (slot + 1) % j->slots;                                  // Never come back to this slot

    if ( (slot % j->slots_per_sector) == 0 )                          // Entering a sector, recycle it
    {
      if ( j->flash.erase(j->flash.ctx, slot * JRNL_ENTRY_SIZE, j->flash.sector_size) != 0 )
      {
        return JRNL_ERR_FLASH;
      }
    }
    else
    {
      if ( read_slot(j, slot, &check) != 0 )
      {
        return JRNL_ERR_FLASH;
      }
      if ( !is_blank(&check) )                                        // Damaged by an earlier power loss, skip it
      {
        continue;
      }
    }

    if ( j->flash.write(j->flash.ctx, slot * JRNL_ENTRY_SIZE, entry, sizeof(*entry)) != 0 )
    {
      return JRNL_ERR_FLASH;
    }

    if ( (read_slot(j, slot, &check) != 0) || (memcmp(&check, entry, sizeof(check)) != 0) ) // Read back to be sure
    {
      return JRNL_ERR_FLASH;
    }

    return JRNL_OK;
  }

  return JRNL_ERR_FULL;
}

/*----------------------------------------------------------------
 *
 * @function: journal_new_session
 *
 * @brief:    Start a new session
 *
 * @return:   JRNL_OK, or an error code
 *
 *----------------------------------------------------------------
 *
 * The in-memory state moves to the new session even if the write
 * fails, so the target keeps working without its storage.
 *
 *--------------------------------------------------------------*/
int journal_new_session(journal_t *j, uint8_t session_type)
{
  jrnl_entry_t entry;
  int          status;

  if ( (j == NULL) || !j->is_open )
  {
    return JRNL_ERR_NOT_OPEN;
  }

  memset(&entry, 0, sizeof(entry));
  entry.kind         = JRNL_SESSION;
  entry.session      = j->session + 1;
  entry.session_type = session_type;
  entry.info.target  = j->target;

  status = write_entry(j, &entry);

  j->session           = entry.session;
  j->session_start_seq = entry.seq;
  j->session_shots     = 0;
  j->session_type      = session_type;
  j->session_open      = true;

  return status;
}

/*----------------------------------------------------------------
 *
 * @function: journal_end_session
 *
 * @brief:    End the current session
 *
 * @return:   JRNL_OK, or an error code
 *
 *----------------------------------------------------------------
 *
 * Nothing is written if no session is open.  The next shot or
 * session type change starts a new session.
 *
 *--------------------------------------------------------------*/
int journal_end_session(journal_t *j, uint8_t reason)
{
  jrnl_entry_t entry;

  if ( (j == NULL) || !j->is_open )
  {
    return JRNL_ERR_NOT_OPEN;
  }

  if ( !j->session_open )
  {
    return JRNL_OK;
  }

  memset(&entry, 0, sizeof(entry));
  entry.kind         = JRNL_END;
  entry.session      = j->session;
  entry.shot         = j->session_shots;
  entry.session_type = j->session_type;
  entry.info.target  = j->target;
  entry.info.reason  = reason;
  j->session_open    = false;

  return write_entry(j, &entry);
}

/*----------------------------------------------------------------
 *
 * @function: journal_set_mode
 *
 * @brief:    Record that the session type changed (ex sighters to match)
 *
 *--------------------------------------------------------------*/
int journal_set_mode(journal_t *j, uint8_t session_type)
{
  jrnl_entry_t entry;

  if ( (j == NULL) || !j->is_open )
  {
    return JRNL_ERR_NOT_OPEN;
  }

  if ( !j->session_open )                                             // No session, or it has ended: start one
  {
    return journal_new_session(j, session_type);
  }

  memset(&entry, 0, sizeof(entry));
  entry.kind         = JRNL_MODE;
  entry.session      = j->session;
  entry.shot         = j->session_shots;
  entry.session_type = session_type;
  entry.info.target  = j->target;
  j->session_type    = session_type;

  return write_entry(j, &entry);
}

/*----------------------------------------------------------------
 *
 * @function: journal_set_target
 *
 * @brief:    Set the target type recorded with the next session and
 *            mode entries
 *
 *--------------------------------------------------------------*/
void journal_set_target(journal_t *j, uint16_t target)
{
  j->target = target;
}

/*----------------------------------------------------------------
 *
 * @function: journal_append_shot
 *
 * @brief:    Append a shot to the current session
 *
 * @return:   JRNL_OK, or an error code
 *
 *----------------------------------------------------------------
 *
 * The caller fills in the shot data.  This function fills in kind,
 * session, shot and seq so the caller can report them to clients.
 *
 *--------------------------------------------------------------*/
int journal_append_shot(journal_t *j, jrnl_entry_t *entry)
{
  int status;

  if ( (j == NULL) || (entry == NULL) || !j->is_open )
  {
    return JRNL_ERR_NOT_OPEN;
  }

  if ( !j->session_open )                                             // First shot ever, or after the session ended
  {
    journal_new_session(j, entry->session_type);
  }

  entry->kind    = JRNL_SHOT;
  entry->session = j->session;
  entry->shot    = j->session_shots;

  status = write_entry(j, entry);

  j->session_shots++;
  j->session_type = entry->session_type;

  return status;
}

/*----------------------------------------------------------------
 *
 * @function: journal_set_time_source
 *
 * @brief:    Install the function that time stamps new entries
 *
 *--------------------------------------------------------------*/
void journal_set_time_source(journal_t *j, jrnl_now_t now, void *ctx)
{
  j->now     = now;
  j->now_ctx = ctx;
}

/*----------------------------------------------------------------
 *
 * @function: journal_clock_set
 *
 * @brief:    Record that the wall clock was set
 *
 * @return:   JRNL_OK, or an error code
 *
 *----------------------------------------------------------------
 *
 * unix_time is the time the clock was set to, and uptime the seconds
 * since power up at that moment.  Entries written since power up
 * that were stamped with seconds since power up can then be given a
 * Unix time by journal_entry_time().
 *
 *--------------------------------------------------------------*/
int journal_clock_set(journal_t *j, uint32_t unix_time, uint32_t uptime, uint8_t source)
{
  jrnl_entry_t entry;
  int          status;

  if ( (j == NULL) || !j->is_open )
  {
    return JRNL_ERR_NOT_OPEN;
  }

  if ( !is_unix_time(unix_time) || (uptime >= JRNL_TIME_MIN) || (source == JRNL_SOURCE_NONE) || (source > JRNL_SOURCE_NTP) )
  {
    return JRNL_ERR_PARAM;
  }

  memset(&entry, 0, sizeof(entry));
  entry.kind           = JRNL_CLOCK_SET;
  entry.session        = j->session;
  entry.shot           = j->session_shots;
  entry.session_type   = j->session_type;
  entry.flags          = (uint8_t)(source << JRNL_SOURCE_SHIFT);
  entry.time           = unix_time;
  entry.clock.boot_seq = j->boot_seq;
  entry.clock.uptime   = uptime;

  status = write_entry(j, &entry);
  remember_clock(j, &entry);                                          // Use it even if the write failed

  return status;
}

/*----------------------------------------------------------------
 *
 * @function: journal_entry_time
 *
 * @brief:    Work out the Unix time of an entry
 *
 * @return:   Unix time, or JRNL_TIME_UNKNOWN
 *
 *----------------------------------------------------------------
 *
 * *source is set to JRNL_SOURCE_xxx (it may be NULL).
 *
 *--------------------------------------------------------------*/
uint32_t journal_entry_time(const journal_t *j, const jrnl_entry_t *entry, uint8_t *source)
{
  const jrnl_clock_t *best = NULL;
  uint32_t            i;
  uint8_t             dummy;

  if ( source == NULL )
  {
    source = &dummy;
  }
  *source = JRNL_SOURCE_NONE;

  if ( entry->time == JRNL_TIME_UNKNOWN )
  {
    return JRNL_TIME_UNKNOWN;
  }

  if ( is_unix_time(entry->time) )
  {
    *source = entry_source(entry);
    return entry->time;
  }

  /*
   * Seconds since power up.  Look for the clock setting made later
   * in the same power up.
   */
  for ( i = 0; i != j->clock_count; i++ )
  {
    if ( (j->clocks[i].boot_seq <= entry->seq) && (entry->seq < j->clocks[i].seq)
         && ((best == NULL) || (j->clocks[i].seq > best->seq)) )     // Use the newest, it is the most accurate
    {
      best = &j->clocks[i];
    }
  }

  if ( best == NULL )
  {
    return JRNL_TIME_UNKNOWN;
  }

  *source = best->source;
  return entry->time + best->offset;
}

/*----------------------------------------------------------------
 *
 * Iterators
 *
 *--------------------------------------------------------------*/
void journal_iter_session(const journal_t *j, jrnl_iter_t *it)
{
  memset(it, 0, sizeof(*it));
  it->slot    = oldest_slot(j);
  it->session = j->session;
  it->kind    = JRNL_SHOT;

  if ( !j->is_open || (j->session == 0) )                             // Nothing to replay
  {
    it->visited = j->slots;
  }
}

void journal_iter_after(const journal_t *j, jrnl_iter_t *it, uint32_t after_seq)
{
  memset(it, 0, sizeof(*it));
  it->slot     = oldest_slot(j);
  it->last_seq = after_seq;

  if ( !j->is_open )
  {
    it->visited = j->slots;
  }
}

/*----------------------------------------------------------------
 *
 * @function: journal_iter_follow
 *
 * @brief:    Let an iterator that has caught up continue later
 *
 *----------------------------------------------------------------
 *
 * Once journal_next() returns false the iterator is finished.  Call
 * this before the next journal_next() to pick up the entries that were
 * appended since; nothing already returned is returned again.  Used to
 * stream shots to a client as they arrive.
 *
 * If more than a whole journal of entries is appended in between, the
 * oldest of them have been overwritten and are skipped.
 *
 *--------------------------------------------------------------*/
void journal_iter_follow(const journal_t *j, jrnl_iter_t *it)
{
  if ( j->is_open && (it->visited >= j->slots) )
  {
    it->visited = 0;
  }
}

/*----------------------------------------------------------------
 *
 * @function: journal_next
 *
 * @brief:    Return the next matching entry in seq order
 *
 * @return:   true if an entry was returned
 *
 *----------------------------------------------------------------
 *
 * Entries are visited from the oldest sector up to the head.  It is
 * safe to append between calls: an entry is only returned if its seq
 * is higher than the previous one, and at most one pass is made over
 * the storage.
 *
 *--------------------------------------------------------------*/
bool journal_next(const journal_t *j, jrnl_iter_t *it, jrnl_entry_t *entry)
{
  uint32_t slot;

  while ( it->visited < j->slots )
  {
    slot = it->slot;
    if ( slot == j->head )                                            // Caught up with the writer
    {
      it->visited = j->slots;
      break;
    }

    it->slot = (slot + 1) % j->slots;
    it->visited++;

    if ( (read_slot(j, slot, entry) != 0) || !journal_entry_valid(entry) )
    {
      continue;
    }

    if ( (entry->seq <= it->last_seq)                                 // Already returned, or older
         || ((it->session != 0) && (entry->session != it->session))   // Wrong session
         || ((it->kind != 0) && (entry->kind != it->kind)) )          // Wrong kind
    {
      continue;
    }

    it->last_seq = entry->seq;
    return true;
  }

  return false;
}
