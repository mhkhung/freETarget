/*----------------------------------------------------------------
 *
 * timed_fire.c
 *
 * Specific timed fire event
 *
 *-------------------------------------------------------------*/
#include "esp_timer.h"
#include "driver/gpio.h"
#include "stdio.h"
#include "string.h"

#include "freETarget.h"
#include "helpers.h"
#include "gpio.h"
#include "gpio_define.h"
#include "analog_io.h"
#include "json.h"
#include "mechanical.h"
#include "timer.h"
#include "diag_tools.h"
#include "timed_fire.h"
#include "shot_journal.h"

/*
 * Definitions and includes
 */
#define LED_DARK 0
#define LED_ON   1
#define LED_RAMP 2

/*
 * Function prototypes
 */
static bool timed_fire_active(void);          // TRUE if the timed fire event is active
static bool timed_fire_start_new_cycle(void); // TRUE if a new timed fire cycle is starting
static void timed_fire_next_state(void);      // Start the next state in the timed fire sequence
static void timed_fire_exit(void);            // Handle the exit of the timed fire event
static bool timed_fire_finish_window(void);   // Report a miss once the closed window's shots have been reduced
static void timed_fire_event_override(void);  // Change settings based on the event name (e.g. shot count override in the event name)

/*
 *  Variables
 */
typedef struct
{
  real_t *timer;              // Timer used to control state length (from json.c)
  int     time_scale;         // Scaling factor for the timer (e.g. ONE_SECOND or TEN_MILLISECONDS)
  char   *status_LED;         // Status LED output
  int     LED_bright;         // Brightness of target LED
  char   *message;            // Message to be sent to PC
  int     in_shot;            // In a shot cycle
  int    *exit_condition;     // Pointer to the exit condition (0 == exit)
  int     zero;               // Where to go next if the exit condition is zero
  int     not_zero;           // Where to go if the exit condition is non-zero
} rapid_state_t;

typedef struct
{
  char          *event;       // What event are we shooting
  rapid_state_t *rapid_state; // State machine for this event
  void (*start_up)(void);     // Function to call to start the relay
  char score_mode;            // What score mode to use for this event
} course_of_fire_t;

/*
 * Variables
 */
time_count_t event_timer; // Timer used for rapid fire ecents

// Timer to reset LED status
static real_t go_wait     = 3l;            // Wait for the PC to catchup
static real_t grace_time  = 30;            // ISSF grace time for in flight shots (30 x 10ms)
static real_t all_done    = 0l;            // All finished
static int    always_true = 0;             // Force exit condition to be true (time ran out)

static real_t         adjusted_rapid_wait; // Time interval removing grace period
static int            cycle_count;         // How many have we received
static rapid_state_t *rapid_state = NULL;  // What state table to use
static unsigned int   rapid_index;         // Index of the current state
static int            last_enable = 0;
static char           score_mode;          // What score mode are we in?
static volatile int   scored_shots;        // Shots scored by reduce() during this string
static int            window_start_shots;  // Accepted shots before the current shooting window
static bool           window_miss_pending; // Closed window waiting for its queued shots to be reduced

/*
 * State tables for timed fire events
 */
const rapid_state_t rapid_state_ISSF[] = {
    {&all_done,        ONE_SECOND, LED_RAPID_OFF,  LED_RAMP, "RAPID_IDLE",    0,       &always_true, 1, 1}, // 0 Do nothing
    {&go_wait,         ONE_SECOND, LED_RAPID_OFF,  LED_RAMP, "RAPID_ENABLED", 0,       &always_true, 2, 2}, // 1 Wait for json_rapid_enable
    {&json_rapid_wait, ONE_SECOND, LED_RAPID_WARN, LED_ON,   "RAPID_WAIT",    0,       &always_true, 3, 3}, // 2 The event is enabled
    {&json_rapid_time, ONE_SECOND, LED_RAPID_ON,   LED_ON,   "RAPID_ON",      IN_SHOT, &always_true, 4, 4}, // 3 Timer on for the event
    {&grace_time,      TICK_10ms,  LED_RAPID_WARN, LED_RAMP, "RAPID_GRACE",   IN_SHOT, &always_true, 5, 5}, // 4 Delay for the grace period
    {&json_rapid_wait, ONE_SECOND, LED_RAPID_WARN, LED_RAMP, "RAPID_EXIT",    0,       &always_true, 6, 6}, // 5 Keep the RED light on for a while
    {&all_done,        ONE_SECOND, LED_RAPID_OFF,  LED_RAMP, "ALL_DONE",      0,       &always_true, 0, 0}  // 6 Event finished, turn off
};

const rapid_state_t rapid_state_sport[] = {
    {&all_done,            ONE_SECOND, LED_RAPID_OFF,  LED_RAMP, "SPORT_IDLE",    0,       &always_true, 1, 1}, // 0 Do nothing
    {&go_wait,             ONE_SECOND, LED_RAPID_OFF,  LED_RAMP, "SPORT_ENABLED", 0,       &always_true, 2, 2}, // 1 Wait for json_rapid_enable
    {&json_rapid_wait,     ONE_SECOND, LED_RAPID_WARN, LED_RAMP, "SPORT_WAIT_1",  0,       &always_true, 3, 3}, // 2 Warn the shooter
    {&json_rapid_time,     ONE_SECOND, LED_RAPID_ON,   LED_ON,   "SPORT_ON_1",    IN_SHOT, &always_true, 4, 4}, // 3 Timer on for the event
    {&grace_time,          TICK_10ms,  LED_RAPID_WARN, LED_ON,   "SPORT_GRACE_1", IN_SHOT, &always_true, 5, 5}, // 4 Delay for the grace period
    {&adjusted_rapid_wait, 1,          LED_RAPID_WARN, LED_RAMP, "SPORT_WAIT_2",  0,       &cycle_count, 6, 3}, // 5 Warn the event is enabled
    {&all_done,            ONE_SECOND, LED_RAPID_WARN, LED_RAMP, "ALL_DONE",      0,       &always_true, 0, 0}  // 8 Event finished, turn off
};

const rapid_state_t tabata_rapid_state[] = {
    //                                                                                       next    T  F
    {&all_done,        ONE_SECOND, LED_TABATA_OFF,  LED_DARK, "TABATA_IDLE",  0,       &always_true, 1, 1}, // 0 Wait for json_tabata_enable
    {&json_rapid_wait, ONE_SECOND, LED_TABATA_WARN, LED_RAMP, "TABATA_BEGIN", 0,       &always_true, 2, 2}, // 1 Wait for json_tabata_enable
    {&json_rapid_time, ONE_SECOND, LED_TABATA_ON,   LED_ON,   "TABATA_ON",    IN_SHOT, &always_true, 3, 3}, // 2 Wait for json_tabata_enable
    {&grace_time,      TICK_10ms,  LED_TABATA_OFF,  LED_DARK, "TABATA_GRACE", IN_SHOT, &cycle_count, 4, 4}, // 3 Delay for the grace period
    {&all_done,        ONE_SECOND, LED_TABATA_OFF,  LED_DARK, "TABATA_DONE",  0,       &always_true, 0, 1}, // 4 Fake the rest period
};

/*
 * Functions to start timed fire events
 */
static void start_rapid_fire(void)
{
  //  {"ATHLETE":"Allan Brown", "EVENT":"RFP", "TARGET_NAME":"freETarget.targets.AirPistol"}
  //  {"ATHLETE":"Allan Brown", "EVENT":"RFP -n4", "TARGET_NAME":"freETarget.targets.AirPistol"}
  //  {"TRACE":2048, "RAPID_COUNT":1, "RAPID_WAIT":20, "RAPID_TIME":240, "RAPID_ENABLE": 1 }
  //  {"TRACE":2048, "PAPER_TIME":0, "EVENT":"RFP -n4 Allan", "RAPID_COUNT": 1 , "RAPID_WAIT":3, "RAPID_TIME":8, "RAPID_ENABLE": 1 }
  //  {"TRACE":2048, "PAPER_TIME":0, "EVENT":"RFP Allan", "RAPID_COUNT": 1 , "RAPID_WAIT":3, "RAPID_TIME":8, "RAPID_ENABLE": 1 }
  //  {              "PAPER_TIME":0, "EVENT":"RFP Allan", "RAPID_COUNT": 1 , "RAPID_WAIT":3, "RAPID_TIME":8, "RAPID_ENABLE": 1 }

  json_rapid_count = 5;        // Expect five shots in this one session

  event_override(); // Look for a shot count in the event name
}

static void start_sport_pistol(void)
{
  //  {"TRACE":2048, "TRACE":8, "TRACE":64, "PAPER_TIME":0, "EVENT":"SPP Allan", "RAPID_COUNT":5, "RAPID_WAIT":7, "RAPID_TIME":50,
  //  "RAPID_ENABLE": 1 }
  //  {"TRACE":2048, "PAPER_TIME":0, "EVENT":"SPP Allan", "RAPID_COUNT":5, "RAPID_WAIT":7, "RAPID_TIME":50, "RAPID_ENABLE": 1 }
  //  {"PAPER_TIME":0, "EVENT":"SPP Allan", "RAPID_COUNT":5, "RAPID_WAIT":7, "RAPID_TIME":50, "RAPID_ENABLE": 1 }
  //  {"PAPER_TIME":0, "EVENT":"SPP Allan", "RAPID_COUNT":5, "RAPID_WAIT":7, "RAPID_TIME":50, "RAPID_ENABLE": 0 }
  real_t temp;

  event_override();                // Look for a shot count in the event name

  temp = (real_t)json_rapid_time;             // Total event time
  temp -= json_rapid_count * json_rapid_wait; // Subtract the total wait time between shots
  temp /= json_rapid_count;                   // Calculate the average time per shot
  json_rapid_time = temp;                     // Update the rapid time with the calculated average per shot
  return;
}

static void start_tabata(void)
{
  // {"PAPER_TIME":0, "TRACE":2048, "TRACE":8, "TRACE":64,"EVENT":"TBT Allan", "RAPID_COUNT": 1 , "RAPID_WAIT":3, "RAPID_TIME":7,
  // "RAPID_ENABLE": 1 }
  // {"PAPER_TIME":0, "TRACE":2048, "EVENT":"TBT Allan", "RAPID_COUNT": 1 , "RAPID_WAIT":3, "RAPID_TIME":7, "RAPID_ENABLE": 1 }
  // {"PAPER_TIME":0, "EVENT":"TBT Allan", "RAPID_COUNT": 1 , "RAPID_WAIT":3, "RAPID_TIME":7, "RAPID_ENABLE": 1 }
  event_override(); // Look for a shot count in the event name
  return;
}

/*
 * Course of fire definitions
 */
const course_of_fire_t course_of_fire[] = {
    {"RFP", &rapid_state_ISSF,   &start_rapid_fire,   'E'}, // Rapid fire pistol
    {"SPP", &rapid_state_sport,  &start_sport_pistol, 'D'}, // Sport pistol
    {"TBT", &tabata_rapid_state, &start_tabata,       'D'}, // Tabata training
    {NULL,  0,                   0,                   0  }
};

/*----------------------------------------------------------------
 *
 * @function: timed_fire
 *
 * @brief:    Start or stop a timed fire session
 *
 * @return:   Nothing
 *
 *----------------------------------------------------------------
 *
 * This is the function that carries out the timing for a timed
 * fire event.
 *
 * The function waits for json_rapid_enable to start the timed fire event.
 * When that happens, the state machine is initialized and the correct
 * state machine begins sequencing.
 *
 * The state begins by setting the timeduration for that state.  When
 * the time runs out, the state machine transitions to the next state.
 * At that time, any special work that is needed in that state is
 * perfomed and the state machine continues to the next state.
 *
 * When the last state is reached, the timed fire event is concluded.
 *
 * Once this function has been executed, the lights will be turned off
 * in anticipation of the next event.
 *
 * rapid_state_ISSF - Standard rapid fire sequence, ex 5 shots, one target
 * rapid_state_sport - Standard sport fire sequence, ex 1 shot into one target five times
 * tabata_rapid_state - Standard tabata training sequence
 *
 * ------------------------------------------------------------*/
void timed_event_task(void)
{
  if ( !timed_fire_finish_window() )
  {
    return; // Keep the lights closed until pending shots have been judged
  }

  /*
   *  Check exit conditions
   */
  if ( timed_fire_active() == false )
  {
    return;
  }

  /*
   *  Look to see if we are starting a new timed fire sequence
   */
  timed_fire_start_new_cycle();

  /*
   *  Go to the next state in the rapid fire sequence
   */
  timed_fire_next_state();

  /*
   *  Check to see if we have reached the end of the timed fire event
   */
  timed_fire_exit();

  DLT(DLT_RAPID_FIRE, SEND(CONSOLE, sprintf(_xs, "State: %s,  time: %ld, LED: \"%s\", IN_SHOT: %d", (rapid_state + rapid_index)->message,
                                            event_timer, (rapid_state + rapid_index)->status_LED, (rapid_state + rapid_index)->in_shot);))

  /*
   * All done.
   */
  return;
}

/*----------------------------------------------------------------
 *
 * @function: report_miss
 *
 * @brief:    Save and send a missed timed fire shot
 *
 *----------------------------------------------------------------
 *
 * Uses its own record, so the shot queue that the interrupt and
 * reduce() share is not touched.
 *
 *--------------------------------------------------------------*/
static void report_miss(void)
{
  shot_record_t miss;
  char          score[SCORE_TEXT];               // JSON score message

  memset(&miss, 0, sizeof(miss));
  miss.miss         = 1;
  miss.x_mm         = 1000;                       // Way off the target
  miss.y_mm         = 1000;
  miss.shot_time    = run_time_ms();
  miss.session_type = SESSION_VALID | json_session_type;

  shot_journal_record(&miss);                     // Save the miss, sets the shot number and seq
  build_json_score(&miss, SCORE_USB, score, sizeof(score));
  serial_to_all(score, ALL_SCORES);
}

/*----------------------------------------------------------------
 *
 * @function: timed_fire_scored
 *
 * @brief:    reduce() scored a shot
 *
 *--------------------------------------------------------------*/
void timed_fire_scored(void)
{
  IF_IN(IN_RAPID)
  {
    scored_shots++;
  }
}

/*
 * Acquisition alone is not a shot: reduce() may reject a neighbouring
 * lane's trigger.  Wait for the queue to drain before deciding whether
 * a closed window needs a miss, including shots captured at its end.
 */
static bool timed_fire_finish_window(void)
{
  if ( !window_miss_pending )
  {
    return true;
  }
  if ( shot_in != shot_out )
  {
    return false;
  }

  window_miss_pending = false;
  if ( scored_shots == window_start_shots )
  {
    report_miss();
  }
  return true;
}

/*----------------------------------------------------------------
 *
 * @function: timed_fire_active
 *
 * @brief:    See if the timed fire is active
 *
 * @return:   true if the timed fire is active, false otherwise
 *
 * ------------------------------------------------------------*/
static bool timed_fire_active(void)
{
  IF_NOT(IN_OPERATION)
  {
    return false;
  }

  if ( event_timer != 0 )          // Time has not yet elapsed
  {
    return false;
  }

  return (json_rapid_enable == 1); // Nothing to do if Rapid fire is not enabled
}

/*----------------------------------------------------------------
 *
 * @function: timed_fire_start_new_cycle
 *
 * @brief:    Start a new cycle of the timed fire
 *
 * @return:   void
 *
 *---------------------------------------------------------------
 *
 * Look for a transition to ON.
 *
 * If so, then find the state machine belonging to this event
 * and set up the new cycle
 *
 * ------------------------------------------------------------*/
static bool timed_fire_start_new_cycle(void)
{
  int i;

  /*
   * See if there is a transition to ON
   */
  if ( (json_rapid_enable & (last_enable ^ json_rapid_enable)) == 0 )
  {
    return false;
  }

  run_state |= IN_RAPID;

  /*
   * Look for the state machine
   */
  i = 0;
  while ( (course_of_fire[i].event != NULL) && !contains(json_event, course_of_fire[i].event) )
  {
    i++;
  }

  if ( course_of_fire[i].event == NULL ) // Not a known event, use the first one (rapid fire pistol)
  {
    i = 0;
  }

  rapid_state = course_of_fire[i].rapid_state;
  score_mode  = course_of_fire[i].score_mode;
  if ( course_of_fire[i].start_up != NULL )
  {
    course_of_fire[i].start_up();        // Perform the start-up action for this state machine
  }

  /*
   * Setup the new cycle
   */
  event_timer         = ((int)*(rapid_state->timer)) * ONE_SECOND;
  rapid_index         = 0;                                           // Start at the beginning of the state machine
  scored_shots        = 0;                                           // No shots yet
  window_miss_pending = false;
  adjusted_rapid_wait = (json_rapid_wait * ONE_SECOND) - grace_time; // Corrected rapid wait time
  cycle_count         = json_rapid_count;                            // Initialize the cycle count with the number of expected shots

  DLT(DLT_RAPID_FIRE, SEND(CONSOLE, sprintf(_xs, "Starting: %s, cycle_count: %d, Wait: %4.2f, On: %4.2f", course_of_fire[i].event,
                                            cycle_count, json_rapid_wait, json_rapid_time);))

  /*
   *  All done, exit
   */

  last_enable = json_rapid_enable;
  return true;
}

/*----------------------------------------------------------------
 *
 * @function: timed_fire_start_next_state
 *
 * @brief:    The event has completed this state, transition to the next state if necessary
 *
 * @return:   void
 *
 *---------------------------------------------------------------
 *
 * Look for a transition to ON.
 *
 * If so, then find the state machine belonging to this event
 * and set up the new cycle
 *
 * ------------------------------------------------------------*/
static void timed_fire_next_state(void)
{
  unsigned int last_shot_state = (rapid_state + rapid_index)->in_shot;

  if ( *((rapid_state + rapid_index)->exit_condition) != 0 ) // Decrement the exit condition if it is not zero
  {
    (*(rapid_state + rapid_index)->exit_condition)--;
  }

  if ( *((rapid_state + rapid_index)->exit_condition) == 0 ) // Pick the next state based on the exit condition
  {
    rapid_index = (rapid_state + rapid_index)->zero;
  }
  else
  {
    rapid_index = (rapid_state + rapid_index)->not_zero;
  }

  /*
   *  Set the timer for the next state
   */
  event_timer = ((int)*(rapid_state + rapid_index)->timer) * (rapid_state + rapid_index)->time_scale; // New time

  switch ( (rapid_state + rapid_index)->LED_bright )       // Set the LED brightness based on the state
  {
    case LED_RAMP:
      set_LED_PWM_now(10);                                 // Ramp the lights on
      break;

    case LED_ON:
      set_LED_PWM_now(json_LED_PWM);                       // Turn on the lights
      break;

    case LED_DARK:
      set_LED_PWM_now(0);                                  // Turn off the lights
      break;
  }

  set_status_LED((rapid_state + rapid_index)->status_LED); // Set the red and green light s

  /*
   * Update the run state
   */
  if ( !last_shot_state && (rapid_state + rapid_index)->in_shot ) // Set the baseline before opening the window
  {
    window_start_shots = scored_shots;
    run_state &= ~IN_AQUIRE;
  }
  run_state = (run_state & ~IN_SHOT) | (rapid_state + rapid_index)->in_shot;

  if ( last_shot_state && !(run_state & IN_SHOT) && (score_mode == 'D') )
  {
    window_miss_pending = true;
    timed_fire_finish_window();
  }

  /*
   *  All done
   */
  return;
}

/*----------------------------------------------------------------
 *
 * @function: timed_fire_exit
 *
 * @brief:    The timed fire event has ended, handle any necessary cleanup
 *
 * @return:   void
 *
 *---------------------------------------------------------------
 *
 * Look for a transition to ON.
 *
 * If so, then find the state machine belonging to this event
 * and set up the new cycle
 *
 * ------------------------------------------------------------*/
static void timed_fire_exit(void)
{
  /*
   * Check to see if we are at the last state
   */
  if ( rapid_index != 0 ) // Not at the end of the state machine, so nothing to do
  {
    return;
  }

  /*
   *  Handle any missed shots
   */
  if ( score_mode == 'E' )                           // Report the shots that were not fired as misses
  {
    for ( int n = scored_shots; n < json_rapid_count; n++ )
    {
      report_miss();
    }
  }

  /*
   *  Finished the timed fire event
   */
  run_state = (run_state & ~IN_RAPID) | IN_SHOT;    // Back to normal shooting
  json_rapid_enable = 0;                    // No longer enabled
  last_enable       = 0;
  DLT(DLT_RAPID_FIRE, SEND(CONSOLE, sprintf(_xs, "Timed fire event ended");))
  return;
}

