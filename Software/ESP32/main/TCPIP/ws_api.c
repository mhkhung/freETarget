/*----------------------------------------------------------------
 *
 * ws_api.c
 *
 * WebSocket API.  See ws_api.h for the protocol.
 *
 *----------------------------------------------------------------
 *
 * Tasks
 *
 *   The web server task runs ws_handler(): it accepts connections and
 *   receives frames.  It never sends, and only touches the client table
 *   under client_lock.
 *
 *   The ws_sender task does all of the sending.  Each client has its own
 *   journal cursor, so a client that connects late, or reconnects, gets
 *   exactly the shots it has not seen.  The sender is woken whenever the
 *   journal is written or command output is queued.
 *
 *   Frames are sent with httpd_ws_send_data(), which hands the frame to
 *   the web server task and waits.  The client table lock is never held
 *   while waiting, so the web server task can always make progress.
 *
 *--------------------------------------------------------------*/
#include <stdlib.h>
#include <string.h>

#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "freETarget.h"
#include "diag_tools.h"
#include "helpers.h"
#include "json.h"
#include "serial_io.h"
#include "shot_journal.h"
#include "wall_clock.h"
#include "ws_api.h"

#define WS_URI          "/ws"
#define WS_MAX_CLIENTS  4              // Connected WebSocket clients
#define WS_MAX_COMMAND  512            // Longest frame accepted from a client
#define WS_BATCH        16             // Journal entries sent to one client before serving the next
#define WS_CONSOLE_SIZE 4096           // Command output waiting to be sent
#define WS_TEXT_SIZE    1024           // Largest console frame
#define WS_STACK        (6 * 1024)
#define WS_PRIORITY     4              // Same as the TCP polling tasks
#define WS_SHOT_FIELDS  "SNQM?TUXPOE"  // Shot, session, seq, miss, session type, time, utc, x/y, polar, target type, names
#define WS_MAX_SESSIONS 50             // Sessions listed by {"SESSIONS":n}

/*
 * Shared between the web server task and the sender (client_lock)
 */
typedef struct
{
  bool         used;                   // Slot in use
  int          fd;                     // Socket
  unsigned int gen;                    // Changes whenever the slot gets a new client
  bool         hello_pending;          // Send the hello message
  bool         after_pending;          // {"AFTER":n} received
  unsigned int after;                  // n
  bool         sessions_pending;       // {"SESSIONS":n} received
  unsigned int sessions;               // n
  bool         history_pending;        // {"HISTORY":s} received
  unsigned int history;                // s
} ws_client_t;

/*
 * Private to the sender task
 */
typedef struct
{
  unsigned int          gen;           // Client this state belongs to
  bool                  subscribed;    // Streaming shots
  bool                  caught_up;     // {"caught_up"} has been sent
  unsigned int          last_seq;      // Last entry sent
  shot_journal_cursor_t cursor;
  bool                  hist_active;   // Replaying a past session
  unsigned int          hist_session;  // which one
  shot_journal_cursor_t hist_cursor;
} ws_stream_t;

static httpd_handle_t    ws_server;
static ws_client_t       clients[WS_MAX_CLIENTS];
static ws_stream_t       streams[WS_MAX_CLIENTS];
static SemaphoreHandle_t client_lock;
static StaticSemaphore_t client_lock_buffer;
static RingbufHandle_t   console;      // Command output for the clients
static TaskHandle_t      sender;
static volatile int      client_count; // Read without the lock to skip work when nobody listens
static unsigned int      next_gen = 1;

static void wake_sender(void)
{
  if ( sender != NULL )
  {
    xTaskNotifyGive(sender);
  }
}

/*----------------------------------------------------------------
 *
 * Client table
 *
 *--------------------------------------------------------------*/
static void add_client(int fd)
{
  int i;
  int slot = -1;

  xSemaphoreTake(client_lock, portMAX_DELAY);
  for ( i = 0; i != WS_MAX_CLIENTS; i++ )
  {
    if ( clients[i].used && (clients[i].fd == fd) ) // The socket was reused, replace the old client
    {
      slot = i;
      break;
    }
    if ( !clients[i].used && (slot < 0) )
    {
      slot = i;
    }
  }

  if ( slot >= 0 )
  {
    if ( !clients[slot].used )
    {
      client_count++;
    }
    memset(&clients[slot], 0, sizeof(clients[slot]));
    clients[slot].used          = true;
    clients[slot].fd            = fd;
    clients[slot].gen           = next_gen++;
    clients[slot].hello_pending = true;
  }
  xSemaphoreGive(client_lock);

  if ( slot < 0 )
  {
    DLT(DLT_CRITICAL, SEND(CONSOLE, sprintf(_xs, "WebSocket: too many clients, closing %d", fd);))
    httpd_sess_trigger_close(ws_server, fd);
    return;
  }

  DLT(DLT_COMMUNICATION, SEND(CONSOLE, sprintf(_xs, "WebSocket client %d connected", fd);))
  wake_sender();
}

static bool remove_client(int fd, unsigned int gen) // gen 0 = any client on this socket.  True if one was removed
{
  int  i;
  bool removed = false;

  xSemaphoreTake(client_lock, portMAX_DELAY);
  for ( i = 0; i != WS_MAX_CLIENTS; i++ )
  {
    if ( clients[i].used && (clients[i].fd == fd) && ((gen == 0) || (clients[i].gen == gen)) )
    {
      clients[i].used = false;
      client_count--;
      removed         = true;
    }
  }
  xSemaphoreGive(client_lock);
  return removed;
}

void ws_api_closed(int fd)
{
  if ( client_lock != NULL )
  {
    remove_client(fd, 0);
  }
}

/*
 * Find "key": number in a frame.  Returns true and sets *value (0 if
 * negative) if the key is present.
 */
static bool ws_number(const char *text, const char *key, long *value)
{
  const char *p = strstr(text, key);

  if ( p == NULL )
  {
    return false;
  }

  p += strlen(key);
  while ( (*p == ' ') || (*p == ':') )
  {
    p++;
  }
  *value = strtol(p, NULL, 10);
  if ( *value < 0 )
  {
    *value = 0;
  }
  return true;
}

/*----------------------------------------------------------------
 *
 * @function: ws_handler
 *
 * @brief:    Web server callback for /ws
 *
 *--------------------------------------------------------------*/
static esp_err_t ws_handler(httpd_req_t *req)
{
  httpd_ws_frame_t frame;
  char             text[WS_MAX_COMMAND + 1];
  int              fd = httpd_req_to_sockfd(req);
  int              i;
  long             n        = -1;
  long             sessions = -1;
  long             history  = -1;

  if ( req->method == HTTP_GET )                       // Handshake done, a new client
  {
    add_client(fd);
    return ESP_OK;
  }

  memset(&frame, 0, sizeof(frame));
  if ( httpd_ws_recv_frame(req, &frame, 0) != ESP_OK ) // Find the length
  {
    return ESP_FAIL;
  }

  if ( frame.len > WS_MAX_COMMAND )                     // Closes the connection
  {
    DLT(DLT_CRITICAL, SEND(CONSOLE, sprintf(_xs, "WebSocket: %d byte frame refused", (int)frame.len);))
    return ESP_FAIL;
  }

  frame.payload = (uint8_t *)text;
  if ( (frame.len != 0) && (httpd_ws_recv_frame(req, &frame, frame.len) != ESP_OK) )
  {
    return ESP_FAIL;
  }
  text[frame.len] = 0;

  if ( frame.type != HTTPD_WS_TYPE_TEXT )
  {
    return ESP_OK;                                       // Ignore binary frames
  }

  if ( ws_number(text, "\"AFTER\"", &n) | ws_number(text, "\"SESSIONS\"", &sessions) | ws_number(text, "\"HISTORY\"", &history) )
  {
    xSemaphoreTake(client_lock, portMAX_DELAY);
    for ( i = 0; i != WS_MAX_CLIENTS; i++ )
    {
      if ( clients[i].used && (clients[i].fd == fd) )
      {
        if ( n >= 0 )                                    // {"AFTER": n}
        {
          clients[i].after         = (unsigned int)n;
          clients[i].after_pending = true;
        }
        if ( sessions >= 0 )                             // {"SESSIONS": n}
        {
          clients[i].sessions         = (unsigned int)sessions;
          clients[i].sessions_pending = true;
        }
        if ( history > 0 )                               // {"HISTORY": s}
        {
          clients[i].history         = (unsigned int)history;
          clients[i].history_pending = true;
        }
      }
    }
    xSemaphoreGive(client_lock);
    wake_sender();
    return ESP_OK;
  }

  tcpip_socket_2_queue(text, frame.len);                // An ordinary JSON command
  return ESP_OK;
}

/*----------------------------------------------------------------
 *
 * @function: ws_api_console
 *
 * @brief:    Queue command output for the WebSocket clients
 *
 *----------------------------------------------------------------
 *
 * Called from serial_to_all() with the _xs lock held, so it must not
 * block.  Output is dropped if nobody is connected or the queue is full.
 *
 *--------------------------------------------------------------*/
void ws_api_console(const char *text)
{
  size_t length;

  if ( (console == NULL) || (client_count <= 0) || (text == NULL) )
  {
    return;
  }

  length = strlen(text);
  if ( length == 0 )
  {
    return;
  }
  if ( length > WS_TEXT_SIZE )
  {
    length = WS_TEXT_SIZE;
  }

  if ( xRingbufferSend(console, text, length, 0) == pdTRUE )
  {
    wake_sender();
  }
}

/*----------------------------------------------------------------
 *
 * Sending (ws_sender task only)
 *
 *--------------------------------------------------------------*/
static bool send_text(int fd, const char *text, size_t length)
{
  httpd_ws_frame_t frame;

  if ( httpd_ws_get_fd_info(ws_server, fd) != HTTPD_WS_CLIENT_WEBSOCKET )
  {
    return false;
  }

  memset(&frame, 0, sizeof(frame));
  frame.final   = true;
  frame.type    = HTTPD_WS_TYPE_TEXT;
  frame.payload = (uint8_t *)text;
  frame.len     = length;

  return httpd_ws_send_data(ws_server, fd, &frame) == ESP_OK;
}

static void drop(int fd, unsigned int gen)
{
  /*
   * Close the socket only if this client was still on it.  If it had
   * already gone, the web server closed the socket and may have given
   * the same number to a new connection, which must not be closed.
   */
  if ( remove_client(fd, gen) )
  {
    DLT(DLT_COMMUNICATION, SEND(CONSOLE, sprintf(_xs, "WebSocket client %d dropped", fd);))
    httpd_sess_trigger_close(ws_server, fd);
  }
}

static bool send_hello(int fd)
{
  char     text[SHORT_TEXT * 6];
  char     name[SHORT_TEXT];
  uint8_t  source;
  uint32_t now = wall_clock_now(&source);

  target_name(name);
  snprintf(text, sizeof(text), "{\"hello\":\"freETarget\", \"version\":%s, \"name\":\"%.60s\", \"session\":%u, \"next_seq\":%u, "
                               "\"utc\":%lu, \"clock\":\"%s\", \"target\":%d, \"athelete\":\"%.60s\", \"event\":\"%.60s\", "
                               "\"target_name\":\"%.60s\", \"session_type\":%d, \"session_open\":%s, "
                               "\"x_offset\":%.2f, \"y_offset\":%.2f}",
           SOFTWARE_VERSION, name, shot_journal_session(), shot_journal_next_seq(),
           (source == JRNL_SOURCE_NONE) ? 0ul : (unsigned long)now, wall_clock_source_name(source), http_target_type(),
           json_athlete, json_event, json_target_name, shot_journal_session_type(), shot_journal_session_open() ? "true" : "false",
           (double)json_x_offset, (double)json_y_offset);   // The calibration, so clients need not ask for an ECHO
  return send_text(fd, text, strlen(text));
}

static bool send_event(int fd, const shot_journal_event_t *ev, unsigned int history) // history 0 = live
{
  char   text[SCORE_TEXT];
  size_t used = 0;

  text[0] = 0;
  if ( history != 0 )                                    // Replayed from a past session
  {
    used = snprintf(text, sizeof(text), "{\"history\":%u, ", history);
  }
  else
  {
    text[0] = '{';
    used    = 1;
  }

  if ( ev->kind == JRNL_SHOT )
  {
    build_json_score((shot_record_t *)&ev->shot, WS_SHOT_FIELDS, text + used, sizeof(text) - used - 2);
  }
  else
  {
    snprintf(text + used, sizeof(text) - used - 2, "\"session\":%u, \"session_type\":%u, \"seq\":%u, \"change\":\"%s\", "
             "\"target\":%u, \"utc\":%u, \"reason\":\"%s\"", ev->session, ev->session_type, ev->seq,
             (ev->kind == JRNL_SESSION) ? "new" : (ev->kind == JRNL_END) ? "end" : "mode", ev->target, ev->utc,
             (ev->kind != JRNL_END) ? "" : (ev->reason == JRNL_END_IDLE) ? "idle" : "command");
  }
  used           = strlen(text);
  text[used]     = '}';
  text[used + 1] = 0;

  return send_text(fd, text, strlen(text));
}

/*
 * {"sessions":[{...}, ...]} for {"SESSIONS":n}
 */
static bool send_sessions(int fd, unsigned int max)
{
  shot_journal_summary_t *list;
  char                   *text;
  size_t                  size;
  size_t                  used;
  int                     count;
  bool                    ok;

  if ( (max == 0) || (max > WS_MAX_SESSIONS) )
  {
    max = WS_MAX_SESSIONS;
  }

  list = malloc(sizeof(*list) * max);
  size = 32 + max * 200;
  text = malloc(size);
  if ( (list == NULL) || (text == NULL) )
  {
    free(list);
    free(text);
    return true;                                         // Not the client's fault, keep it
  }

  count = shot_journal_sessions(list, (int)max);
  used  = snprintf(text, size, "{\"sessions\":[");
  for ( int i = 0; i != count; i++ )
  {
    used += snprintf(text + used, size - used,
                     "%s{\"session\":%u, \"first_seq\":%u, \"last_seq\":%u, \"shots\":%u, \"sighters\":%u, \"target\":%u, "
                     "\"session_type\":%u, \"first_utc\":%u, \"last_utc\":%u, \"ended\":%s}",
                     (i == 0) ? "" : ", ", list[i].session, list[i].first_seq, list[i].last_seq, list[i].shots, list[i].sighters,
                     list[i].target, list[i].session_type, list[i].first_utc, list[i].last_utc, list[i].ended ? "true" : "false");
  }
  snprintf(text + used, size - used, "]}");

  ok = send_text(fd, text, strlen(text));
  free(list);
  free(text);
  return ok;
}

/*
 * Send the queued command output to every client, joined into frames
 * of up to WS_TEXT_SIZE bytes.
 */
static void send_console(void)
{
  static char  text[WS_TEXT_SIZE + 1];
  int          fds[WS_MAX_CLIENTS];
  unsigned int gens[WS_MAX_CLIENTS];
  size_t       used = 0;
  size_t       length;
  char        *item;
  int          count;
  int          i;

  while ( 1 )
  {
    item = (char *)xRingbufferReceive(console, &length, 0);
    if ( (item != NULL) && ((used + length) <= WS_TEXT_SIZE) )
    {
      memcpy(text + used, item, length);                 // Keep joining
      used += length;
      vRingbufferReturnItem(console, item);
      continue;
    }

    if ( used != 0 )                                      // Send what has been joined so far
    {
      count = 0;
      xSemaphoreTake(client_lock, portMAX_DELAY);
      for ( i = 0; i != WS_MAX_CLIENTS; i++ )
      {
        if ( clients[i].used )
        {
          fds[count]  = clients[i].fd;
          gens[count] = clients[i].gen;
          count++;
        }
      }
      xSemaphoreGive(client_lock);

      for ( i = 0; i != count; i++ )
      {
        if ( !send_text(fds[i], text, used) )
        {
          drop(fds[i], gens[i]);
        }
      }
      used = 0;
    }

    if ( item == NULL )
    {
      return;
    }

    memcpy(text, item, length);                           // Start the next frame with the item that did not fit
    used = length;
    vRingbufferReturnItem(console, item);
  }
}

static bool serve_requests(int slot, const ws_client_t *c);

/*
 * Serve one client.  Returns true if it has more to send.
 */
static bool serve(int slot)
{
  ws_client_t          c;
  ws_stream_t         *s = &streams[slot];
  shot_journal_event_t ev;
  char                 text[SHORT_TEXT];
  int                  n;
  bool                 more;

  xSemaphoreTake(client_lock, portMAX_DELAY);
  c                               = clients[slot];
  clients[slot].hello_pending     = false;
  clients[slot].after_pending     = false;
  clients[slot].sessions_pending  = false;
  clients[slot].history_pending   = false;
  xSemaphoreGive(client_lock);

  if ( !c.used )
  {
    s->subscribed = false;
    return false;
  }

  if ( s->gen != c.gen )                                 // A new client in this slot
  {
    memset(s, 0, sizeof(*s));
    s->gen = c.gen;
  }

  if ( c.hello_pending && !send_hello(c.fd) )
  {
    drop(c.fd, c.gen);
    return false;
  }

  if ( c.after_pending )
  {
    shot_journal_cursor_start(&s->cursor, c.after);
    s->subscribed = true;
    s->caught_up  = false;
    s->last_seq   = c.after;
  }

  more = serve_requests(slot, &c);
  xSemaphoreTake(client_lock, portMAX_DELAY);
  c.used = clients[slot].used && (clients[slot].gen == c.gen);
  xSemaphoreGive(client_lock);
  if ( !c.used )                                         // Dropped while answering
  {
    return false;
  }

  if ( !s->subscribed )
  {
    return more;
  }

  for ( n = 0; n != WS_BATCH; n++ )
  {
    if ( !shot_journal_cursor_next(&s->cursor, &ev) )
    {
      if ( !s->caught_up )
      {
        snprintf(text, sizeof(text), "{\"caught_up\":%u}", s->last_seq);
        if ( !send_text(c.fd, text, strlen(text)) )
        {
          drop(c.fd, c.gen);
          return false;
        }
        s->caught_up = true;
      }
      return more;
    }

    if ( !send_event(c.fd, &ev, 0) )
    {
      drop(c.fd, c.gen);
      return false;
    }
    s->last_seq = ev.seq;
  }

  return true;                                           // Batch full, come back after the others
}

/*
 * Requests that are answered once: the session list and a past
 * session.  Returns true if there is more to send.
 */
static bool serve_requests(int slot, const ws_client_t *c)
{
  ws_stream_t         *s = &streams[slot];
  shot_journal_event_t ev;
  char                 text[SHORT_TEXT];
  bool                 done = false;
  int                  n;

  if ( c->sessions_pending && !send_sessions(c->fd, c->sessions) )
  {
    drop(c->fd, c->gen);
    return false;
  }

  if ( c->history_pending )                               // Start (or restart) a replay
  {
    shot_journal_cursor_start_all(&s->hist_cursor);      // From the oldest entry still in the journal
    s->hist_active  = true;
    s->hist_session = c->history;
  }

  if ( !s->hist_active )
  {
    return false;
  }

  for ( n = 0; n != WS_BATCH * 4; n++ )                   // Skipping older sessions is cheap
  {
    if ( !shot_journal_cursor_next(&s->hist_cursor, &ev) || (ev.session > s->hist_session) )
    {
      done = true;
      break;
    }
    if ( ev.session < s->hist_session )
    {
      continue;
    }
    if ( !send_event(c->fd, &ev, s->hist_session) )
    {
      drop(c->fd, c->gen);
      return false;
    }
  }

  if ( !done )
  {
    return true;
  }

  snprintf(text, sizeof(text), "{\"history_done\":%u}", s->hist_session);
  s->hist_active = false;
  if ( !send_text(c->fd, text, strlen(text)) )
  {
    drop(c->fd, c->gen);
  }
  return false;
}

static void ws_sender(void *arg)
{
  bool busy;
  int  i;

  (void)arg;

  while ( 1 )
  {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));      // Woken by new entries or output, or once a second

    do
    {
      busy = false;
      send_console();
      for ( i = 0; i != WS_MAX_CLIENTS; i++ )
      {
        if ( serve(i) )
        {
          busy = true;
        }
      }
    } while ( busy );
  }
}

/*----------------------------------------------------------------
 *
 * @function: ws_api_register
 *
 * @brief:    Add the /ws endpoint to a web server
 *
 *--------------------------------------------------------------*/
void ws_api_register(httpd_handle_t server)
{
  static const httpd_uri_t uri = {
      .uri          = WS_URI,
      .method       = HTTP_GET,
      .handler      = ws_handler,
      .user_ctx     = NULL,
      .is_websocket = true,
  };

  if ( ws_server != NULL )                               // Only once
  {
    return;
  }

  ws_server   = server;
  client_lock = xSemaphoreCreateMutexStatic(&client_lock_buffer);
  console     = xRingbufferCreate(WS_CONSOLE_SIZE, RINGBUF_TYPE_NOSPLIT);

  if ( httpd_register_uri_handler(server, &uri) != ESP_OK )
  {
    DLT(DLT_CRITICAL, SEND(CONSOLE, sprintf(_xs, "WebSocket: %s could not be registered", WS_URI);))
    return;
  }

  xTaskCreate(ws_sender, "ws_sender", WS_STACK, NULL, WS_PRIORITY, &sender);
  shot_journal_set_notify(wake_sender);
  DLT(DLT_INFO, SEND(CONSOLE, sprintf(_xs, "Registering %s (WebSocket)", WS_URI);))
}
