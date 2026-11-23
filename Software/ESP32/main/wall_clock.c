/*----------------------------------------------------------------
 *
 * wall_clock.c
 *
 * Time of day for the shot time stamps.  See wall_clock.h
 *
 *----------------------------------------------------------------
 *
 * The time is kept as an offset from esp_timer (microseconds since
 * power up), so it never jumps unless a time source sets it again.
 *
 * Whenever the clock is set, shot_journal_clock_set() records it in
 * the journal.  The journal then fills in the time of the shots that
 * were fired earlier in this power up, before the clock was known.
 *
 *--------------------------------------------------------------*/
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#include "freETarget.h"
#include "diag_tools.h"
#include "journal.h"
#include "serial_io.h"
#include "shot_journal.h"
#include "wall_clock.h"

#define NTP_SERVER   "pool.ntp.org"     // Used if the router does not announce one
#define RESYNC_STEP  2                  // Record an NTP correction bigger than this (seconds)

static portMUX_TYPE clock_lock   = portMUX_INITIALIZER_UNLOCKED;
static uint8_t      clock_source = JRNL_SOURCE_NONE; // Where the time came from
static int64_t      clock_offset;                    // Unix time - uptime, in microseconds

/*----------------------------------------------------------------
 *
 * Helpers
 *
 *--------------------------------------------------------------*/
uint32_t wall_clock_uptime(void)
{
  return (uint32_t)(esp_timer_get_time() / 1000000);
}

const char *wall_clock_source_name(int source)
{
  switch ( source )
  {
    case JRNL_SOURCE_NTP:
      return "ntp";
    case JRNL_SOURCE_CLIENT:
      return "client";
    default:
      return "none";
  }
}

int wall_clock_source(void)
{
  int source;

  taskENTER_CRITICAL(&clock_lock);
  source = clock_source;
  taskEXIT_CRITICAL(&clock_lock);

  return source;
}

/*
 * Days since 1970-01-01 for a date (Howard Hinnant's days_from_civil)
 */
static int32_t days_from_civil(int32_t y, int32_t m, int32_t d)
{
  int32_t era;
  int32_t yoe;
  int32_t doy;
  int32_t doe;

  y  -= (m <= 2);
  era = (y >= 0 ? y : y - 399) / 400;
  yoe = y - era * 400;
  doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;

  return era * 146097 + doe - 719468;
}

/*
 * The earliest time a client may set: the day before this firmware
 * was built, or the newest NTP time in the journal, whichever is later.
 */
static uint32_t earliest_time(void)
{
  static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
  const char       *date     = __DATE__;                             // "Nov 22 2026"
  const char        name[4]  = {date[0], date[1], date[2], 0};
  const char       *month    = strstr(months, name);
  uint32_t          earliest = JRNL_TIME_MIN;
  uint32_t          ntp;

  if ( month != NULL )
  {
    earliest = (uint32_t)(days_from_civil(atoi(&date[7]), (int32_t)(month - months) / 3 + 1, atoi(&date[4])) - 1) * 86400u;
  }

  ntp = shot_journal_last_ntp_time();
  return (ntp > earliest) ? ntp : earliest;
}

/*
 * Set the clock.  Returns true if the journal should be told.
 * A client may only set a clock that has not been set yet; the test
 * and the update are done together so two clients cannot both win.
 */
static bool set_clock(int64_t unix_us, uint8_t source, uint32_t *unix_time, uint32_t *uptime)
{
  int64_t now_us = esp_timer_get_time();
  int64_t offset = unix_us - now_us;
  bool    record;

  taskENTER_CRITICAL(&clock_lock);
  if ( (source == JRNL_SOURCE_CLIENT) && (clock_source != JRNL_SOURCE_NONE) )
  {
    record = false;
  }
  else
  {
    record       = (clock_source != source) || (llabs(offset - clock_offset) > (int64_t)RESYNC_STEP * 1000000);
    clock_source = source;
    clock_offset = offset;
  }
  taskEXIT_CRITICAL(&clock_lock);

  *unix_time = (uint32_t)(unix_us / 1000000);
  *uptime    = (uint32_t)(now_us / 1000000);

  return record;
}

/*----------------------------------------------------------------
 *
 * @function: wall_clock_now
 *
 * @brief:    Time stamp for the journal
 *
 * @return:   Unix time, or seconds since power up if the clock is not set
 *
 *--------------------------------------------------------------*/
uint32_t wall_clock_now(uint8_t *source)
{
  int64_t now_us = esp_timer_get_time();
  int64_t offset;
  uint8_t s;

  taskENTER_CRITICAL(&clock_lock);
  s      = clock_source;
  offset = clock_offset;
  taskEXIT_CRITICAL(&clock_lock);

  if ( source != NULL )
  {
    *source = s;
  }

  if ( s == JRNL_SOURCE_NONE )
  {
    return (uint32_t)(now_us / 1000000);
  }

  return (uint32_t)((now_us + offset) / 1000000);
}

/*----------------------------------------------------------------
 *
 * NTP (station mode only)
 *
 *----------------------------------------------------------------
 *
 * The callback runs in the lwIP task each time the time server
 * answers (once at start up, then every hour).
 *
 *--------------------------------------------------------------*/
static void ntp_synced(struct timeval *tv)
{
  uint32_t unix_time;
  uint32_t uptime;

  if ( (uint32_t)tv->tv_sec < JRNL_TIME_MIN )                        // Nonsense from the server
  {
    return;
  }

  if ( set_clock((int64_t)tv->tv_sec * 1000000 + tv->tv_usec, JRNL_SOURCE_NTP, &unix_time, &uptime) )
  {
    shot_journal_clock_set(unix_time, uptime, JRNL_SOURCE_NTP);
    DLT(DLT_INFO, SEND(CONSOLE, sprintf(_xs, "Clock set by NTP: %lu", (unsigned long)unix_time);))
  }
}

void wall_clock_ntp_init(void)
{
  esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(1, ESP_SNTP_SERVER_LIST(NTP_SERVER));

  config.start                      = true;               // Keeps trying until the network is up
  config.wait_for_sync              = false;
  config.server_from_dhcp           = true;               // Use the router's time server if it has one
  config.renew_servers_after_new_IP = true;               // and keep NTP_SERVER as the second choice
  config.index_of_first_server      = 1;
  config.ip_event_to_renew          = IP_EVENT_STA_GOT_IP;
  config.sync_cb                    = ntp_synced;

  if ( esp_netif_sntp_init(&config) != ESP_OK )
  {
    DLT(DLT_CRITICAL, SEND(CONSOLE, sprintf(_xs, "NTP could not be started");))
  }
}

/*----------------------------------------------------------------
 *
 * @function: wall_clock_from_client
 *
 * @brief:    {"TIME": <Unix seconds>} from a client
 *
 *----------------------------------------------------------------
 *
 * The client's time is a fallback.  It is ignored if NTP or another
 * client already set the clock in this power up, or if it is earlier
 * than a time the target already trusts.  {"TIME":0} only reports.
 *
 * Reply:  {"TIME": 1795000000, "CLOCK": "ntp"}   ("TIME": 0 if not set)
 *
 *--------------------------------------------------------------*/
void wall_clock_from_client(int unix_time)
{
  uint32_t    t = (uint32_t)unix_time;
  uint32_t    set_time;
  uint32_t    uptime;
  uint8_t     source;
  const char *ignored = NULL;

  if ( unix_time > 0 )
  {
    source = (uint8_t)wall_clock_source();
    if ( source == JRNL_SOURCE_NTP )
    {
      ignored = "the clock was set by NTP";
    }
    else if ( source == JRNL_SOURCE_CLIENT )
    {
      ignored = "the clock was already set by a client";
    }
    else if ( t < earliest_time() )
    {
      ignored = "the time is too early";
    }
    else
    {
      struct timeval tv = {.tv_sec = (time_t)t, .tv_usec = 0};

      if ( set_clock((int64_t)t * 1000000, JRNL_SOURCE_CLIENT, &set_time, &uptime) )
      {
        settimeofday(&tv, NULL);
        shot_journal_clock_set(set_time, uptime, JRNL_SOURCE_CLIENT);
        DLT(DLT_INFO, SEND(CONSOLE, sprintf(_xs, "Clock set by a client: %lu", (unsigned long)set_time);))
      }
      else
      {
        ignored = "the clock was set at the same time by another source";
      }
    }

    if ( ignored != NULL )
    {
      DLT(DLT_INFO, SEND(CONSOLE, sprintf(_xs, "{\"TIME\":%lu} ignored, %s", (unsigned long)t, ignored);))
    }
  }

  t = wall_clock_now(&source);
  SEND(ALL, sprintf(_xs, "\r\n{\"TIME\": %lu, \"CLOCK\": \"%s\"}\r\n", (source == JRNL_SOURCE_NONE) ? 0ul : (unsigned long)t,
                    wall_clock_source_name(source));)
}

/*----------------------------------------------------------------
 *
 * @function: wall_clock_show
 *
 * @brief:    Print the clock status as part of {"ECHO":0}
 *
 *--------------------------------------------------------------*/
void wall_clock_show(void)
{
  uint8_t   source;
  time_t    now = (time_t)wall_clock_now(&source);
  struct tm utc;

  if ( source == JRNL_SOURCE_NONE )
  {
    SEND(ALL, sprintf(_xs, "\"CLOCK\":            \"not set\",");)
    return;
  }

  gmtime_r(&now, &utc);
  SEND(ALL, sprintf(_xs, "\"CLOCK\":            \"%04d-%02d-%02d %02d:%02d:%02d UTC, from %s\",", utc.tm_year + 1900, utc.tm_mon + 1,
                    utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec, wall_clock_source_name(source));)
}
