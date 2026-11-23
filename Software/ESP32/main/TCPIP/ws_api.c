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
#define WS_SHOT_FIELDS  "SNQM?TUXP"    // Shot, session, seq, miss, session type, time, utc, x/y, polar

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

static void remove_client(int fd, unsigned int gen) // gen 0 = any client on this socket
{
  int i;

  xSemaphoreTake(client_lock, portMAX_DELAY);
  for ( i = 0; i != WS_MAX_CLIENTS; i++ )
  {
    if ( clients[i].used && (clients[i].fd == fd) && ((gen == 0) || (clients[i].gen == gen)) )
    {
      clients[i].used = false;
      client_count--;
    }
  }
  xSemaphoreGive(client_lock);
}

void ws_api_closed(int fd)
{
  if ( client_lock != NULL )
  {
    remove_client(fd, 0);
  }
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
  const char      *after;
  int              fd = httpd_req_to_sockfd(req);
  int              i;
  long             n;

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

  after = strstr(text, "\"AFTER\"");
  if ( after != NULL )                                   // {"AFTER": n}
  {
    after += strlen("\"AFTER\"");
    while ( (*after == ' ') || (*after == ':') )
    {
      after++;
    }
    n = strtol(after, NULL, 10);

    xSemaphoreTake(client_lock, portMAX_DELAY);
    for ( i = 0; i != WS_MAX_CLIENTS; i++ )
    {
      if ( clients[i].used && (clients[i].fd == fd) )
      {
        clients[i].after         = (n > 0) ? (unsigned int)n : 0;
        clients[i].after_pending = true;
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
  DLT(DLT_COMMUNICATION, SEND(CONSOLE, sprintf(_xs, "WebSocket client %d dropped", fd);))
  remove_client(fd, gen);
  httpd_sess_trigger_close(ws_server, fd);
}

static bool send_hello(int fd)
{
  char     text[SHORT_TEXT * 3];
  char     name[SHORT_TEXT];
  uint8_t  source;
  uint32_t now = wall_clock_now(&source);

  target_name(name);
  snprintf(text, sizeof(text), "{\"hello\":\"freETarget\", \"version\":%s, \"name\":\"%.60s\", \"session\":%u, \"next_seq\":%u, "
                               "\"utc\":%lu, \"clock\":\"%s\"}",
           SOFTWARE_VERSION, name, shot_journal_session(), shot_journal_next_seq(),
           (source == JRNL_SOURCE_NONE) ? 0ul : (unsigned long)now, wall_clock_source_name(source));
  return send_text(fd, text, strlen(text));
}

static bool send_event(int fd, const shot_journal_event_t *ev)
{
  char   text[SCORE_TEXT];
  size_t used;

  if ( ev->kind == JRNL_SHOT )
  {
    text[0] = '{';
    build_json_score((shot_record_t *)&ev->shot, WS_SHOT_FIELDS, text + 1, sizeof(text) - 2);
    used = strlen(text);
    text[used]     = '}';
    text[used + 1] = 0;
  }
  else
  {
    snprintf(text, sizeof(text), "{\"session\":%u, \"session_type\":%u, \"seq\":%u, \"change\":\"%s\"}", ev->session,
             ev->session_type, ev->seq, (ev->kind == JRNL_SESSION) ? "new" : "mode");
  }

  return send_text(fd, text, strlen(text));
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

  xSemaphoreTake(client_lock, portMAX_DELAY);
  c                            = clients[slot];
  clients[slot].hello_pending  = false;
  clients[slot].after_pending  = false;
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

  if ( !s->subscribed )
  {
    return false;
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
      return false;
    }

    if ( !send_event(c.fd, &ev) )
    {
      drop(c.fd, c.gen);
      return false;
    }
    s->last_seq = ev.seq;
  }

  return true;                                           // Batch full, come back after the others
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
