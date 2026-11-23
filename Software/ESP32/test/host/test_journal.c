/*----------------------------------------------------------------
 *
 * test_journal.c
 *
 * Host unit tests for main/journal.c.  Run with "make" in this
 * directory.
 *
 * The flash is simulated with NOR semantics: a write can only clear
 * bits and an erase sets a whole sector back to 0xFF.  A "power cut"
 * can be scheduled after any number of bytes have been written or
 * erased; the operation in progress then stops part way and every
 * later operation fails until the test "reboots" by reopening the
 * journal on the same memory.
 *
 *--------------------------------------------------------------*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "journal.h"

/*
 * Test bookkeeping
 */
static int failures = 0;
static int checks   = 0;

#define CHECK(cond)                                                          \
  do                                                                         \
  {                                                                          \
    checks++;                                                                \
    if ( !(cond) )                                                           \
    {                                                                        \
      failures++;                                                            \
      printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);               \
    }                                                                        \
  } while ( 0 )

/*
 * Simulated NOR flash
 */
#define SECTOR 4096

typedef struct
{
  uint8_t *mem;
  uint32_t size;
  long     budget;    // Bytes left before the power cut, -1 = no cut
  bool     dead;      // Power has been cut
  unsigned erases;    // Sector erases performed
} sim_flash_t;

static bool spend(sim_flash_t *f)                // true if this byte may be changed
{
  if ( f->dead )
  {
    return false;
  }
  if ( f->budget == 0 )
  {
    f->dead = true;
    return false;
  }
  if ( f->budget > 0 )
  {
    f->budget--;
  }
  return true;
}

static int sim_read(void *ctx, uint32_t offset, void *buffer, size_t length)
{
  sim_flash_t *f = ctx;

  if ( f->dead || (offset + length > f->size) )
  {
    return -1;
  }
  memcpy(buffer, f->mem + offset, length);
  return 0;
}

static int sim_write(void *ctx, uint32_t offset, const void *buffer, size_t length)
{
  sim_flash_t   *f = ctx;
  const uint8_t *p = buffer;

  if ( offset + length > f->size )
  {
    return -1;
  }
  for ( size_t i = 0; i != length; i++ )
  {
    if ( !spend(f) )
    {
      return -1;
    }
    f->mem[offset + i] &= p[i];                  // NOR: bits can only be cleared
  }
  return 0;
}

static int sim_erase(void *ctx, uint32_t offset, size_t length)
{
  sim_flash_t *f = ctx;

  if ( ((offset % SECTOR) != 0) || ((length % SECTOR) != 0) || (offset + length > f->size) )
  {
    return -1;
  }
  for ( size_t i = 0; i != length; i++ )
  {
    if ( !spend(f) )
    {
      return -1;
    }
    f->mem[offset + i] = 0xFF;
  }
  f->erases++;
  return 0;
}

static sim_flash_t *sim_new(uint32_t sectors)
{
  sim_flash_t *f = calloc(1, sizeof(*f));

  f->size   = sectors * SECTOR;
  f->mem    = malloc(f->size);
  f->budget = -1;
  memset(f->mem, 0xFF, f->size);                 // Factory fresh
  return f;
}

static void sim_free(sim_flash_t *f)
{
  free(f->mem);
  free(f);
}

static jrnl_flash_t driver(sim_flash_t *f)
{
  jrnl_flash_t d = {sim_read, sim_write, sim_erase, f, f->size, SECTOR};
  return d;
}

static void reboot(journal_t *j, sim_flash_t *f) // Power comes back, reopen the journal
{
  jrnl_flash_t d = driver(f);

  f->dead   = false;
  f->budget = -1;
  CHECK(journal_open(j, &d) == JRNL_OK);
}

static jrnl_entry_t make_shot(int n)
{
  jrnl_entry_t e;

  memset(&e, 0, sizeof(e));
  e.session_type = 1;
  e.x_mm         = (float)n;
  e.y_mm         = (float)(-n);
  e.shot_time    = (uint32_t)n * 1000u;
  return e;
}

/*----------------------------------------------------------------
 * Tests
 *--------------------------------------------------------------*/
static void test_crc(void)
{
  printf("crc32\n");
  CHECK(journal_crc32("123456789", 9) == 0xCBF43926u);  // Standard check value
}

static void test_geometry(void)
{
  journal_t    j;
  sim_flash_t *f = sim_new(4);
  jrnl_flash_t d = driver(f);

  printf("geometry\n");
  d.size = SECTOR;                                      // One sector is not enough
  CHECK(journal_open(&j, &d) == JRNL_ERR_PARAM);
  d.size        = 4 * SECTOR;
  d.sector_size = 100;                                  // Not a multiple of 64
  CHECK(journal_open(&j, &d) == JRNL_ERR_PARAM);
  d = driver(f);
  d.size = 3 * SECTOR + 64;                             // Not whole sectors
  CHECK(journal_open(&j, &d) == JRNL_ERR_PARAM);
  CHECK(journal_open(&j, NULL) == JRNL_ERR_PARAM);
  sim_free(f);
}

static void test_empty(void)
{
  journal_t    j;
  jrnl_iter_t  it;
  jrnl_entry_t e;
  sim_flash_t *f = sim_new(4);

  printf("empty journal\n");
  reboot(&j, f);
  CHECK(j.entries == 0);
  CHECK(j.next_seq == 1);
  CHECK(j.session == 0);
  CHECK(j.session_shots == 0);
  journal_iter_session(&j, &it);
  CHECK(!journal_next(&j, &it, &e));
  journal_iter_after(&j, &it, 0);
  CHECK(!journal_next(&j, &it, &e));
  sim_free(f);
}

static void test_append_and_recover(void)
{
  journal_t    j;
  jrnl_iter_t  it;
  jrnl_entry_t e;
  sim_flash_t *f = sim_new(4);
  int          n;

  printf("append and recover\n");
  reboot(&j, f);

  for ( n = 0; n != 10; n++ )
  {
    e = make_shot(n);
    CHECK(journal_append_shot(&j, &e) == JRNL_OK);
    CHECK(e.shot == n);                                 // Shot numbers start at 0
    CHECK(e.session == 1);                              // First shot starts session 1
  }
  CHECK(e.seq == 11);                                   // seq 1 is the session entry

  reboot(&j, f);
  CHECK(j.session == 1);
  CHECK(j.session_shots == 10);
  CHECK(j.next_seq == 12);
  CHECK(j.entries == 11);
  CHECK(j.bad_entries == 0);

  e = make_shot(10);                                    // Carry on after the reboot
  CHECK(journal_append_shot(&j, &e) == JRNL_OK);
  CHECK(e.shot == 10);
  CHECK(e.seq == 12);

  journal_iter_session(&j, &it);
  for ( n = 0; journal_next(&j, &it, &e); n++ )
  {
    CHECK(e.shot == n);
    CHECK(e.x_mm == (float)n);
    CHECK(e.shot_time == (uint32_t)n * 1000u);
  }
  CHECK(n == 11);
  sim_free(f);
}

static void test_sessions(void)
{
  journal_t    j;
  jrnl_iter_t  it;
  jrnl_entry_t e;
  sim_flash_t *f = sim_new(4);
  int          n;
  uint32_t     last;

  printf("sessions and resume\n");
  reboot(&j, f);

  for ( n = 0; n != 5; n++ )
  {
    e = make_shot(n);
    journal_append_shot(&j, &e);
  }
  last = e.seq;                                         // A client saw up to here

  CHECK(journal_new_session(&j, 2) == JRNL_OK);
  CHECK(j.session == 2);
  CHECK(j.session_shots == 0);
  for ( n = 0; n != 3; n++ )
  {
    e = make_shot(100 + n);
    journal_append_shot(&j, &e);
    CHECK(e.session == 2);
    CHECK(e.shot == n);
  }

  reboot(&j, f);
  CHECK(j.session == 2);
  CHECK(j.session_shots == 3);
  CHECK(j.session_type == 1);                           // From the last shot

  journal_iter_session(&j, &it);                        // Only the new session's shots
  for ( n = 0; journal_next(&j, &it, &e); n++ )
  {
    CHECK(e.session == 2);
    CHECK(e.kind == JRNL_SHOT);
  }
  CHECK(n == 3);

  journal_iter_after(&j, &it, last);                    // Resume: session entry + 3 shots
  CHECK(journal_next(&j, &it, &e) && (e.kind == JRNL_SESSION) && (e.session == 2) && (e.session_type == 2));
  for ( n = 0; journal_next(&j, &it, &e); n++ )
  {
    CHECK(e.kind == JRNL_SHOT);
    CHECK(e.seq > last);
  }
  CHECK(n == 3);

  CHECK(journal_new_session(&j, 0) == JRNL_OK);         // Empty session survives a reboot
  reboot(&j, f);
  CHECK(j.session == 3);
  CHECK(j.session_shots == 0);
  journal_iter_session(&j, &it);
  CHECK(!journal_next(&j, &it, &e));
  sim_free(f);
}

static void test_mode(void)
{
  journal_t    j;
  jrnl_entry_t e;
  sim_flash_t *f = sim_new(4);

  printf("session type changes\n");
  reboot(&j, f);
  CHECK(journal_set_mode(&j, 4) == JRNL_OK);            // No session yet: starts one
  CHECK(j.session == 1);
  e = make_shot(0);
  e.session_type = 5;
  journal_append_shot(&j, &e);
  CHECK(journal_set_mode(&j, 2) == JRNL_OK);
  reboot(&j, f);
  CHECK(j.session == 1);
  CHECK(j.session_type == 2);                           // Mode entry is the newest
  CHECK(j.session_shots == 1);                          // Mode entries are not shots
  sim_free(f);
}

static void test_wraparound(void)
{
  journal_t    j;
  jrnl_iter_t  it;
  jrnl_entry_t e;
  sim_flash_t *f;                                       // 256 slots
  int          n, total;
  uint32_t     prev;
  int          first;

  printf("wraparound\n");
  for ( total = 1000; total < 1000 + 256; total += 13 ) // Leave the head in every sector, at different offsets
  {
    f = sim_new(4);
    reboot(&j, f);
    for ( n = 0; n != total; n++ )
    {
      e = make_shot(n);
      CHECK(journal_append_shot(&j, &e) == JRNL_OK);
    }

    reboot(&j, f);
    CHECK(j.session_shots == total);                    // Numbering survives even though old entries are gone
    CHECK(j.next_seq == (uint32_t)total + 2);

    journal_iter_session(&j, &it);
    prev  = 0;
    first = -1;
    for ( n = 0; journal_next(&j, &it, &e); n++ )
    {
      if ( first < 0 )
      {
        first = e.shot;
      }
      CHECK(e.seq > prev);                              // Strictly in order
      CHECK(e.x_mm == (float)e.shot);
      prev = e.seq;
    }
    CHECK(n >= 192);                                    // At least 3 of 4 sectors kept
    CHECK(n <= 256);
    CHECK(e.shot == total - 1);                         // Newest shot is last
    CHECK(first == total - n);                          // No gaps
    sim_free(f);
  }
}

static void test_damaged_slot(void)
{
  journal_t    j;
  jrnl_entry_t e;
  sim_flash_t *f = sim_new(4);
  uint32_t     head;

  printf("damaged slot is skipped\n");
  reboot(&j, f);
  e = make_shot(0);
  journal_append_shot(&j, &e);
  head = j.head;
  f->mem[head * JRNL_ENTRY_SIZE + 5] = 0x00;            // Half-written garbage in the next slot
  reboot(&j, f);
  CHECK(j.bad_entries == 1);
  CHECK(j.head == head);
  e = make_shot(1);
  CHECK(journal_append_shot(&j, &e) == JRNL_OK);
  CHECK(j.head == head + 2);                            // Written one slot further on
  reboot(&j, f);
  CHECK(j.session_shots == 2);
  sim_free(f);
}

static void test_append_during_iteration(void)
{
  journal_t    j;
  jrnl_iter_t  it;
  jrnl_entry_t e;
  sim_flash_t *f = sim_new(4);
  int          n, seen = 0;
  uint32_t     prev = 0;

  printf("append while replaying\n");
  reboot(&j, f);
  for ( n = 0; n != 300; n++ )
  {
    e = make_shot(n);
    journal_append_shot(&j, &e);
  }

  journal_iter_session(&j, &it);
  while ( journal_next(&j, &it, &e) )
  {
    CHECK(e.seq > prev);
    prev = e.seq;
    if ( (seen++ % 3) == 0 )                            // Keep shooting while a client replays
    {
      jrnl_entry_t s = make_shot(1000 + seen);
      journal_append_shot(&j, &s);
    }
  }
  CHECK(seen > 0);
  CHECK(seen <= 256);
  sim_free(f);
}

/*
 * Cut the power after every possible number of bytes while appending
 * across several sector boundaries, then check that nothing that was
 * acknowledged was lost and that the journal keeps working.
 */
static void test_power_cuts(void)
{
  const int    total = 140;                             // Crosses two sector boundaries in a 4 sector journal
  sim_flash_t *f;
  journal_t    j;
  jrnl_iter_t  it;
  jrnl_entry_t e;
  long         cut, step;
  int          n, acked, count;
  long         total_bytes = 0;

  printf("power cuts\n");

  f = sim_new(4);                                       // Measure the bytes a clean run changes
  reboot(&j, f);
  f->budget = 1L << 30;
  for ( n = 0; n != total; n++ )
  {
    e = make_shot(n);
    journal_append_shot(&j, &e);
  }
  total_bytes = (1L << 30) - f->budget;
  sim_free(f);

  step = 7;                                             // Prime step: hits every offset within an entry over the run
  for ( cut = 0; cut < total_bytes; cut += step )
  {
    f = sim_new(4);
    reboot(&j, f);
    f->budget = cut;

    acked = 0;
    for ( n = 0; n != total; n++ )
    {
      e = make_shot(n);
      if ( journal_append_shot(&j, &e) != JRNL_OK )
      {
        break;
      }
      acked++;
    }

    reboot(&j, f);                                      // Power returns

    count = 0;
    journal_iter_session(&j, &it);
    while ( journal_next(&j, &it, &e) )
    {
      CHECK(e.x_mm == (float)e.shot);                   // Contents intact
      count++;
    }
    if ( acked > 0 )
    {
      CHECK(j.session_shots >= acked);                  // Every acknowledged shot is still counted
      CHECK(count >= ((acked < 192) ? acked : 192));    // and still readable (up to the ring capacity)
    }

    e = make_shot(j.session_shots);                     // And the journal carries on
    CHECK(journal_append_shot(&j, &e) == JRNL_OK);
    CHECK(e.seq == j.next_seq - 1);
    reboot(&j, f);
    CHECK(j.session_shots == e.shot + 1);

    sim_free(f);
  }
  printf("  %ld cut points over %ld bytes\n", (total_bytes + step - 1) / step, total_bytes);
}

/*
 * The first sector of a brand new partition may contain anything
 */
static void test_garbage_flash(void)
{
  journal_t    j;
  jrnl_entry_t e;
  sim_flash_t *f = sim_new(4);

  printf("garbage in a new partition\n");
  for ( uint32_t i = 0; i != f->size; i++ )
  {
    f->mem[i] = (uint8_t)(i * 37u + 11u);
  }
  reboot(&j, f);
  CHECK(j.entries == 0);
  CHECK(j.bad_entries == f->size / JRNL_ENTRY_SIZE);
  for ( int n = 0; n != 300; n++ )
  {
    e = make_shot(n);
    CHECK(journal_append_shot(&j, &e) == JRNL_OK);
  }
  reboot(&j, f);
  CHECK(j.session_shots == 300);
  sim_free(f);
}

/*
 * Simulated wall clock
 */
typedef struct
{
  uint32_t uptime;    // Seconds since power up
  uint32_t offset;    // Unix time - uptime once the clock is set
  uint8_t  source;    // JRNL_SOURCE_xxx
} sim_clock_t;

static uint32_t sim_now(void *ctx, uint8_t *source)
{
  sim_clock_t *c = (sim_clock_t *)ctx;

  *source = c->source;
  return (c->source == JRNL_SOURCE_NONE) ? c->uptime : c->uptime + c->offset;
}

static void power_up(journal_t *j, sim_flash_t *f, sim_clock_t *c) // Reboot with the clock not set
{
  reboot(j, f);
  c->uptime = 0;
  c->offset = 0;
  c->source = JRNL_SOURCE_NONE;
  journal_set_time_source(j, sim_now, c);
}

static void set_clock(journal_t *j, sim_clock_t *c, uint32_t unix_time, uint8_t source)
{
  c->offset = unix_time - c->uptime;
  c->source = source;
  CHECK(journal_clock_set(j, unix_time, c->uptime, source) == JRNL_OK);
}

static uint32_t shot_time_of(journal_t *j, uint32_t seq, uint8_t *source) // Look an entry up by seq
{
  jrnl_iter_t  it;
  jrnl_entry_t e;

  journal_iter_after(j, &it, seq - 1);
  if ( !journal_next(j, &it, &e) || (e.seq != seq) )
  {
    CHECK(!"entry not found");
    return 0;
  }
  return journal_entry_time(j, &e, source);
}

static void test_no_clock(void)
{
  sim_flash_t *f = sim_new(4);
  journal_t    j;
  jrnl_entry_t e = make_shot(1);
  uint8_t      source;

  printf("no clock\n");
  reboot(&j, f);                                        // No time source installed
  CHECK(journal_append_shot(&j, &e) == JRNL_OK);
  CHECK(e.time == JRNL_TIME_UNKNOWN);
  CHECK(journal_entry_time(&j, &e, &source) == JRNL_TIME_UNKNOWN);
  CHECK(source == JRNL_SOURCE_NONE);
  CHECK(journal_entry_time(&j, &e, NULL) == JRNL_TIME_UNKNOWN);

  CHECK(journal_clock_set(&j, 999999999u, 1, JRNL_SOURCE_NTP) == JRNL_ERR_PARAM);  // Not a Unix time
  CHECK(journal_clock_set(&j, JRNL_TIME_UNKNOWN, 1, JRNL_SOURCE_NTP) == JRNL_ERR_PARAM);
  CHECK(journal_clock_set(&j, 1800000000u, JRNL_TIME_MIN, JRNL_SOURCE_NTP) == JRNL_ERR_PARAM); // Not an uptime
  CHECK(journal_clock_set(&j, 1800000000u, 1, JRNL_SOURCE_NONE) == JRNL_ERR_PARAM);
  CHECK(journal_clock_set(&j, 1800000000u, 1, 3) == JRNL_ERR_PARAM);
  CHECK(j.clock_count == 0);
  sim_free(f);
}

static void test_clock(void)
{
  sim_flash_t *f = sim_new(4);
  journal_t    j;
  sim_clock_t  c;
  jrnl_iter_t  it;
  jrnl_entry_t e;
  uint8_t      source;
  uint32_t     a, b, d, n;
  const uint32_t T = 1800000000u;

  printf("clock set after the first shot\n");
  power_up(&j, f, &c);

  c.uptime = 10;                                        // Shot before the clock is set
  e = make_shot(1);
  CHECK(journal_append_shot(&j, &e) == JRNL_OK);
  a = e.seq;
  CHECK(e.time == 10);
  CHECK((e.flags & JRNL_SOURCE_MASK) == 0);
  CHECK(journal_entry_time(&j, &e, &source) == JRNL_TIME_UNKNOWN);
  CHECK(source == JRNL_SOURCE_NONE);

  c.uptime = 20;                                        // NTP sets the clock
  set_clock(&j, &c, T, JRNL_SOURCE_NTP);
  CHECK(shot_time_of(&j, a, &source) == T - 10);        // The early shot now has a time
  CHECK(source == JRNL_SOURCE_NTP);

  c.uptime = 30;                                        // Shot after the clock is set
  e = make_shot(2);
  e.flags = JRNL_FLAG_MISS;
  CHECK(journal_append_shot(&j, &e) == JRNL_OK);
  b = e.seq;
  CHECK(e.time == T + 10);
  CHECK((e.flags & JRNL_FLAG_MISS) != 0);               // The miss flag survives the stamp
  CHECK(journal_entry_time(&j, &e, &source) == T + 10);
  CHECK(source == JRNL_SOURCE_NTP);
  CHECK(j.last_ntp_time == T + 10);

  /*
   * Power cycle.  The clock is lost but the journal still knows the
   * times of the earlier shots.
   */
  power_up(&j, f, &c);
  CHECK(j.clock_count == 1);
  CHECK(j.last_ntp_time == T + 10);
  CHECK(shot_time_of(&j, a, &source) == T - 10);
  CHECK(source == JRNL_SOURCE_NTP);
  CHECK(shot_time_of(&j, b, &source) == T + 10);

  c.uptime = 5;                                         // A shot in the new power up, before any clock
  e = make_shot(3);
  CHECK(journal_append_shot(&j, &e) == JRNL_OK);
  d = e.seq;
  CHECK(shot_time_of(&j, d, &source) == JRNL_TIME_UNKNOWN); // The old clock setting must not be used

  c.uptime = 8;                                         // A client sets the clock
  set_clock(&j, &c, T + 1000, JRNL_SOURCE_CLIENT);
  CHECK(shot_time_of(&j, d, &source) == T + 997);
  CHECK(source == JRNL_SOURCE_CLIENT);
  CHECK(shot_time_of(&j, a, &source) == T - 10);        // Earlier power ups are unchanged
  CHECK(source == JRNL_SOURCE_NTP);
  CHECK(j.last_ntp_time == T + 10);                     // Client times do not count

  c.uptime = 9;                                         // NTP corrects the client time later on
  set_clock(&j, &c, T + 2000, JRNL_SOURCE_NTP);
  CHECK(shot_time_of(&j, d, &source) == T + 1996);      // The newest setting wins
  CHECK(source == JRNL_SOURCE_NTP);

  journal_iter_session(&j, &it);                        // Clock entries are not shots
  for ( n = 0; journal_next(&j, &it, &e); n++ )
  {
    CHECK(e.kind == JRNL_SHOT);
  }
  CHECK(n == 3);

  power_up(&j, f, &c);                                  // Everything survives another power cycle
  CHECK(shot_time_of(&j, a, NULL) == T - 10);
  CHECK(shot_time_of(&j, b, NULL) == T + 10);
  CHECK(shot_time_of(&j, d, &source) == T + 1996);
  CHECK(source == JRNL_SOURCE_NTP);
  CHECK(j.session_shots == 3);                          // Clock entries do not disturb the session
  sim_free(f);
}

static void test_clock_before_session(void)
{
  sim_flash_t *f = sim_new(4);
  journal_t    j;
  sim_clock_t  c;
  jrnl_entry_t e;
  uint32_t     a;

  printf("clock set in another power up\n");
  power_up(&j, f, &c);
  c.uptime = 4;                                         // Shot in a power up that never gets a clock
  e = make_shot(1);
  CHECK(journal_append_shot(&j, &e) == JRNL_OK);
  a = e.seq;

  power_up(&j, f, &c);
  c.uptime = 3;
  set_clock(&j, &c, 1800000000u, JRNL_SOURCE_NTP);
  CHECK(shot_time_of(&j, a, NULL) == JRNL_TIME_UNKNOWN); // A later power up's clock must not be used

  c.source = JRNL_SOURCE_CLIENT;                        // A time source that claims a source for an uptime
  c.offset = 0;
  e = make_shot(2);
  CHECK(journal_append_shot(&j, &e) == JRNL_OK);
  CHECK(e.time == 3);
  CHECK((e.flags & JRNL_SOURCE_MASK) == 0);             // is not trusted
  sim_free(f);

  printf("clock set before the first shot\n");
  f = sim_new(4);
  power_up(&j, f, &c);
  c.uptime = 3;
  set_clock(&j, &c, 1800000000u, JRNL_SOURCE_NTP);      // Clock set before the first shot
  power_up(&j, f, &c);
  CHECK(j.session == 0);                                // No session to resume
  CHECK(j.next_seq == 2);

  e = make_shot(1);
  CHECK(journal_append_shot(&j, &e) == JRNL_OK);
  CHECK(j.session == 1);
  sim_free(f);
}

static void test_many_power_ups(void)
{
  sim_flash_t *f = sim_new(8);
  journal_t    j;
  sim_clock_t  c;
  jrnl_entry_t e;
  uint32_t     seq[JRNL_CLOCKS + 5];
  uint32_t     boot;
  const uint32_t boots = JRNL_CLOCKS + 5;

  printf("many power ups\n");

  for ( boot = 0; boot != boots; boot++ )
  {
    power_up(&j, f, &c);
    c.uptime = 100;
    e = make_shot((int)boot);
    CHECK(journal_append_shot(&j, &e) == JRNL_OK);
    seq[boot] = e.seq;
    c.uptime = 160;
    set_clock(&j, &c, 1800000000u + boot * 86400u, (boot & 1) ? JRNL_SOURCE_CLIENT : JRNL_SOURCE_NTP);
    CHECK(j.clock_count == ((boot < JRNL_CLOCKS) ? boot + 1 : JRNL_CLOCKS));
  }

  power_up(&j, f, &c);
  CHECK(j.clock_count == JRNL_CLOCKS);
  for ( boot = 0; boot != boots; boot++ )
  {
    uint8_t  source;
    uint32_t t = shot_time_of(&j, seq[boot], &source);

    if ( boot < boots - JRNL_CLOCKS )                   // Too old, the setting was forgotten
    {
      CHECK(t == JRNL_TIME_UNKNOWN);
    }
    else
    {
      CHECK(t == 1800000000u + boot * 86400u - 60u);
      CHECK(source == ((boot & 1) ? JRNL_SOURCE_CLIENT : JRNL_SOURCE_NTP));
    }
  }
  sim_free(f);
}

static void test_follow(void)
{
  sim_flash_t *f = sim_new(4);
  journal_t    j;
  jrnl_iter_t  it;
  jrnl_entry_t e;
  uint32_t     last = 0;
  uint32_t     got  = 0;
  uint32_t     total;
  int          n;

  printf("follow new entries\n");
  reboot(&j, f);
  journal_iter_after(&j, &it, 0);
  CHECK(!journal_next(&j, &it, &e));                    // Empty
  journal_iter_follow(&j, &it);
  CHECK(!journal_next(&j, &it, &e));                    // Still empty

  for ( total = 0; total != 700; total++ )              // Several laps of the 256 entry ring, read as we go
  {
    e = make_shot((int)total);
    CHECK(journal_append_shot(&j, &e) == JRNL_OK);
    if ( (total % 7) == 0 )                             // The reader falls behind now and then
    {
      continue;
    }
    journal_iter_follow(&j, &it);
    for ( n = 0; journal_next(&j, &it, &e); n++ )
    {
      CHECK(e.seq == last + 1);                         // In order, nothing skipped or repeated
      last = e.seq;
      got++;
    }
    CHECK(n >= 1);
  }
  journal_iter_follow(&j, &it);
  while ( journal_next(&j, &it, &e) )
  {
    CHECK(e.seq == last + 1);
    last = e.seq;
    got++;
  }
  CHECK(last == j.next_seq - 1);                        // Caught up with the writer
  CHECK(got == 701);                                    // 700 shots + the session entry

  journal_iter_session(&j, &it);                        // A session iterator follows too
  while ( journal_next(&j, &it, &e) )
  {
  }
  e = make_shot(1);
  CHECK(journal_append_shot(&j, &e) == JRNL_OK);
  CHECK(!journal_next(&j, &it, &e));                    // Finished until told to follow
  journal_iter_follow(&j, &it);
  CHECK(journal_next(&j, &it, &e) && (e.seq == j.next_seq - 1));
  CHECK(!journal_next(&j, &it, &e));
  sim_free(f);
}

static void test_follow_overrun(void)
{
  sim_flash_t *f = sim_new(4);
  journal_t    j;
  jrnl_iter_t  it;
  jrnl_entry_t e;
  uint32_t     last = 0;
  uint32_t     first = 0;
  int          n;

  printf("follow after the reader is lapped\n");
  reboot(&j, f);
  e = make_shot(0);
  CHECK(journal_append_shot(&j, &e) == JRNL_OK);
  journal_iter_after(&j, &it, 0);
  while ( journal_next(&j, &it, &e) )
  {
    last = e.seq;
  }
  for ( n = 0; n != 300; n++ )                          // More than the ring holds
  {
    e = make_shot(n);
    CHECK(journal_append_shot(&j, &e) == JRNL_OK);
  }
  journal_iter_follow(&j, &it);
  for ( n = 0; journal_next(&j, &it, &e); n++ )
  {
    if ( n == 0 )
    {
      first = e.seq;
    }
    CHECK(e.seq > last);                                // Never goes backwards
    last = e.seq;
  }
  CHECK(first > 2);                                     // The oldest were overwritten
  CHECK(last == j.next_seq - 1);
  sim_free(f);
}

static void test_target(void)
{
  sim_flash_t *f = sim_new(4);
  journal_t    j;
  jrnl_iter_t  it;
  jrnl_entry_t e;
  int          n;

  printf("target type of a session\n");
  reboot(&j, f);
  CHECK(j.target == 0);
  journal_set_target(&j, 110);
  CHECK(journal_new_session(&j, 2) == JRNL_OK);
  e = make_shot(1);
  CHECK(journal_append_shot(&j, &e) == JRNL_OK);
  journal_set_target(&j, 111);
  CHECK(journal_set_mode(&j, 4) == JRNL_OK);
  e = make_shot(2);
  CHECK(journal_append_shot(&j, &e) == JRNL_OK);

  journal_iter_after(&j, &it, 0);
  for ( n = 0; journal_next(&j, &it, &e); )
  {
    if ( e.kind == JRNL_SESSION )
    {
      CHECK(e.info.target == 110);
      n++;
    }
    if ( e.kind == JRNL_MODE )
    {
      CHECK(e.info.target == 111);
      n++;
    }
  }
  CHECK(n == 2);

  reboot(&j, f);
  CHECK(j.target == 111);                               // From the newest session or mode entry

  journal_set_target(&j, 500);                          // The next session has its own
  CHECK(journal_new_session(&j, 0) == JRNL_OK);
  reboot(&j, f);
  CHECK(j.target == 500);
  e = make_shot(3);                                     // Shots do not change it
  CHECK(journal_append_shot(&j, &e) == JRNL_OK);
  reboot(&j, f);
  CHECK(j.target == 500);
  sim_free(f);
}

static void test_end_session(void)
{
  sim_flash_t *f = sim_new(4);
  journal_t    j;
  sim_clock_t  c;
  jrnl_entry_t e;
  jrnl_iter_t  it;
  int          n;

  printf("ending a session\n");
  power_up(&j, f, &c);
  CHECK(!j.session_open);
  CHECK(journal_end_session(&j, JRNL_END_COMMAND) == JRNL_OK);   // Nothing to end
  CHECK(j.next_seq == 1);                                         // and nothing written

  journal_set_target(&j, 110);
  CHECK(journal_new_session(&j, 2) == JRNL_OK);
  CHECK(j.session_open);
  e = make_shot(1);
  CHECK(journal_append_shot(&j, &e) == JRNL_OK);
  CHECK(journal_end_session(&j, JRNL_END_IDLE) == JRNL_OK);
  CHECK(!j.session_open);
  CHECK(j.newest.kind == JRNL_END);
  CHECK(j.newest.info.reason == JRNL_END_IDLE);
  CHECK(j.newest.info.target == 110);
  CHECK(j.newest.session == 1);
  CHECK(journal_end_session(&j, JRNL_END_COMMAND) == JRNL_OK);   // Already ended: no second entry
  CHECK(j.newest.info.reason == JRNL_END_IDLE);

  c.uptime = 50;                                                  // A clock entry after the end must not reopen it
  set_clock(&j, &c, 1800000000u, JRNL_SOURCE_NTP);
  power_up(&j, f, &c);
  CHECK(j.session == 1);
  CHECK(!j.session_open);

  e = make_shot(2);                                               // The next shot starts session 2
  CHECK(journal_append_shot(&j, &e) == JRNL_OK);
  CHECK(e.session == 2);
  CHECK(e.shot == 0);
  CHECK(j.session_open);
  power_up(&j, f, &c);
  CHECK(j.session == 2);
  CHECK(j.session_open);
  CHECK(j.session_shots == 1);

  CHECK(journal_end_session(&j, JRNL_END_COMMAND) == JRNL_OK);
  CHECK(journal_set_mode(&j, 4) == JRNL_OK);                      // A mode change after the end also starts one
  CHECK(j.session == 3);
  CHECK(j.session_open);

  journal_iter_after(&j, &it, 0);                                 // Two end entries in all
  for ( n = 0; journal_next(&j, &it, &e); )
  {
    n += (e.kind == JRNL_END);
  }
  CHECK(n == 2);
  sim_free(f);
}

int main(void)
{
  test_crc();
  test_geometry();
  test_empty();
  test_append_and_recover();
  test_sessions();
  test_mode();
  test_wraparound();
  test_damaged_slot();
  test_append_during_iteration();
  test_power_cuts();
  test_garbage_flash();
  test_no_clock();
  test_clock();
  test_clock_before_session();
  test_many_power_ups();
  test_follow();
  test_follow_overrun();
  test_target();
  test_end_session();

  printf("\n%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
