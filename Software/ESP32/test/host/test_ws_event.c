/* Session-change frames must identify the event without relabeling history. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "serial_io.h"
#define FREETARGET_C
#include "freETarget_host.h"
#include "journal.h"
#include "shot_journal_host.h"
#define WS_SHOT_FIELDS "SNQM?TUXPOE"

static char json_event[128] = "Pistol Match";
static char sent[SCORE_TEXT];
unsigned int shot_journal_session(void) { return 7; }
static bool send_text(int fd, const char *text, size_t length)
{ (void)fd; assert(length < sizeof(sent)); memcpy(sent, text, length + 1); return true; }
static void build_json_score(shot_record_t *shot, const char *format, char *out, size_t size)
{ (void)shot; (void)format; (void)out; (void)size; assert(false); }

#include "ws_event_host.inc"

int main(void)
{
  shot_journal_event_t ev = {.kind = JRNL_SESSION, .session = 7, .seq = 20, .target = 100};
  assert(send_event(1, &ev, 0));
  assert(strstr(sent, "\"event\":\"Pistol Match\""));
  ev.kind = JRNL_MODE;
  strcpy(json_event, "Pistol Final");
  assert(send_event(1, &ev, 0));
  assert(strstr(sent, "\"event\":\"Pistol Final\""));
  assert(send_event(1, &ev, 7));
  assert(strstr(sent, "\"history\":7") && !strstr(sent, "\"event\""));
  ev.session = 6;
  assert(send_event(1, &ev, 0));
  assert(!strstr(sent, "\"event\""));
  puts("WebSocket: current session event metadata and history isolation passed");
  return 0;
}
