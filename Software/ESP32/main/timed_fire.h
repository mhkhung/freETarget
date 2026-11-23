/*----------------------------------------------------------------
 *
 * timed_fire.h
 *
 * Software to run the Air-Gun / Small Bore Electronic Target
 *
 *--------------------------------------------------------------*/

#ifndef _TIMED_FIRE_H
#define _TIMED_FIRE_H

extern time_count_t event_timer; // Timer used for rapid fire events

/*
 * timed fire functions
 */
void timed_event_task(void); // Run the Rapid Fire state machine
void timed_fire_scored(void); // reduce() scored a shot (counts the shots of a timed fire string)

#endif
