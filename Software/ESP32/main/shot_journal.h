/*----------------------------------------------------------------
 *
 * shot_journal.h
 *
 * ESP32 binding of the shot journal (journal.c)
 *
 *----------------------------------------------------------------
 *
 * Shots are written to the "shots" flash partition as soon as they
 * are scored, so they survive a power loss.  If the partition is not
 * present (a unit that was only updated over the air keeps its old
 * partition table) a small RAM journal is used instead.  That keeps
 * the replay-on-connect behaviour but not the power-loss protection.
 *
 *--------------------------------------------------------------*/
#ifndef _SHOT_JOURNAL_H_
#define _SHOT_JOURNAL_H_

#include <stdbool.h>
#include <stdint.h>

#include "freETarget.h"

typedef bool (*shot_journal_cb_t)(shot_record_t *shot, void *ctx); // Return false to stop the replay

void shot_journal_init(void);                                       // Open the journal, call once at start up
bool shot_journal_resume(void);                                     // true if a session was recovered from flash
int  shot_journal_session_type(void);                               // Session type (SESSION_SIGHT / MATCH) of the current session
void shot_journal_record(shot_record_t *shot);                      // Save a shot, sets shot->shot, seq, utc and clock
void shot_journal_new_session(int session_type);                    // A new session was started
void shot_journal_set_mode(int session_type);                       // Sighters / match changed
void shot_journal_clock_set(uint32_t unix_time, uint32_t uptime, int source); // The wall clock was set
uint32_t shot_journal_last_ntp_time(void);                          // Newest NTP time in the journal (0 = none)
int  shot_journal_replay_session(shot_journal_cb_t f, void *ctx);   // Replay the current session, returns the count
void shot_journal_show(void);                                       // Print the journal status (ECHO)

#endif
