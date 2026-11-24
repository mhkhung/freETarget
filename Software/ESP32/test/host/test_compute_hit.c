/*
 * Runs the real shot position solver (compute_hit.c) with exact sensor
 * timings for shots all over the card, with the sensors in the paper plane
 * and 13 mm in front of it (the Z_OFFSET default).
 *
 * Shots more than 82 mm from the centre cross the line between two sensors.
 * The solver used to mirror them back over that line, putting shots near
 * the edge of the card several millimetres (up to 13 mm) out.  With the
 * sensors off the paper, the slant range also has to be taken off.
 */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "serial_io.h"
#define FREETARGET_C
#include "freETarget_host.h"

#define DLT(level, statement)
#ifndef PI
#define PI 3.14159265358979323846
#endif
#define SQ(x) ((x) * (x))
#define LED_NORTH_FAILED "n"
#define LED_EAST_FAILED  "e"
#define LED_SOUTH_FAILED "s"
#define LED_WEST_FAILED  "w"
#define RUN_NORTH_LO 0
#define RUN_NORTH_HI 0
#define RUN_EAST_LO  0
#define RUN_EAST_HI  0
#define RUN_SOUTH_LO 0
#define RUN_SOUTH_HI 0
#define RUN_WEST_LO  0
#define RUN_WEST_HI  0
#define BIT_NORTH_LO 0
#define BIT_NORTH_HI 0
#define BIT_EAST_LO  0
#define BIT_EAST_HI  0
#define BIT_SOUTH_LO 0
#define BIT_SOUTH_HI 0
#define BIT_WEST_LO  0
#define BIT_WEST_HI  0

real_t json_sensor_dia = 232;
int    json_z_offset, json_calibre_x10 = 45, json_session_type;
int    json_north_x, json_north_y, json_east_x, json_east_y, json_south_x, json_south_y, json_west_x, json_west_y;

static const double speed = 0.3434; // mm/us at 20 C

double speed_of_sound(double temperature, double humidity)
{
  (void)temperature;
  (void)humidity;
  return speed;
}
double temperature_C(void) { return 20; }
double humidity_RH(void) { return 50; }
void   ft_timer_new(time_count_t *timer, unsigned int value, void *callback, char *name)
{
  (void)timer;
  (void)value;
  (void)callback;
  (void)name;
}
bool find_xy_3D(sensor_t *s, real_t estimate, real_t z_offset_clock);

#pragma GCC diagnostic ignored "-Wmissing-field-initializers" // The sensor table in compute_hit.c
#include "compute_hit_host.inc"

/*
 * Each counter starts when its sensor hears the shot; all stop together
 */
static double error_mm(double x, double y, double depth)
{
  shot_record_t shot;
  double        r     = json_sensor_dia / 2;
  double        sx[4] = {0, r, 0, -r}, sy[4] = {r, 0, -r, 0};
  int           i;

  memset(&shot, 0, sizeof(shot));
  for ( i = N; i <= W; i++ )
  {
    double d = sqrt(SQ(x - sx[i]) + SQ(y - sy[i]) + SQ(depth));
    shot.timer_count[i] = 20000 - (int)floor(d / speed * OSCILLATOR_MHZ + 0.5);
  }
  if ( compute_hit(&shot) == MISS )
  {
    return INFINITY;
  }
  return hypot(shot.x * speed / OSCILLATOR_MHZ - x, shot.y * speed / OSCILLATOR_MHZ - y);
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
        double e = error_mm(radius * cos(angle * PI / 180), radius * sin(angle * PI / 180), depths[d]);
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

  /*
   * Z_OFFSET set but the sensors in the paper plane is the only case where
   * a shot is "closer than the sensor plane"; it must not become NaN
   */
  json_z_offset = 13;
  if ( !isfinite(error_mm(0, 115, 0)) )
  {
    printf("FAIL: a shot next to a sensor is lost when Z_OFFSET is larger than the real offset\n");
    failures++;
  }

  if ( failures == 0 )
  {
    printf("PASS: shot positions within 0.25 mm out to 110 mm, sensors in the paper plane or 13 mm off it\n");
  }
  return failures != 0;
}
