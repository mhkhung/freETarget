/* Test the actual ESP32 journal binding with the journal's simulated flash. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "serial_io.h"
#define FREETARGET_C
#include "freETarget_host.h"

// Reuse the NOR flash and clock fixtures without running their separate suite.
#define main journal_tests_main
#include "test_journal.c"
#undef main
#include "shot_journal_host.h"

#define DLT(level, statement)

static journal_t journal;
static sim_clock_t clock_state;
static uint32_t last_activity;
static bool active_this_boot;
static unsigned int unjournaled_shots;
static int json_session_type;
static char json_athlete[128];

static void take(void) {}
static void give(void) {}
static void give_and_notify(void) {}
static uint32_t wall_clock_uptime(void) { return clock_state.uptime; }
static uint32_t wall_clock_now(uint8_t *source) { return sim_now(&clock_state, source); }
static int http_target_type(void) { return 100; }

#include "shot_journal_host.inc"

static void restart(sim_flash_t *flash)
{
  power_up(&journal, flash, &clock_state);
  active_this_boot = false;
  last_activity = 0;
}

static void sync_clock(uint32_t unix_time, uint8_t source)
{
  clock_state.offset = unix_time - clock_state.uptime;
  clock_state.source = source;
  shot_journal_clock_set(unix_time, clock_state.uptime, source);
}

static shot_record_t fire(void)
{
  shot_record_t shot = {0};
  shot.session_type = SESSION_VALID;
  shot.x_mm = 1;
  shot_journal_record(&shot);
  return shot;
}

static void test_live_and_history(void)
{
  sim_flash_t *flash = sim_new(4);
  shot_journal_cursor_t fresh, history, reconnect;
  shot_journal_event_t event;
  shot_record_t first, second;
  int shots = 0, ends = 0;

  restart(flash);
  shot_journal_cursor_start(&fresh, 0);
  assert(!shot_journal_cursor_next(&fresh, &event));
  first = fire();
  assert(shot_journal_cursor_next(&fresh, &event) && event.kind == JRNL_SESSION);
  assert(shot_journal_cursor_next(&fresh, &event) && event.shot.seq == first.seq);
  assert(!shot_journal_cursor_next(&fresh, &event));

  shot_journal_end_session(JRNL_END_COMMAND);
  assert(shot_journal_cursor_next(&fresh, &event) && event.kind == JRNL_END); // Existing client sees the end
  shot_journal_cursor_start(&reconnect, first.seq);
  assert(shot_journal_cursor_next(&reconnect, &event) && event.kind == JRNL_END);
  assert(!shot_journal_cursor_next(&reconnect, &event));

  restart(flash);
  shot_journal_cursor_start(&fresh, 0); // A fresh connection after the end must get no replay
  assert(!shot_journal_cursor_next(&fresh, &event));
  sync_clock(1800000000u, JRNL_SOURCE_CLIENT);
  assert(!shot_journal_cursor_next(&fresh, &event)); // Clock traffic does not start a new session

  second = fire();
  assert(second.session == first.session + 1);
  assert(shot_journal_cursor_next(&fresh, &event) && event.kind == JRNL_SESSION);
  assert(shot_journal_cursor_next(&fresh, &event) && event.shot.seq == second.seq);
  assert(!shot_journal_cursor_next(&fresh, &event));

  shot_journal_cursor_start(&fresh, 0); // An open session still replays for a new client
  assert(shot_journal_cursor_next(&fresh, &event) && event.kind == JRNL_SESSION);
  assert(shot_journal_cursor_next(&fresh, &event) && event.shot.seq == second.seq);
  assert(!shot_journal_cursor_next(&fresh, &event));

  shot_journal_cursor_start_all(&history); // Explicit history access still includes ended sessions
  while ( shot_journal_cursor_next(&history, &event) )
  {
    shots += event.kind == JRNL_SHOT;
    ends += event.kind == JRNL_END;
  }
  assert(shots == 2 && ends == 1);
  sim_free(flash);
}

static void test_idle_after_restart(uint8_t source)
{
  sim_flash_t *flash = sim_new(4);

  restart(flash);
  sync_clock(1800000000u, JRNL_SOURCE_NTP);
  fire();
  restart(flash);
  clock_state.uptime = 10;
  sync_clock(1800007200u, source); // Normal startup synchronization, two hours after the last shot
  shot_journal_tick();
  assert(!shot_journal_session_open());
  assert(journal.newest.kind == JRNL_END && journal.newest.info.reason == JRNL_END_IDLE);
  sim_free(flash);
}

static void test_idle_uptime(void)
{
  sim_flash_t *flash = sim_new(4);

  restart(flash);
  fire();
  restart(flash); // Without a trusted old timestamp, wait an hour of uptime
  clock_state.uptime = SESSION_IDLE_S - 1;
  shot_journal_tick();
  assert(shot_journal_session_open());
  clock_state.uptime++;
  shot_journal_tick();
  assert(!shot_journal_session_open());

  clock_state.uptime++;
  fire(); // A new shot resets activity in this boot
  clock_state.uptime += SESSION_IDLE_S - 1;
  sync_clock(1800010000u, JRNL_SOURCE_NTP);
  shot_journal_tick();
  assert(shot_journal_session_open());
  clock_state.uptime++;
  shot_journal_tick();
  assert(!shot_journal_session_open());
  sim_free(flash);
}

int main(void)
{
  test_live_and_history();
  test_idle_after_restart(JRNL_SOURCE_NTP);
  test_idle_after_restart(JRNL_SOURCE_CLIENT);
  test_idle_uptime();
  assert(failures == 0);
  puts("Shot journal: ended-session subscriptions, history and idle expiry after clock sync passed");
  return 0;
}
