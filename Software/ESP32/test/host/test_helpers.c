/*
 * contains() and http_target_type() from main/helpers.c (extracted by
 * test_firmware.py).  The target type decides which face the clients
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

  assert(type_of("Pistol Match", "freETarget.targets.AirPistol") == 100);
  assert(type_of("Pistol Practice", "freETarget.targets.AirPistol") == 101);
  assert(type_of("Rifle Match", "freETarget.targets.AirRifle") == 110);
  assert(type_of("Rifle Practice", "freETarget.targets.AirRifle") == 111);
  assert(type_of("50m Pistol Match", "freETarget.targets.Pistol50m") == 500);
  assert(type_of("50m Rifle Practice", "freETarget.targets.Rifle50m") == 511);

  puts("Helpers: contains() and the target type from event and target names passed");
  return 0;
}
