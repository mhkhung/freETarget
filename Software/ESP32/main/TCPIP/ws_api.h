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
 * Not on port 80: that server is held up for as long as the main
 * page waits for a shot in /events.  A test page is at
 * http://<target>:8080/live
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
 *   anything else  A JSON command, exactly as over USB or TCP, for
 *                  example {"TIME": 1795000000} or {"ECHO":0}.
 *
 * Target to client
 *
 *   {"hello":"freETarget", ...}   Sent once on connection.  Carries
 *                  the firmware version, target name, current session
 *                  and the seq the next entry will get ("next_seq").  If
 *                  next_seq is not greater than the last seq a client
 *                  saw, the journal was erased: start over with
 *                  {"AFTER":0}.
 *
 *   {"session":5, "session_type":1, "seq":40, "change":"new"}
 *                  A session started ("new") or switched between
 *                  sighters and match ("mode").  Shots that follow
 *                  belong to it.
 *
 *   {"shot":3, "session":5, "seq":44, ...}
 *                  A shot, in journal order.  "seq" is unique and never
 *                  reused; remember the last one seen and send it with
 *                  {"AFTER":n} after reconnecting.
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
