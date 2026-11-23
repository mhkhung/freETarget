/*
 * Text values in JSON commands (handle_json() and good_input() from
 * main/json.c, extracted by test_firmware.py).  A value longer than its
 * setting is cut to fit instead of overwriting the memory after it, and
 * the athlete's name is never written to flash.
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "serial_io.h"
#define FREETARGET_C
#include "freETarget_host.h"
#undef EXTERN
#include "json_host.h"

#define DLT(level, statement)
#undef SEND
#define SEND(who, message)

typedef int nvs_handle_t;
static nvs_handle_t my_handle;
static int          saved_strings;
static char         saved_key[32];

static void nvs_set_str(nvs_handle_t h, const char *key, const char *value) { (void)h; (void)value; saved_strings++; snprintf(saved_key, sizeof(saved_key), "%s", key); }
static void nvs_set_i32(nvs_handle_t h, const char *key, int value) { (void)h; (void)key; (void)value; }
static void nvs_commit(nvs_handle_t h) { (void)h; }
static int  to_int(char c) { return c - '0'; }
static int  instr(char *s1, char *s2)
{
  int i = 0;
  while ( (*s1 != 0) && (*s2 != 0) ) { if ( *s1 != *s2 ) return -1; s1++; s2++; i++; }
  return (*s2 == 0) ? i : -1;
}
void        show_echo(void) {}
static void show_names(int v) { (void)v; }
static void set_trace(int v) { (void)v; }
static void set_50m(int x) { (void)x; }
static void unlock_target(unsigned int password) { (void)password; }
static void lock_target(unsigned int password) { (void)password; }

/*
 * The settings under test, laid out with guard bytes after each so an
 * overrun shows up.
 */
static struct
{
  char athlete[LARGE_STRING];
  char guard1[8];
  char pwd[PWD_SIZE + 1];
  char guard2[8];
  char name[SMALL_STRING];
  char guard3[8];
} mem;

const json_message_t JSON[] = {
    {SHOW, "\"ATHLETE\":",   (int *)mem.athlete, IS_TEXT + LARGE_STRING,   0, 0,           0, 0},
    {SHOW, "\"NAME_TEXT\":", (int *)mem.name,    IS_TEXT + SSID_SIZE,      0, "NAME_TEXT", 0, 0},
    {SHOW, "\"WIFI_PWD\":",  (int *)mem.pwd,     IS_SECRET + PWD_SIZE + 1, 0, "WIFI_PWD",  0, 0},
    {0,    0,                0,                  0,                        0, 0,           0, 0},
};

char         input_JSON[1024];
unsigned int got_right_bracket;
static unsigned int in_JSON;
static bool         got_left_bracket;
static bool         not_found;
int                 json_lock;                         // Declared extern by json.h

#ifdef __clang__
#pragma clang diagnostic ignored "-Wparentheses-equality" // json.c writes if ( (a == b) )
#endif
#include "json_parse_host.inc"

static void command(const char *text)
{
  snprintf(input_JSON, sizeof(input_JSON), "%s", text);
  got_right_bracket = (unsigned int)strlen(input_JSON);
  handle_json();
}

static bool guards_intact(void)
{
  static const char zero[8];
  return !memcmp(mem.guard1, zero, 8) && !memcmp(mem.guard2, zero, 8) && !memcmp(mem.guard3, zero, 8);
}

int main(void)
{
  char long_text[600];

  command("\"ATHLETE\":\"Jane Doe\"");
  assert(strcmp(mem.athlete, "Jane Doe") == 0);
  assert(saved_strings == 0);                          // The athlete's name stays in RAM

  memset(long_text, 'x', 500);
  long_text[500] = 0;
  char cmd[700];
  snprintf(cmd, sizeof(cmd), "\"ATHLETE\":\"%s\"", long_text);
  command(cmd);                                        // Longer than the setting and the parser's buffer
  assert(strlen(mem.athlete) == LARGE_STRING - 1);
  assert(guards_intact());

  snprintf(cmd, sizeof(cmd), "\"NAME_TEXT\":\"%.40s\"", long_text);
  command(cmd);
  assert(strlen(mem.name) == SSID_SIZE - 1);
  assert(guards_intact());
  assert(strcmp(saved_key, "NAME_TEXT") == 0);

  snprintf(cmd, sizeof(cmd), "\"WIFI_PWD\":\"%.63s\"", long_text);
  command(cmd);                                        // A 63 character WPA password fits
  assert(strlen(mem.pwd) == 63);
  assert(guards_intact());

  command("\"ATHLETE\":\"unterminated");               // No closing quote: stop at the end of the input
  assert(strcmp(mem.athlete, "unterminated") == 0);
  command("\"ATHLETE\":");                             // No opening quote either
  assert(guards_intact());

  puts("JSON text: values cut to fit their settings, athlete name not saved");
  return 0;
}
