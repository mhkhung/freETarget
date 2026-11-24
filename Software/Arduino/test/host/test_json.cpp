/*
 * Runs the Arduino read_JSON() against typed input on a computer.
 *
 *   - Every command used to hang read_JSON() once it reached the end of
 *     the command table (the loop also waited for a GPIO table index that
 *     was never advanced).
 *   - A text value longer than its setting ran over the RAM and NON-VOL
 *     that follow it, and a missing closing quote ran off the input.
 */
#include <signal.h>
#include <unistd.h>
#include "arduino_stub.h"

std::string serial_out;
SerialStub  Serial, Serial2, Serial3;
uint8_t     eeprom[EEPROM_SIZE + EEPROM_GUARD];
unsigned int eeprom_writes;
EEPROMStub  EEPROM;

/*
 * The headers freETarget.ino includes ahead of json.ino in the sketch
 */
#include "freETarget.h"
#include "gpio.h"
#include "compute_hit.h"
#include "analog_io.h"
#include "nonvol.h"
#include "mechanical.h"
#include "diag_tools.h"
#include "timer.h"

/*
 * What the rest of the sketch provides
 */
unsigned int shot_number;
const GPIO   init_table[] = {{37, INPUT_PULLUP, 0}, {0xff, 0xff, 0xff}};
static unsigned int echoes;
void bye(void) {}
void set_trip_point(int) {}
void show_echo(void) { echoes++; }
static void show_test(int) {}
static void show_names(int) {}
static void set_trace(int) {}
static void diag_delay(int) {}
void init_nonvol(int) {}
void set_LED_PWM_now(int) {}
void backup_nonvol(void) {}
void restore_nonvol(void) {}
void rapid_enable(unsigned int) {}
void setup(void) {}
void gen_position(int) {}
void tabata_enable(unsigned int) {}
void POST_version(void) {}
void compute_vset_PWM(double) {}
void self_test(uint16_t) {}
void drive_paper(void) {}
bool do_dlt(unsigned int) { return false; }

/*
 * Input comes from this string
 */
static const char *input;
char available_all(void) { return *input != 0; }
char get_all(void) { return *input ? *input++ : 0; }

#undef  DIAMETER
#define DIAMETER 230.0                          // mechanical.h uses GCC's 230.0d
#include "json_host.inc"                // json.ino, see extract.py

/*
 * Checks
 */
static int failures;
#define CHECK(cond, ...)                                     \
  do                                                         \
  {                                                          \
    if ( !(cond) )                                           \
    {                                                        \
      failures++;                                            \
      printf("FAIL %s:%d: ", __FILE__, __LINE__);            \
      printf(__VA_ARGS__);                                   \
      printf("\n");                                          \
    }                                                        \
  } while ( 0 )

static void on_alarm(int)
{
  static const char text[] = "FAIL: read_JSON() did not return (hangs on every command)\n";
  write(1, text, sizeof(text) - 1);
  _exit(2);
}

static void command(const char *text)
{
  input = text;
  serial_out.clear();
  alarm(2);
  read_JSON();
  alarm(0);
}

int main(void)
{
  signal(SIGALRM, on_alarm);
  memset(eeprom, 0xAB, sizeof(eeprom));

  command("{\"ECHO\":0}");
  CHECK(echoes == 1, "ECHO ran %u times", echoes);

  command("{\"DIP\":9, \"ANGLE\":30}");
  CHECK(json_dip_switch == 9 && json_sensor_angle == 30, "DIP %d ANGLE %d", json_dip_switch, json_sensor_angle);

  command("{\"WIFI_SSID\":\"Range Wifi\"}");
  CHECK(strcmp(json_wifi_ssid, "Range Wifi") == 0, "SSID '%s'", json_wifi_ssid);
  CHECK(strcmp((char *)&eeprom[NONVOL_WIFI_SSID_32], "Range Wifi") == 0, "NON-VOL SSID '%s'", (char *)&eeprom[NONVOL_WIFI_SSID_32]);

  /*
   * Longer than the setting: kept to its size, nothing after it touched
   */
  int ring_time = 0x1234;
  EEPROM.put(NONVOL_MIN_RING_TIME, ring_time);          // Follows the SSID in NON-VOL
  EEPROM.put(NONVOL_WIFI_IP, ring_time);                // Follows the password
  command("{\"WIFI_SSID\":\"0123456789012345678901234567890123456789\"}");
  CHECK(strlen(json_wifi_ssid) == sizeof(json_wifi_ssid) - 1, "SSID kept %zu characters", strlen(json_wifi_ssid));
  CHECK(EEPROM.get(NONVOL_MIN_RING_TIME, ring_time) == 0x1234, "SSID ran over MIN_RING_TIME in NON-VOL (%x)", ring_time);
  CHECK(serial_out.find("too long") != std::string::npos, "no warning: %s", serial_out.c_str());

  command("{\"WIFI_PWD\":\"a-very-long-passphrase-indeed\"}");
  CHECK(strlen(json_wifi_pwd) == sizeof(json_wifi_pwd) - 1, "PWD kept %zu characters", strlen(json_wifi_pwd));
  CHECK(EEPROM.get(NONVOL_WIFI_IP, ring_time) == 0x1234, "PWD ran over WIFI_IP in NON-VOL (%x)", ring_time);

  /*
   * No closing quote, or no text at all: stop at the end of the input
   */
  command("{\"WIFI_SSID\":\"Unclosed}");
  CHECK(strcmp(json_wifi_ssid, "Unclosed") == 0, "SSID '%s'", json_wifi_ssid);
  command("{\"WIFI_SSID\":}");
  CHECK(strcmp(json_wifi_ssid, "Unclosed") == 0, "SSID changed to '%s'", json_wifi_ssid);

  /*
   * Rewriting the same value does not wear the EEPROM
   */
  command("{\"WIFI_SSID\":\"Range Wifi\"}");
  unsigned int writes = eeprom_writes;
  command("{\"WIFI_SSID\":\"Range Wifi\"}");
  CHECK(eeprom_writes == writes, "%u EEPROM writes for the same SSID", eeprom_writes - writes);

  /*
   * A '?' shows the status and reports that input was read
   */
  input = "?";
  bool got = read_JSON();
  CHECK(got, "read_JSON() returned false after '?'");

  printf(failures ? "%d FAILED\n" : "PASS: Arduino read_JSON (no hang, bounded text, end of input)\n", failures);
  return failures != 0;
}
