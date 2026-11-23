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
#include "journal.h"

typedef bool (*shot_journal_cb_t)(shot_record_t *shot, void *ctx); // Return false to stop the replay
typedef void (*shot_journal_notify_t)(void);                        // Called after every new entry

/*
 * Cursor: reads the journal from a given seq onward and keeps
 * following new entries as they are written.
 */
typedef struct
{
  jrnl_iter_t it;
} shot_journal_cursor_t;

typedef struct
{
  int           kind;           // JRNL_SHOT, JRNL_SESSION or JRNL_MODE
  unsigned int  seq;
  unsigned int  session;
  unsigned int  session_type;   // SESSION_xxx (without SESSION_VALID)
  unsigned int  target;         // JRNL_SESSION / JRNL_MODE: target type code (0 = unknown)
  unsigned int  utc;            // Unix time of the entry (0 = unknown)
  shot_record_t shot;           // JRNL_SHOT only
} shot_journal_event_t;

/*
 * One session, as listed by shot_journal_sessions()
 */
typedef struct
{
  unsigned int session;         // Session number
  unsigned int first_seq;       // seq of its first and last entries
  unsigned int last_seq;
  unsigned int shots;           // Shots (including misses)
  unsigned int sighters;        // of which sighters
  unsigned int target;          // Target type code (0 = unknown)
  unsigned int session_type;    // Latest SESSION_xxx (without SESSION_VALID)
  unsigned int first_utc;       // Unix time of the first and last entries (0 = unknown)
  unsigned int last_utc;
} shot_journal_summary_t;

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
void shot_journal_set_notify(shot_journal_notify_t f);              // Install the new entry callback
void shot_journal_cursor_start(shot_journal_cursor_t *c, unsigned int after_seq); // after_seq 0 = start of the current session
void shot_journal_cursor_start_all(shot_journal_cursor_t *c);       // From the oldest entry still in the journal
bool shot_journal_cursor_next(shot_journal_cursor_t *c, shot_journal_event_t *ev); // false when caught up (call again later)
unsigned int shot_journal_session(void);                            // Current session number (0 = none)
unsigned int shot_journal_next_seq(void);                           // seq the next entry will get
int  shot_journal_sessions(shot_journal_summary_t *list, int max);  // The newest max sessions, oldest first; returns the count

#endif
