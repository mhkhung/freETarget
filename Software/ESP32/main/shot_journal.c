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

static void take(void)
{
  xSemaphoreTake(lock, portMAX_DELAY);
}

static void give(void)
{
  xSemaphoreGive(lock);
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
  give();

  if ( status != JRNL_OK )
  {
    DLT(DLT_CRITICAL, SEND(CONSOLE, sprintf(_xs, "Shot journal could not be opened (%d)", status);))
    return;
  }

  resumed = (journal.session != 0);
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
 * Fills in shot->shot (the shot number within the session) and
 * shot->seq (the journal sequence number) so that the score sent to
 * the clients matches what was saved.
 *
 *--------------------------------------------------------------*/
void shot_journal_record(shot_record_t *shot)
{
  jrnl_entry_t entry;
  int          status;

  if ( !journal.is_open )
  {
    shot->shot = unjournaled_shots++;
    shot->seq  = 0;
    return;
  }

  memset(&entry, 0, sizeof(entry));
  entry.session_type = (uint8_t)shot->session_type;
  entry.miss         = (uint8_t)(shot->miss != 0);
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
  status = journal_append_shot(&journal, &entry);
  give();

  shot->shot = entry.shot;
  shot->seq  = entry.seq;

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
  status = journal_new_session(&journal, (uint8_t)session_type);
  give();

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
  status = journal_set_mode(&journal, (uint8_t)session_type);
  give();

  if ( status != JRNL_OK )
  {
    DLT(DLT_CRITICAL, SEND(CONSOLE, sprintf(_xs, "Session type could not be saved (%d)", status);))
  }
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
  int           count = 0;

  if ( !journal.is_open )
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
    give();                             // Do not hold the journal while talking to a client

    if ( !more )
    {
      break;
    }

    memset(&shot, 0, sizeof(shot));
    shot.shot          = entry.shot;
    shot.seq           = entry.seq;
    shot.session_type  = entry.session_type;
    shot.miss          = entry.miss;
    shot.shot_time     = entry.shot_time;
    shot.x_mm          = entry.x_mm;
    shot.y_mm          = entry.y_mm;
    shot.radius        = entry.radius;
    shot.angle         = entry.angle;
    shot.face_strike   = entry.face_strike;
    shot.sensor_status = entry.sensor_status;
    for ( int i = N; i <= W; i++ )
    {
      shot.timer_count[i] = entry.timer_count[i];
    }

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
