#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "serial_io.h"
#define FREETARGET_C
#include "freETarget_host.h"

#define DLT(level, statement)
#define LED_RAPID_OFF "off"
#define LED_RAPID_WARN "warn"
#define LED_RAPID_ON "on"
#define LED_TABATA_OFF "off"
#define LED_TABATA_WARN "warn"
#define LED_TABATA_ON "on"
#define LED_MISS "miss"
#define NOT_MISSED_SHOT false
#define MISSED_SHOT true
#define REMOTE_MODE_CLIENT 1
#define METHOD_POST 1
#define START 0

static real_t json_rapid_wait = 7, json_rapid_time = 3;
static int json_rapid_count = 5, json_rapid_enable, json_session_type, json_LED_PWM;
static char json_event[64];
static unsigned int misses;
static unsigned int journaled;
static int json_max_radius, json_remote_modes, json_paper_time, json_step_count, json_paper_eco, json_paper_shot;
static real_t json_sensor_dia = 232;
static char json_remote_url[128];
static unsigned int location, computed_location;

static int run_time_ms(void) { return 0; }
static void event_override(void) {}
static bool contains(const char *text, const char *part) { return strstr(text, part) != NULL; }
static void set_LED_PWM_now(int brightness) { (void)brightness; }
static void set_status_LED(char *status) { (void)status; }

static void shot_journal_record(shot_record_t *shot)
{
  journaled++;
  if ( shot->miss ) misses++;
}

static void build_json_score(shot_record_t *shot, const char *fields, char *out, size_t size)
{
  (void)shot;
  (void)fields;
  (void)size;
  out[0] = 0;
}

void serial_to_all(char *text, int ports)
{
  (void)text;
  (void)ports;
}

#include "timed_fire_host.inc"

static unsigned int compute_hit(shot_record_t *shot) { (void)shot; return computed_location; }
static void prepare_score(shot_record_t *shot, unsigned int number, bool miss) { (void)shot; (void)number; (void)miss; }
static void show_sensor_fault(unsigned int status) { (void)status; }
static void paper_start(void) {}
static void vTaskDelay(int ticks) { (void)ticks; ring_timer = 0; }
static double degrees_to_radians(double angle) { return angle * PI / 180; }
static void http_native_request(char *url, int method, char *score, size_t size)
{
  (void)url; (void)method; (void)score; (void)size;
}
static int json_send_miss = 1;

#include "reduction_host.inc"

static void next_state(void)
{
  event_timer = 0;
  timed_event_task();
}

static void start_sport(void)
{
  run_state = IN_OPERATION | IN_SHOT; // Ordinary shooting before enabling the string
  shot_in = shot_out = 0;
  misses = 0;
  journaled = 0;
  json_rapid_count = 5;
  json_rapid_wait = 7;
  json_rapid_time = 50;
  json_rapid_enable = 1;
  last_enable = 0;
  event_timer = 0;
  strcpy(json_event, "SPP");
  next_state(); // SPORT_ENABLED
  assert(misses == 0); // Leaving ordinary shooting must not create a timed miss
  next_state(); // SPORT_WAIT_1
  next_state(); // SPORT_ON_1
  assert(run_state & IN_SHOT);
}

static void close_window(void)
{
  next_state(); // Grace period still accepts shots
  assert(run_state & IN_SHOT);
  next_state(); // SPORT_WAIT_2
  assert(!(run_state & IN_SHOT));
}

int main(void)
{
  start_sport();
  close_window();
  assert(misses == 1);

  start_sport();
  memset(&record[0], 0, sizeof(record[0]));
  record[0].sensor_status = 0x0f;
  record[0].radius = 1000; // All sensors triggered, but this shot is on another lane
  computed_location = 0;
  shot_in = 1;
  run_state |= IN_AQUIRE;
  reduce();
  assert(journaled == 0);
  close_window();
  assert(misses == 1);

  start_sport();
  run_state |= IN_AQUIRE; // Neighbouring trigger acquired, then rejected by reduce()
  close_window();
  assert(misses == 1);

  start_sport();
  timed_fire_scored();
  run_state |= IN_AQUIRE; // A rejected trigger must not erase an earlier valid shot
  close_window();
  assert(misses == 0);
  next_state(); // Next window must have its own shot count
  close_window();
  assert(misses == 1);

  start_sport();
  next_state(); // Grace
  timed_fire_scored(); // A valid shot in the grace period
  next_state(); // Closed
  assert(misses == 0);

  start_sport();
  shot_in = 1; // Shot captured just before closing, still waiting for reduction
  close_window();
  assert(misses == 0);
  next_state(); // Do not open the next window while this one is unresolved
  assert(!(run_state & IN_SHOT));
  assert(misses == 0);
  memset(&record[0], 0, sizeof(record[0]));
  record[0].sensor_status = 0x0f;
  reduce(); // Accept the shot captured before closing, even though the window is now closed
  next_state();
  assert(misses == 0);
  assert(run_state & IN_SHOT);

  start_sport();
  shot_in = 1;
  close_window();
  shot_out = 1; // reduce() rejected the pending neighbouring trigger
  timed_event_task(); // Resolve even while the rest timer is still running
  assert(misses == 1);
  timed_event_task();
  assert(misses == 1); // Never report the same window twice

  start_sport();
  timed_fire_scored(); // A reported sensor miss also counts as a shot
  close_window();
  assert(misses == 0);

  start_sport();
  shot_in = 1;
  close_window();
  memset(&record[0], 0, sizeof(record[0]));
  record[0].sensor_status = 0x0f;
  record[0].miss = 1; // An out-of-window sensor miss must not count for this string
  computed_location = MISS;
  reduce();
  assert(journaled == 0);
  timed_event_task();
  assert(misses == 1);
  computed_location = 0;

  // A complete sport string still produces exactly five entries.
  start_sport();
  for ( int n = 0; n != 5; n++ )
  {
    run_state |= IN_AQUIRE; // Every window receives a rejected neighbouring trigger
    close_window();
    assert(misses == (unsigned int)n + 1);
    if ( n != 4 ) next_state();
  }
  next_state(); // ALL_DONE
  next_state(); // SPORT_IDLE
  assert(!(run_state & IN_RAPID));
  assert(misses == 5);

  // Rapid-fire mode reports missing shots at the end of the string.
  run_state = IN_OPERATION | IN_SHOT;
  json_rapid_enable = 1;
  strcpy(json_event, "RFP");
  misses = 0;
  next_state();
  next_state();
  next_state();
  timed_fire_scored();
  timed_fire_scored();
  next_state();
  next_state();
  assert(misses == 0);
  next_state();
  next_state();
  assert(misses == 3);

  puts("Timed fire: rejected triggers, per-window counts, grace, pending reductions and rapid strings passed");
  return 0;
}
