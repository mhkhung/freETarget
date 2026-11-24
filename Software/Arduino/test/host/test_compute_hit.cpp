/*
 * Runs the Arduino shot position solver (compute_hit.ino) with exact
 * sensor timings for shots all over the card, with the sensors in the
 * paper plane and 13 mm in front of it (the Z_OFFSET default).
 *
 * The Mega's double is a 32-bit float, so this test computes with float too.
 *
 * Shots more than 82 mm from the centre cross the line between two sensors.
 * The solver used to mirror them back over that line, several millimetres
 * out.  With the sensors off the paper, the estimate of the distance to the
 * closest sensor also has to be a slant range, like the measured times.
 */
#include <math.h>
#include "arduino_stub.h"

std::string  serial_out;
SerialStub   Serial, Serial2, Serial3;
uint8_t      eeprom[EEPROM_SIZE + EEPROM_GUARD];
unsigned int eeprom_writes;
EEPROMStub   EEPROM;

#define double float                            // As on the AVR
#include "freETarget.h"
#include "compute_hit.h"
#include "json.h"

double       json_sensor_dia = 232, json_doppler = 0, s_of_sound;
int          json_north_x, json_north_y, json_east_x, json_east_y, json_south_x, json_south_y, json_west_x, json_west_y;
int          json_z_offset, json_calibre_x10 = 45, json_rh = 50;
unsigned int location;
const char*  which_one[4] = {"N:", "E:", "S:", "W:"};
int          json_clock[4];
static const double speed = 0.3434;             // mm/us at 20 C

double speed_of_sound(double temperature, int humidity) { (void)temperature; (void)humidity; return speed; }
double temperature_C(void) { return 20; }
bool   do_dlt(unsigned int level) { (void)level; return false; }

#undef  DIAMETER
#include "compute_hit_host.inc"
#undef double

/*
 * Each counter starts when its sensor hears the shot; all stop together.
 * The Arduino counts at 8 MHz.
 */
static double error_mm(double x, double y, double depth)
{
  shot_record_t shot;
  double        r     = json_sensor_dia / 2;
  double        sx[4] = {0, r, 0, -r}, sy[4] = {r, 0, -r, 0};

  memset(&shot, 0, sizeof(shot));
  for ( int i = N; i <= W; i++ )
  {
    double d = sqrt((x - sx[i]) * (x - sx[i]) + (y - sy[i]) * (y - sy[i]) + depth * depth);
    shot.timer_count[i] = 20000 - (int)floor(d / speed * OSCILLATOR_MHZ + 0.5);
  }
  if ( compute_hit(&shot) == MISS )
  {
    return INFINITY;
  }
  return hypot(shot.xphys_mm - x, shot.yphys_mm - y);
}

int main(void)
{
  static const int depths[] = {0, 13};
  int              failures = 0;

  for ( unsigned d = 0; d < sizeof(depths) / sizeof(depths[0]); d++ )
  {
    json_z_offset = depths[d];
    for ( int radius = 0; radius <= 110; radius += 5 )
    {
      double worst = 0, at = 0;
      for ( int angle = 0; angle < 360; angle += 5 )
      {
        double e = error_mm(radius * cos(angle * M_PI / 180), radius * sin(angle * M_PI / 180), depths[d]);
        if ( e > worst )
        {
          worst = e;
          at    = angle;
        }
        if ( radius == 0 )
        {
          break;
        }
      }
      if ( worst > 0.25 )
      {
        printf("FAIL: sensors %d mm off the paper: %.2f mm error %d mm from the centre at %.0f degrees\n", depths[d], worst,
               radius, at);
        failures++;
      }
    }
  }

  if ( failures == 0 )
  {
    printf("PASS: Arduino shot positions within 0.25 mm out to 110 mm, sensors in the paper plane or 13 mm off it\n");
  }
  return failures != 0;
}
