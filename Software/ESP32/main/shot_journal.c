/*----------------------------------------------------------------
 *
 * shot_journal.c
 *
 * ESP32 binding of the shot journal (journal.c)
 *
 *----------------------------------------------------------------
 *
 * The storage is the "shots" data partition (see partitions.csv).
 * All access goes through one mutex so that the target loop, the
 * timed fire timer and the connection tasks can share the journal.
 * The mutex is released between replayed shots so a slow client
 * never holds up the scoring of a new shot.
 *
 * Flash writes are done from reduce(), before the target is armed
 * again, so the short time that interrupts are held off while the
 * flash is busy cannot cost a shot.
 *
 *--------------------------------------------------------------*/
#include <stdlib.h>
#include <string.h>

#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "freETarget.h"
#include "diag_tools.h"
#include "json.h"
#include "journal.h"
#include "serial_io.h"
#include "shot_journal.h"
#include "wall_clock.h"
#include "helpers.h"

#define PARTITION_LABEL "shots"         // Name of the partition in partitions.csv
#define RAM_SECTORS     4               // Size of the fallback journal (256 entries)
#define SECTOR_SIZE     4096            // Flash erase sector

static journal_t               journal;             // Journal state
static const esp_partition_t  *partition;           // Flash partition, NULL if not present
static uint8_t                *ram_store;           // Fallback storage if there is no partition
static SemaphoreHandle_t       lock;                // Serializes access to the journal
static StaticSemaphore_t       lock_buffer;
static bool                    resumed;             // A session was recovered at start up
static unsigned int            unjournaled_shots;   // Shot counter if the journal could not be opened
static shot_journal_notify_t   notify;              // Told about every new entry
static uint32_t                last_activity;       // Uptime (s) of the latest shot or session change in this power up
static bool                    active_this_boot;    // There was some

/*----------------------------------------------------------------
 *
 * Storage drivers
 *
 *--------------------------------------------------------------*/
static int flash_read(void *ctx, uint32_t offset, void *buffer, size_t length)
{
  return (esp_partition_read((const esp_partition_t *)ctx, offset, buffer, length) == ESP_OK) ? 0 : -1;
}

static int flash_write(void *ctx, uint32_t offset, const void *buffer, size_t length)
{
  return (esp_partition_write((const esp_partition_t *)ctx, offset, buffer, length) == ESP_OK) ? 0 : -1;
}

static int flash_erase(void *ctx, uint32_t offset, size_t length)
{
  return (esp_partition_erase_range((const esp_partition_t *)ctx, offset, length) == ESP_OK) ? 0 : -1;
}

static int ram_read(void *ctx, uint32_t offset, void *buffer, size_t length)
{
  memcpy(buffer, (uint8_t *)ctx + offset, length);
  return 0;
}

static int ram_write(void *ctx, uint32_t offset, const void *buffer, size_t length)
{
  uint8_t       *to   = (uint8_t *)ctx + offset;
  const uint8_t *from = (const uint8_t *)buffer;

  for ( size_t i = 0; i != length; i++ )
  {
    to[i] &= from[i];                   // Behave like flash
  }
  return 0;
}

static int ram_erase(void *ctx, uint32_t offset, size_t length)
{
  memset((uint8_t *)ctx + offset, 0xFF, length);
  return 0;
}

static uint32_t journal_now(void *ctx, uint8_t *source)
{
  (void)ctx;
  return wall_clock_now(source);
}

static void take(void)
{
  xSemaphoreTake(lock, portMAX_DELAY);
}

static void give(void)
{
  xSemaphoreGive(lock);
}

static void note_activity(void)         // A shot or session change: restart the idle time
{
  last_activity    = wall_clock_uptime();
  active_this_boot = true;
}

static void give_and_notify(void)       // Release the journal after writing, then tell the listener
{
  xSemaphoreGive(lock);
  if ( notify != NULL )
  {
    notify();
  }
}

/*
 * Turn a journal entry back into a shot record
 */
static void entry_to_shot(const jrnl_entry_t *entry, uint32_t time, uint8_t source, shot_record_t *shot)
{
  memset(shot, 0, sizeof(*shot));
  shot->shot          = entry->shot;
  shot->seq           = entry->seq;
  shot->session       = entry->session;
  shot->session_type  = entry->session_type;
  shot->miss          = (entry->flags & JRNL_FLAG_MISS) != 0;
  shot->utc           = (time == JRNL_TIME_UNKNOWN) ? 0 : time;
  shot->clock         = source;
  shot->shot_time     = entry->shot_time;
  shot->x_mm          = entry->x_mm;
  shot->y_mm          = entry->y_mm;
  shot->radius        = entry->radius;
  shot->angle         = entry->angle;
  shot->face_strike   = entry->face_strike;
  shot->sensor_status = entry->sensor_status;
  for ( int i = N; i <= W; i++ )
  {
    shot->timer_count[i] = entry->timer_count[i];
  }
}

/*----------------------------------------------------------------
 *
 * @function: shot_journal_init
 *
 * @brief:    Find the storage and recover the journal
 *
 *--------------------------------------------------------------*/
void shot_journal_init(void)
{
  jrnl_flash_t driver;
  int          status;

  lock = xSemaphoreCreateMutexStatic(&lock_buffer);

  partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, PARTITION_LABEL);
  if ( partition != NULL )
  {
    driver.read        = flash_read;
    driver.write       = flash_write;
    driver.erase       = flash_erase;
    driver.ctx         = (void *)partition;
    driver.sector_size = SECTOR_SIZE;
    driver.size        = partition->size - (partition->size % SECTOR_SIZE);
  }
  else
  {
    DLT(DLT_CRITICAL, SEND(CONSOLE, sprintf(_xs, "No \"%s\" partition: shots are kept in RAM and lost on power off. Reflash over USB to fix.",
                                            PARTITION_LABEL);))
    ram_store = malloc(RAM_SECTORS * SECTOR_SIZE);
    if ( ram_store == NULL )
    {
      DLT(DLT_CRITICAL, SEND(CONSOLE, sprintf(_xs, "Shot journal disabled: out of memory");))
      return;
    }
    memset(ram_store, 0xFF, RAM_SECTORS * SECTOR_SIZE);
    driver.read        = ram_read;
    driver.write       = ram_write;
    driver.erase       = ram_erase;
    driver.ctx         = ram_store;
    driver.sector_size = SECTOR_SIZE;
    driver.size        = RAM_SECTORS * SECTOR_SIZE;
  }

  take();
  status = journal_open(&journal, &driver);
  journal_set_time_source(&journal, journal_now, NULL);
  give();

  if ( status != JRNL_OK )
  {
    DLT(DLT_CRITICAL, SEND(CONSOLE, sprintf(_xs, "Shot journal could not be opened (%d)", status);))
    return;
  }

  resumed = (journal.session != 0) && journal.session_open;   // An ended session is not resumed
  DLT(DLT_INFO, SEND(CONSOLE, sprintf(_xs, "Shot journal: %s, %lu entries, %lu damaged, session %lu with %u shots, next seq %lu",
                                      (partition != NULL) ? "flash" : "RAM", (unsigned long)journal.entries,
                                      (unsigned long)journal.bad_entries, (unsigned long)journal.session,
                                      (unsigned int)journal.session_shots, (unsigned long)journal.next_seq);))
}

/*----------------------------------------------------------------
 *
 * @function: shot_journal_resume
 *
 * @brief:    Pick up the session that was running before a restart
 *
 * @return:   true if a session was recovered
 *
 *----------------------------------------------------------------
 *
 * The session type (sighters or match) is restored as well so that
 * new shots carry on in the same mode.
 *
 *--------------------------------------------------------------*/
bool shot_journal_resume(void)
{
  if ( !resumed )
  {
    return false;
  }

  json_session_type = shot_journal_session_type();

  return true;
}

int shot_journal_session_type(void)
{
  return journal.session_type & ~SESSION_VALID;
}

/*----------------------------------------------------------------
 *
 * @function: shot_journal_record
 *
 * @brief:    Save a scored shot before it is reported
 *
 *----------------------------------------------------------------
 *
 * Fills in shot->shot (the shot number within the session),
 * shot->seq (the journal sequence number) and shot->utc / clock (the
 * time of day, if known) so that the score sent to the clients
 * matches what was saved.
 *
 *--------------------------------------------------------------*/
void shot_journal_record(shot_record_t *shot)
{
  jrnl_entry_t entry;
  int          status;
  uint8_t      source;
  uint32_t     time;

  if ( !journal.is_open )
  {
    time        = wall_clock_now(&source);
    shot->shot  = unjournaled_shots++;
    shot->seq   = 0;
    shot->utc   = (source == JRNL_SOURCE_NONE) ? 0 : time;
    shot->clock = source;
    return;
  }

  memset(&entry, 0, sizeof(entry));
  entry.session_type = (uint8_t)shot->session_type;
  entry.flags        = (shot->miss != 0) ? JRNL_FLAG_MISS : 0;
  entry.shot_time    = (uint32_t)shot->shot_time;
  entry.x_mm         = (float)shot->x_mm;
  entry.y_mm         = (float)shot->y_mm;
  entry.radius       = (float)shot->radius;
  entry.angle        = (float)shot->angle;
  entry.face_strike  = (uint16_t)shot->face_strike;
  entry.sensor_status = (uint16_t)shot->sensor_status;
  for ( int i = N; i <= W; i++ )
  {
    entry.timer_count[i] = shot->timer_count[i];
  }

  take();
  if ( !journal.session_open )                       // This shot starts a new session: record its target face
  {
    journal_set_target(&journal, (uint16_t)http_target_type());
  }
  status = journal_append_shot(&journal, &entry);
  time   = journal_entry_time(&journal, &entry, &source);
  give_and_notify();
  note_activity();

  shot->shot    = entry.shot;
  shot->seq     = entry.seq;
  shot->session = entry.session;
  shot->utc   = (time == JRNL_TIME_UNKNOWN) ? 0 : time;
  shot->clock = source;

  if ( status != JRNL_OK )
  {
    DLT(DLT_CRITICAL, SEND(CONSOLE, sprintf(_xs, "Shot %lu could not be saved (%d)", (unsigned long)entry.seq, status);))
  }
}

/*----------------------------------------------------------------
 *
 * @function: shot_journal_new_session / shot_journal_set_mode
 *
 *--------------------------------------------------------------*/
void shot_journal_new_session(int session_type)
{
  int status;

  unjournaled_shots = 0;
  if ( !journal.is_open )
  {
    return;
  }

  take();
  journal_set_target(&journal, (uint16_t)http_target_type()); // Remember which target face this session uses
  status = journal_new_session(&journal, (uint8_t)session_type);
  give_and_notify();
  note_activity();

  if ( status != JRNL_OK )
  {
    DLT(DLT_CRITICAL, SEND(CONSOLE, sprintf(_xs, "New session could not be saved (%d)", status);))
  }
}

void shot_journal_set_mode(int session_type)
{
  int status;

  if ( !journal.is_open )
  {
    return;
  }

  take();
  journal_set_target(&journal, (uint16_t)http_target_type());
  status = journal_set_mode(&journal, (uint8_t)session_type);
  give_and_notify();
  note_activity();

  if ( status != JRNL_OK )
  {
    DLT(DLT_CRITICAL, SEND(CONSOLE, sprintf(_xs, "Session type could not be saved (%d)", status);))
  }
}

/*----------------------------------------------------------------
 *
 * @function: shot_journal_end_session
 *
 * @brief:    End the current session
 *
 *----------------------------------------------------------------
 *
 * The next shot starts a new session (as practice), so the next
 * athlete never continues this one.  Clients that connect later get
 * no replay of it.
 *
 *--------------------------------------------------------------*/
void shot_journal_end_session(int reason)
{
  int status;

  json_session_type = SESSION_EMPTY;     // The next session starts as practice
  json_athlete[0]   = 0;                 // and does not carry this athlete's name
  unjournaled_shots = 0;
  if ( !journal.is_open || !journal.session_open )
  {
    return;
  }

  take();
  status = journal_end_session(&journal, (uint8_t)reason);
  give_and_notify();

  DLT(DLT_INFO, SEND(CONSOLE, sprintf(_xs, "Session %lu ended (%s)", (unsigned long)journal.session,
                                      (reason == JRNL_END_IDLE) ? "no activity" : "by a client");))
  if ( status != JRNL_OK )
  {
    DLT(DLT_CRITICAL, SEND(CONSOLE, sprintf(_xs, "Session end could not be saved (%d)", status);))
  }
}

bool shot_journal_session_open(void)
{
  return journal.is_open && journal.session_open;
}

/*----------------------------------------------------------------
 *
 * @function: shot_journal_tick
 *
 * @brief:    End the session after SESSION_IDLE_S without activity
 *
 *----------------------------------------------------------------
 *
 * Within this power up the idle time is measured with the uptime.  For
 * a session resumed at power up it is at least the uptime, and more if
 * the clock shows the last entry was written long before.
 *
 *--------------------------------------------------------------*/
void shot_journal_tick(void)
{
  uint32_t uptime = wall_clock_uptime();
  uint32_t idle;
  uint32_t last;
  uint32_t now;
  uint8_t  source;

  if ( !shot_journal_session_open() )
  {
    return;
  }

  if ( active_this_boot )
  {
    idle = uptime - last_activity;
  }
  else
  {
    idle = uptime;
    take();
    last = journal_entry_time(&journal, &journal.newest, NULL);
    give();
    now = wall_clock_now(&source);
    if ( (source != JRNL_SOURCE_NONE) && (last != JRNL_TIME_UNKNOWN) && (now > last) && ((now - last) > idle) )
    {
      idle = now - last;
    }
  }

  if ( idle >= SESSION_IDLE_S )
  {
    shot_journal_end_session(JRNL_END_IDLE);
  }
}

/*----------------------------------------------------------------
 *
 * @function: shot_journal_clock_set
 *
 * @brief:    Record that the wall clock was set (see wall_clock.c)
 *
 *--------------------------------------------------------------*/
void shot_journal_clock_set(uint32_t unix_time, uint32_t uptime, int source)
{
  int status;

  if ( !journal.is_open )
  {
    return;
  }

  take();
  status = journal_clock_set(&journal, unix_time, uptime, (uint8_t)source);
  give();

  if ( status != JRNL_OK )
  {
    DLT(DLT_CRITICAL, SEND(CONSOLE, sprintf(_xs, "Clock setting could not be saved (%d)", status);))
  }
}

uint32_t shot_journal_last_ntp_time(void)
{
  return journal.is_open ? journal.last_ntp_time : 0;
}

/*----------------------------------------------------------------
 *
 * @function: shot_journal_replay_session
 *
 * @brief:    Call f() for every shot of the current session, oldest first
 *
 * @return:   Number of shots replayed
 *
 *--------------------------------------------------------------*/
int shot_journal_replay_session(shot_journal_cb_t f, void *ctx)
{
  jrnl_iter_t   it;
  jrnl_entry_t  entry;
  shot_record_t shot;
  bool          more;
  int           count  = 0;
  uint32_t      time   = JRNL_TIME_UNKNOWN;
  uint8_t       source = JRNL_SOURCE_NONE;

  if ( !journal.is_open || !journal.session_open )   // An ended session belongs to the last athlete: replay nothing
  {
    return 0;
  }

  take();
  journal_iter_session(&journal, &it);
  give();

  while ( 1 )
  {
    take();
    more = journal_next(&journal, &it, &entry);
    if ( more )
    {
      time = journal_entry_time(&journal, &entry, &source);  // Shots fired before the clock was set get their time here
    }
    give();                             // Do not hold the journal while talking to a client

    if ( !more )
    {
      break;
    }

    entry_to_shot(&entry, time, source, &shot);

    count++;
    if ( !f(&shot, ctx) )
    {
      break;
    }
  }

  return count;
}

/*----------------------------------------------------------------
 *
 * Cursors (used by the WebSocket API)
 *
 *----------------------------------------------------------------
 *
 * The lock is only held while one entry is read, so a slow client
 * never holds up the scoring of a new shot.
 *
 *--------------------------------------------------------------*/
void shot_journal_set_notify(shot_journal_notify_t f)
{
  notify = f;
}

unsigned int shot_journal_session(void)
{
  return journal.is_open ? journal.session : 0;
}

unsigned int shot_journal_next_seq(void)
{
  return journal.is_open ? journal.next_seq : 0;
}

void shot_journal_cursor_start(shot_journal_cursor_t *c, unsigned int after_seq)
{
  memset(c, 0, sizeof(*c));
  if ( !journal.is_open )
  {
    return;
  }

  take();
  if ( (after_seq == 0) && (journal.session != 0) )
  {
    after_seq = journal.session_start_seq - 1;        // From the start of the current session, including its JRNL_SESSION entry
  }
  journal_iter_after(&journal, &c->it, after_seq);
  give();
}

void shot_journal_cursor_start_all(shot_journal_cursor_t *c)
{
  memset(c, 0, sizeof(*c));
  if ( journal.is_open )
  {
    take();
    journal_iter_after(&journal, &c->it, 0);
    give();
  }
}

bool shot_journal_cursor_next(shot_journal_cursor_t *c, shot_journal_event_t *ev)
{
  jrnl_entry_t entry;
  uint32_t     time   = JRNL_TIME_UNKNOWN;
  uint8_t      source = JRNL_SOURCE_NONE;
  bool         more;

  if ( !journal.is_open )
  {
    return false;
  }

  while ( 1 )
  {
    take();
    journal_iter_follow(&journal, &c->it);            // Pick up anything written since the last call
    more = journal_next(&journal, &c->it, &entry);
    if ( more )
    {
      time = journal_entry_time(&journal, &entry, &source);
    }
    give();

    if ( !more )
    {
      return false;
    }

    if ( (entry.kind == JRNL_SHOT) || (entry.kind == JRNL_SESSION) || (entry.kind == JRNL_MODE) || (entry.kind == JRNL_END) )
    {
      break;                                          // Clock settings are not reported
    }
  }

  ev->kind         = entry.kind;
  ev->seq          = entry.seq;
  ev->session      = entry.session;
  ev->session_type = entry.session_type & ~SESSION_VALID;
  ev->target       = (entry.kind == JRNL_SHOT) ? 0 : entry.info.target;
  ev->reason       = (entry.kind == JRNL_END) ? entry.info.reason : 0;
  ev->utc          = (time == JRNL_TIME_UNKNOWN) ? 0 : time;
  if ( entry.kind == JRNL_SHOT )
  {
    entry_to_shot(&entry, time, source, &ev->shot);
  }

  return true;
}

/*----------------------------------------------------------------
 *
 * @function: shot_journal_sessions
 *
 * @brief:    Summarise the sessions still in the journal
 *
 * @return:   Number of sessions in list (the newest max), oldest first
 *
 *----------------------------------------------------------------
 *
 * Reads the whole journal, one entry at a time, so a new shot is
 * never held up for long.
 *
 *--------------------------------------------------------------*/
int shot_journal_sessions(shot_journal_summary_t *list, int max)
{
  shot_journal_cursor_t   cursor;
  shot_journal_event_t   *ev;
  shot_journal_summary_t *s = NULL;
  int                     count = 0;
  int                     first = 0;                    // Ring index of the oldest summary kept

  if ( !journal.is_open || (max <= 0) )
  {
    return 0;
  }

  ev = malloc(sizeof(*ev));
  if ( ev == NULL )
  {
    return 0;
  }

  shot_journal_cursor_start_all(&cursor);             // Everything still in the journal

  while ( shot_journal_cursor_next(&cursor, ev) )
  {
    if ( ev->session == 0 )
    {
      continue;
    }

    if ( (s == NULL) || (s->session != ev->session) )  // A new session
    {
      if ( count < max )
      {
        s = &list[(first + count) % max];
        count++;
      }
      else
      {
        s     = &list[first];                           // Full, forget the oldest
        first = (first + 1) % max;
      }
      memset(s, 0, sizeof(*s));
      s->session   = ev->session;
      s->first_seq = ev->seq;
      s->first_utc = ev->utc;
    }

    s->last_seq     = ev->seq;
    s->session_type = ev->session_type;
    if ( ev->utc != 0 )
    {
      s->last_utc = ev->utc;
      if ( s->first_utc == 0 )
      {
        s->first_utc = ev->utc;
      }
    }

    if ( ev->kind == JRNL_SHOT )
    {
      s->shots++;
      if ( ev->session_type == SESSION_SIGHT )
      {
        s->sighters++;
      }
    }
    else
    {
      if ( ev->target != 0 )
      {
        s->target = ev->target;
      }
      s->ended = (ev->kind == JRNL_END);
    }
  }
  free(ev);

  if ( first != 0 )                                     // Put them in order, oldest first
  {
    shot_journal_summary_t *ordered = malloc(sizeof(*ordered) * count);
    if ( ordered != NULL )
    {
      for ( int i = 0; i != count; i++ )
      {
        ordered[i] = list[(first + i) % max];
      }
      memcpy(list, ordered, sizeof(*ordered) * count);
      free(ordered);
    }
  }

  return count;
}

/*----------------------------------------------------------------
 *
 * @function: shot_journal_show
 *
 * @brief:    Print the journal status as part of {"ECHO":0}
 *
 *--------------------------------------------------------------*/
void shot_journal_show(void)
{
  if ( !journal.is_open )
  {
    SEND(ALL, sprintf(_xs, "\"JOURNAL\":          \"disabled\",");)
    return;
  }

  SEND(ALL, sprintf(_xs, "\"JOURNAL\":          \"%s, %lu slots, session %lu, %u shots, next seq %lu\",",
                    (partition != NULL) ? "flash" : "RAM only", (unsigned long)journal.slots, (unsigned long)journal.session,
                    (unsigned int)journal.session_shots, (unsigned long)journal.next_seq);)
}
