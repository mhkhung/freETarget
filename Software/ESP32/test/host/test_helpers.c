/*
 * contains(), http_target_type() and json_read_only() from main/helpers.c
 * (extracted by test_firmware.py).  The target type decides which face the clients
 * draw and how they score, from the names the PC client and the web
 * page send: {"EVENT":"Rifle Practice", "TARGET_NAME":"freETarget.targets.AirRifle"}.
 */
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

static char json_event[64];
static char json_target_name[64];

#include "helpers_host.inc"

static int type_of(const char *event, const char *target_name)
{
  strcpy(json_event, event);
  strcpy(json_target_name, target_name);
  return http_target_type();
}

int main(void)
{
  assert(contains("freETarget.targets.AirRifle", "Rifle"));   // "rR": a partial match must not hide the real one
  assert(contains("Pistol Practice", "Practice"));
  assert(contains("RFP Allan", "rfp"));                        // Not case sensitive
  assert(contains("aab", "ab"));
  assert(contains("anything", ""));
  assert(!contains("Pistol Match", "Practice"));
  assert(!contains("Rif", "Rifle"));                           // Running off the end is not a match
  assert(!contains("", "x"));

  // GET /json: only the commands that just report, in any arrangement the parser reads
  assert(json_read_only("{\"ECHO\":0}"));
  assert(json_read_only("{\"VERSION\":0}"));
  assert(json_read_only("{\"MFS?\":0}"));
  assert(json_read_only("{ \"ECHO\" : 0 , \"VERSION\":1}"));
  assert(!json_read_only("{}"));                                // Nothing to report
  assert(!json_read_only(""));
  assert(!json_read_only("{\"INIT\":0}"));
  assert(!json_read_only("{\"X_OFFSET\":1.5}"));
  assert(!json_read_only("{\"ECHO\":0,\"INIT\":0}"));          // One changing command spoils it
  assert(!json_read_only("{\"ECHO\":0}{\"RESET\":0}"));        // Two commands in one
  assert(!json_read_only("{^INIT^:0}"));                       // The parser reads ^ as "
  assert(!json_read_only("{\"ECHO\":\"\"INIT\"\"}"));          // Names hidden in a value still run
  assert(!json_read_only("{\"ECHOX\":0}"));
  assert(!json_read_only("{\"echo\":0}"));                     // The parser is case sensitive
  assert(!json_read_only("{\"ECHO\":0}*"));                    // '*' and other characters are refused

  assert(type_of("Pistol Match", "freETarget.targets.AirPistol") == 100);
  assert(type_of("Pistol Practice", "freETarget.targets.AirPistol") == 101);
  assert(type_of("Rifle Match", "freETarget.targets.AirRifle") == 110);
  assert(type_of("Rifle Practice", "freETarget.targets.AirRifle") == 111);
  assert(type_of("50m Pistol Match", "freETarget.targets.Pistol50m") == 500);
  assert(type_of("50m Rifle Practice", "freETarget.targets.Rifle50m") == 511);

  puts("Helpers: contains(), the target type from event and target names, and the read-only GET /json commands passed");
  return 0;
}
