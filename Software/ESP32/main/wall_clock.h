/*----------------------------------------------------------------
 *
 * wall_clock.h
 *
 * Time of day for the shot time stamps
 *
 *----------------------------------------------------------------
 *
 * The board has no battery backed clock, so the time of day is lost
 * at every power off.  It is set again from one of two sources:
 *
 *   NTP     In station mode (WIFI_SSID set) the target asks a time
 *           server: the one announced by the router (DHCP), or
 *           pool.ntp.org.  NTP always wins.
 *
 *   Client  {"TIME": <Unix seconds>} from a client.  Only accepted
 *           if the clock has not been set yet in this power up.
 *
 * Every shot records which source its time came from, so a result
 * whose time came from a client can be told apart.
 *
 *--------------------------------------------------------------*/
#ifndef _WALL_CLOCK_H_
#define _WALL_CLOCK_H_

#include <stdint.h>

uint32_t    wall_clock_uptime(void);              // Seconds since power up
uint32_t    wall_clock_now(uint8_t *source);      // Unix time, or seconds since power up if not set (see journal.h)
int         wall_clock_source(void);              // JRNL_SOURCE_xxx
const char *wall_clock_source_name(int source);   // "ntp", "client" or "none"
void        wall_clock_ntp_init(void);            // Station mode, call before the WiFi is started
void        wall_clock_from_client(int unix_time);// {"TIME":...} command
void        wall_clock_show(void);                // Print the clock status (ECHO)

#endif
