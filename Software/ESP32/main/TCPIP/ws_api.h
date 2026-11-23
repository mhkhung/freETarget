/*----------------------------------------------------------------
 *
 * ws_api.h
 *
 * WebSocket API for browser and other network clients
 *
 *----------------------------------------------------------------
 *
 * Endpoint:  ws://<target>:8080/ws   (the control web server)
 *
 * Not on port 80, so that a /json request there (which waits for
 * its command to finish) never holds up the shots.  The main page
 * (FreeETarget.html) and the test page http://<target>:8080/live
 * both use this API.
 *
 * Every message is one text frame.
 *
 * Client to target
 *
 *   {"AFTER": n}   Send shots from the journal, then keep sending new
 *                  ones as they are fired.
 *                    0  from the start of the current session
 *                    n  everything after seq n (to resume after a
 *                       dropped connection without duplicates)
 *                  Must be sent on its own.  Sending it again starts
 *                  over from the new position.
 *
 *   {"SESSIONS": n}  List the newest n sessions in the journal (0 = up
 *                  to 50).  Answered with {"sessions":[...]}.
 *
 *   {"HISTORY": s} Replay session s from the journal.  Every message of
 *                  the replay carries "history": s, and it ends with
 *                  {"history_done": s}.  Live shots keep coming in
 *                  between.
 *
 *                  AFTER, SESSIONS and HISTORY are answered to this
 *                  client only and may be combined in one frame.
 *
 *   anything else  A JSON command, exactly as over USB or TCP, for
 *                  example {"TIME": 1795000000} or {"ECHO":0}.
 *
 * Target to client
 *
 *   {"hello":"freETarget", ...}   Sent once on connection.  Carries
 *                  the firmware version, target name, current session,
 *                  the seq the next entry will get ("next_seq"), and the
 *                  current target type, session type, athlete, event and
 *                  target name, and "session_open" (false once the
 *                  session has ended).  If
 *                  next_seq is not greater than the last seq a client
 *                  saw, the journal was erased: start over with
 *                  {"AFTER":0}.
 *
 *   {"session":5, "session_type":2, "seq":40, "change":"new",
 *    "target":110, "utc":1795000000}
 *                  A session started ("new") or switched between
 *                  sighters (2) and match (4) ("mode"); 0 is practice.
 *                  {"change":"end", "reason":"command"|"idle"} marks the
 *                  end of the session: by {"SESSION_END":0} or after an
 *                  hour without activity.  The next shot starts a new
 *                  session.
 *                  "target" is the target type when it happened
 *                  (100 air pistol, 110 air rifle, 500 50 m pistol,
 *                  510 50 m rifle, +1 for practice; 0 unknown) and "utc"
 *                  its time (0 unknown).  Shots that follow belong to it.
 *
 *   {"sessions":[{"session":5, "first_seq":40, "last_seq":75,
 *    "shots":30, "sighters":5, "target":110, "session_type":4,
 *    "first_utc":..., "last_utc":...}, ...]}
 *                  Answer to {"SESSIONS":n}, oldest first.
 *
 *   {"shot":3, "session":5, "seq":44, ...}
 *                  A shot, in journal order.  "seq" is unique and never
 *                  reused; remember the last one seen and send it with
 *                  {"AFTER":n} after reconnecting.  "target", "athelete",
 *                  "event" and "target_name" are the target's current
 *                  settings, as in the TCP score messages.
 *
 *   {"caught_up": 44}
 *                  The replay requested by {"AFTER":n} is complete; what
 *                  follows is live.
 *
 *   any other text Replies to commands, as on the USB console.
 *
 *--------------------------------------------------------------*/
#ifndef _WS_API_H_
#define _WS_API_H_

#include "esp_http_server.h"

void ws_api_register(httpd_handle_t server); // Add /ws to a web server (the port 8080 one)
void ws_api_console(const char *text);        // Queue command output for the clients (never blocks)
void ws_api_closed(int fd);                    // A web server socket was closed

#endif
