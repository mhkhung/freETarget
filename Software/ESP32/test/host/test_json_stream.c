#include <assert.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "serial_io.h"
#define FREETARGET_C
#include "freETarget_host.h"

#define DLT(level, statement)

static char input_JSON[1024];
static unsigned int in_JSON, got_right_bracket, from_BlueTooth;
static bool got_left_bracket, keep_space;
static int json_aux_mode = RS485;
static const char *input;
static size_t position;
static int commands, flushes, delay_arrivals;
static bool append_after_command;
static char received[3][256];
static jmp_buf finished;

int serial_available(int ports)
{
  return (ports == AUX_PORT) ? 0 : (int)(strlen(input) - position);
}

char serial_getch(int ports)
{
  (void)ports;
  return input[position++];
}

void serial_putch(char ch, int ports)
{
  (void)ch;
  (void)ports;
}

void serial_flush(int ports)
{
  (void)ports;
  flushes++;
  position = strlen(input);
}

static void vTaskDelay(int ticks)
{
  (void)ticks;
  if ( append_after_command && commands == 1 && delay_arrivals == 0 )
  {
    input = "{\"SESSION\":0}{\"SESSION\":2}";
    position = 0;
    delay_arrivals++;
  }
  else if ( position == strlen(input) )
  {
    longjmp(finished, 1);
  }
}

static void POST_version(void) {}
static void Bluetooth_start_new_connection(void) {}

static void handle_json(void)
{
  assert(commands < 3);
  snprintf(received[commands++], sizeof(received[0]), "%s", input_JSON);
  in_JSON = 0;
  got_right_bracket = 0;
  got_left_bracket = false;
  input_JSON[0] = 0;
}

#include "json_stream_host.inc"

static void test_commands(bool arriving_during_delay)
{
  input = arriving_during_delay
              ? "{\"ATHLETE\":\"Michael Hung\",\"EVENT\":\"Rifle Match\",\"TARGET_NAME\":\"freETarget.targets.AirRifle\"}"
              : "{\"ATHLETE\":\"Michael Hung\",\"EVENT\":\"Rifle Match\",\"TARGET_NAME\":\"freETarget.targets.AirRifle\"}"
                "{\"SESSION\":0}{\"SESSION\":2}";
  position = 0;
  commands = flushes = delay_arrivals = 0;
  append_after_command = arriving_during_delay;
  run_state = IN_OPERATION;

  if ( setjmp(finished) == 0 )
  {
    freeETarget_json(NULL);
  }

  assert(commands == 3);
  assert(flushes == 0);
  assert(strcmp(received[0], "\"ATHLETE\":\"Michael Hung\",\"EVENT\":\"Rifle Match\",\"TARGET_NAME\":\"freETarget.targets.AirRifle\"") == 0);
  assert(strcmp(received[1], "\"SESSION\":0") == 0);
  assert(strcmp(received[2], "\"SESSION\":2") == 0);
}

int main(void)
{
  test_commands(false);
  test_commands(true);
  puts("JSON stream: queued commands and commands arriving during a yield passed");
  return 0;
}
