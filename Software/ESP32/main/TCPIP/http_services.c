/*-------------------------------------------------------
 *
 * http_server.c
 *
 * General purpose file to perform HTTP web server
 * functions
 *
 *------------------------------------------------------
 *
 * This is a generic HTTP web server
 *
 * The softwareis based on
 * https://github.com/espressif/esp-idf/blob/8760e6d2a7e19913bc40675dd71f374bcd51b0ae/examples/protocols/http_server/simple/main/main.c
 *
 *
 * See http_server.h for the various compilation options
 *
 *-----------------------------------------------------*/
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/param.h>
#include "esp_http_server.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_tls.h"
#include <esp_wifi.h>
#include "esp_ota_ops.h"

#include "freETarget.h"
#include "compute_hit.h"
#include "helpers.h"
#include "http_server.h"
#include "http_services.h"
#include "diag_tools.h"
#include "json.h"
#include "serial_io.h"

#include "html.h"
#include "ws_api.h"

#define EXAMPLE_HTTP_QUERY_KEY_MAX_LEN (64)

/*
 *  Variables
 */
static int check_mask = 0;             // Mask of the checkboxes

typedef struct
{
  unsigned int port;
} my_user_ctx_t;                       // Internal user context structure

static const my_user_ctx_t event_menu_ctx  = {EVENT_HTTP_PORT};

/*
 * Local functions
 */
static esp_err_t stop_webserver(httpd_handle_t server);
static esp_err_t service_get_help(httpd_req_t *req);                                   // User help page
static esp_err_t service_get_menu(httpd_req_t *req);                                   // Control back channel
static esp_err_t service_get_who(httpd_req_t *req);                                    // Target information page
static esp_err_t service_get_FreeETarget_png(httpd_req_t *req);                        // Icon for the FreeETarget page
static esp_err_t service_get_json(httpd_req_t *req);                                   // Read-only JSON commands (GET)
static esp_err_t service_post_json(httpd_req_t *req);                                  // JSON commands (POST)
static esp_err_t service_get_calibration(httpd_req_t *req);                            // Current offsets, without a full ECHO
static esp_err_t service_get_live(httpd_req_t *req);                                   // WebSocket test page
static esp_err_t service_get_app(httpd_req_t *req);                                    // Athlete page
static esp_err_t http_404_error_handler(httpd_req_t *req, httpd_err_code_t err);       // Create a URL not found handler
static esp_err_t service_post_post(httpd_req_t *req);
static void      http_printf(const char *format, httpd_req_t *req, unsigned int size); // Formatted print to the HTTP request

/*
 *  URI handlers
 */
#define URI(path, function, context) {.uri = (path), .method = HTTP_GET, .handler = (function), .user_ctx = (context)}

const my_uri_t uri_list[] = {
    {DEFAULT_HTTP_PORT, "Display target",      URI("/", service_get_app, NULL)                             }, // Main target page

    {DEFAULT_HTTP_PORT, "Help menu",           URI("/help", service_get_help, NULL)                        },
    {DEFAULT_HTTP_PORT, "Target info",         URI("/who", service_get_who, NULL)                          },
    {DEFAULT_HTTP_PORT, "JSON status: ECHO, VERSION, MFS? (POST other commands)", URI("/json", service_get_json, NULL) },
    {DEFAULT_HTTP_PORT, "",                    {.uri = "/json", .method = HTTP_POST, .handler = service_post_json, .user_ctx = NULL} },
    {DEFAULT_HTTP_PORT, "",                    URI("/calibration", service_get_calibration, NULL)         },
    {DEFAULT_HTTP_PORT, "",                    URI("/favicon.ico", service_get_FreeETarget_png, NULL)      },

    {EVENT_HTTP_PORT,   "Target control menu", URI("/", service_get_menu, (void *)&event_menu_ctx)         }, // Control back channel
    {EVENT_HTTP_PORT,   "Target control menu", URI("/menu", service_get_menu, (void *)&event_menu_ctx)     },
    {EVENT_HTTP_PORT,   "Duplicate Help menu", URI("/help", service_get_help, NULL)                        },
    {EVENT_HTTP_PORT,   "Live shots (WebSocket)", URI("/live", service_get_live, NULL)                    },
    {EVENT_HTTP_PORT,   "",                    URI("/favicon.ico", service_get_FreeETarget_png, NULL)      },
    {0,                 "",                    {.uri = ""}                                                  }
};

/*
 *  Definitions
 */
#define CHECK_A 0x01 // Bit mask for Air Pistol
#define CHECK_B 0x02 // Bit mask for Air Rifle
#define CHECK_C 0x04 // Bit mask for .22 Pistol
#define CHECK_D 0x08 // Bit mask for 50 M Rifle
#define CHECK_E 0x10 // Bit mask for SIGHT
#define CHECK_F 0x20 // Bit mask for Match
/*----------------------------------------------------------------
 *
 * @function: register_services
 *
 * @brief:    Register the services in this file
 *
 * @return:   None
 *
 *---------------------------------------------------------------
 *
 * Read the nonvol into RAM.
 *
 * If the results is uninitalized then force the factory default.
 * Then check for out of bounds and reset those values
 *
 *------------------------------------------------------------*/
void register_services(httpd_handle_t server, // Pointer to active server
                       unsigned int   port)
{
  int i;

  i = 0;
  while ( uri_list[i].port != 0 )
  {
    if ( uri_list[i].port == port ) // Only register the services for this port
    {
      httpd_register_uri_handler(server, &uri_list[i].uri_struct);
      DLT(DLT_INFO, SEND(CONSOLE, sprintf(_xs, "Registering %s on port %d", uri_list[i].uri_struct.uri, uri_list[i].port);))
    }

    i++;
  }

  /*
   * The WebSocket API goes on the control port, so that a /json
   * request on port 80 (which waits for the command to finish) never
   * holds up the shots.
   */
  if ( port == EVENT_HTTP_PORT )
  {
    ws_api_register(server);        // WebSocket API on /ws
  }
  return;
}

/*----------------------------------------------------------------
 *
 * @function: service_get_app / service_get_live
 *
 * @brief:    The athlete page (/) and the small WebSocket test page
 *            (:8080/live).  Both get their shots over /ws.
 *
 *--------------------------------------------------------------*/
static esp_err_t service_get_app(httpd_req_t *req)
{
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, (const char *)app_html_start, SIZEOF_app_HTML);
}

static esp_err_t service_get_live(httpd_req_t *req)
{
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, (const char *)live_html_start, SIZEOF_live_HTML);
}

/*----------------------------------------------------------------
 *
 * @function: service_get_menu
 *
 * @brief:    Control the target over port 81
 *
 * @return:   esp_err_t, error type
 *
 *---------------------------------------------------------------
 *
 * This function is called in respoinse to a user starting a
 * shooting session.a
 *
 * Arguements
 *
 * MATCH    - Start a match session
 * SIGHT - Start a SIGHT session
 *
 * Air Pistol   - Air Pistol
 * air Rifle  - Air Rifle
 * 50m Pistol - .22 Pistol
 * 50m Rifle - 50 M Rifle
 *
 *  110   ISSF 10 Metre Air Rifle
 *  111   ISSF 10 Metre Air Rifle SIGHT
 *  100   ISSF 10 Metre Air Pistol Match
 *  101   ISSF 10 Metre Air Pistol SIGHT
 *  510   ISSF 50 Metre Rifle Match
 *  511   ISSF 50 Metre Rifle SIGHT
 *  500   ISSF 50 Metre .22 Pistol Match
 *  501   ISSF 50 Metre .22 Pistol SIGHT
 *
 *------------------------------------------------------------*/

#define CHECK_A     0x01 // Bit mask for Air Pistol
#define CHECK_B     0x02 // Bit mask for Air Rifle
#define CHECK_C     0x04 // Bit mask for .22 Pistol
#define CHECK_D     0x08 // Bit mask for 50 M Rifle
#define CHECK_E     0x10 // Bit mask for SIGHT
#define CHECK_F     0x20 // Bit mask for Match
#define START_EVENT 0x01 // Start the event
#define STOP_EVENT  0x02 // Stop the event
#define SIGHT       1
#define MATCH       0
#define AIR_PISTOL  100
#define AIR_RIFLE   110
#define PISTOL_50M  500
#define RIFLE_50M   510

typedef struct
{
  char *uri;          // URI to handle
  int   start_stop;   // Start or stop the ev
  int   check_mask;   // Mask of the checkboxes
  int   session_type; // Session type (110, 111, 100, 101, 510, 511, 500, 501)
} menu_action_t;      // Internal user context structure

menu_action_t menu_actions[] = {
    // uri         start_stop, check_mask, session_type
    {"START",      START_EVENT, CHECK_E, SIGHT     }, // Start a SIGHT session
    {"STOP",       STOP_EVENT,  0,       0         }, // Stop the SIGHT session
    {"SIGHT",      START_EVENT, CHECK_E, SIGHT     }, // Sighters
    {"MATCH",      START_EVENT, CHECK_F, MATCH     }, // Match
    {"Air+Pistol", START_EVENT, CHECK_A, AIR_PISTOL}, // Air Pistol
    {"Air+Rifle",  START_EVENT, CHECK_B, AIR_RIFLE }, // Air Rifle
    {"50m+Pistol", START_EVENT, CHECK_C, PISTOL_50M}, // 50 M Pistol
    {"50m+Rifle",  START_EVENT, CHECK_D, RIFLE_50M }, // 50 M Rifle
    {"",           0,           0,       0         }  // End of list marker
};

static esp_err_t service_get_menu(httpd_req_t *req)
{
  char my_name[SHORT_TEXT];                           // Temporary string
  int  session_type;                                  // Index into the session_type array
  int  i;                                             // Loop index

  DLT(DLT_HTTP, SEND(CONSOLE, sprintf(_xs, "service_get_menu(%s)", req->uri);))

                                                      /*
                                                       *  Decode the command line arguements if there are any
                                                       */
  i            = 0;
  check_mask   = 0;
  session_type = 0;                                 // Default to no session type

  while ( menu_actions[i].uri[0] != 0 )
  {
    if ( contains(req->uri, menu_actions[i].uri) )  // If the URI contains the action
    {

      check_mask |= menu_actions[i].check_mask;     // Set the check mask
      session_type += menu_actions[i].session_type; // Set the session type

      if ( menu_actions[i].start_stop == START_EVENT )
      {
        start_new_session(check_mask & SIGHT);      // Start a new session
      }

      DLT(DLT_HTTP, SEND(CONSOLE, sprintf(_xs, "Setting check_mask: %d", check_mask);))
    }
    i++;
  }
  if ( check_mask == 0 )
  {
    check_mask = CHECK_A | CHECK_E; // Default to air pistol and SIGHT
  }

  /*
   *  Send the reply to the client
   */
  target_name(my_name);                                 // Get the target name
  httpd_resp_set_hdr(req, "get_menu", my_name);
  http_printf(&menu_html_start, req, SIZEOF_menu_HTML); // point to the target HTML file

  return ESP_OK;
}

/*----------------------------------------------------------------
 *
 * @function: service_get_help
 *
 * @brief:    Display the help page
 *
 * @return:   esp_err_t, error type
 *
 *---------------------------------------------------------------
 *
 * Displays the help page to the user.
 *
 *------------------------------------------------------------*/
static esp_err_t service_get_help(httpd_req_t *req)
{
  const char *resp_str;                     // Reply to server
  char        my_name[SHORT_TEXT];          // Temporary string
  char        line[MEDIUM_TEXT + SHORT_TEXT]; // One line of the reply (not _xs, the send may block)
  int         i, j;

  DLT(DLT_HTTP, SEND(CONSOLE, sprintf(_xs, "service_get_help(%s)", req->uri);))

  /*
   *  Send the reply to the client
   */
  target_name(my_name);                      // Get the target name
  resp_str = (const char *)&help_html_start; // point to the target HTML file
  httpd_resp_set_hdr(req, "get_help", my_name);
  DLT(DLT_HTTP, SEND(CONSOLE, sprintf(_xs, "reply(%s)", my_name);))
  httpd_resp_send_chunk(req, resp_str, SIZEOF_help_HTML);

  /*
   *  Send the installed events
   */
  snprintf(line, sizeof(line), "<br><p class=MsoNormal><b><span style='font-size:14.0pt;line-height:115%%'>INSTALLED SERVICES</span></b></p>");
  httpd_resp_send_chunk(req, line, strlen(line));

  i = 0;
  while ( uri_list[i].port != 0 )
  {
    if ( uri_list[i].help[0] != 0 )
    {
      if ( uri_list[i].port == DEFAULT_HTTP_PORT )
      {
        snprintf(line, sizeof(line), "<br>%s.local%s &nbsp;&nbsp;&nbsp;&nbsp;%s", my_name, uri_list[i].uri_struct.uri,
                 uri_list[i].help);
      }
      else
      {
        snprintf(line, sizeof(line), "<br>%s.local:%d%s. &nbsp;&nbsp;&nbsp;&nbsp;%s", my_name, uri_list[i].port,
                 uri_list[i].uri_struct.uri, uri_list[i].help);
      }

      httpd_resp_send_chunk(req, line, strlen(line));
    }
    i++;
  }

  /*
   *  All done
   */
  httpd_resp_send_chunk(req, NULL, 0); // End the response
  return ESP_OK;
}

/*----------------------------------------------------------------
 *
 * @function: service_get_json, service_post_json
 *
 * @brief:    Run a JSON command and return its output
 *
 * @return:   esp_err_t, error type
 *
 *---------------------------------------------------------------
 *
 * Commands that change the target are a POST with the command as an
 * application/json body:
 *
 *   POST /json   {"X_OFFSET":0.5, "Y_OFFSET":-1}
 *
 * A web page on another site cannot send that without the target's
 * permission, which it never gives.  GET is kept for typing into a
 * browser, but only for the commands that just report:
 *
 *   /json?{"ECHO":0}      /json?{"VERSION":0}      /json?{"MFS?":0}
 *
 * The command goes into the TCPIP queue and the JSON task runs it.  Its
 * output comes back to this request through http_send_string().
 *
 *------------------------------------------------------------*/
#define JSON_WAIT_MS  5000                          // Longest a /json request waits for its command
#define JSON_OUTPUT_MS 15000                        // Longest it collects the output ({"ECHO":0} takes about 6 s)
#define JSON_POST_MAX CONFIG_HTTPD_MAX_URI_LEN      // Longest command (the same as a GET can carry)
#define JSON_BODY_MS  5000                          // Total receive budget, including clients sending very slowly

static esp_err_t service_get_calibration(httpd_req_t *req)
{
  char text[128];

  snprintf(text, sizeof(text), "{\"x_offset\":%.2f,\"y_offset\":%.2f}", (double)json_x_offset, (double)json_y_offset);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_sendstr(req, text);
}

static bool json_busy(void)                         // A command is queued or running
{
  return (serial_available(TCPIP) != 0) || ((run_state & IN_HTTP) != 0);
}

static esp_err_t run_json(httpd_req_t *req, char *command)
{
  char my_name[SHORT_TEXT];                         // Target name
  int  waited;                                      // ms waited

  /*
   * Let the previous command finish first, or the rest of its output
   * would come back in this reply
   */
  for ( waited = 0; (waited < JSON_WAIT_MS) && json_busy(); waited += 10 )
  {
    vTaskDelay(TICK_10ms);
  }
  if ( json_busy() )
  {
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_sendstr(req, "The target is busy with another command; try again.");
    return ESP_OK;
  }

  tcpip_socket_2_queue(command, strlen(command));   // Put the data into the TCPIP queue

  /*
   * Set the header to indicate that this is a json request, then wait till it's done
   */
  target_name(&my_name);                            // Get the target name
  httpd_resp_set_hdr(req, "get_json", my_name);
  http_send_string_start(req);                      // Start sending a string to the client

  /*
   * Wait until the JSON task has read the command and finished with it.
   */
  for ( waited = 0; waited < JSON_OUTPUT_MS; waited += 10 )
  {
    vTaskDelay(TICK_10ms);
    if ( (waited >= 50) && !json_busy() )
    {
      break;                                        // Read and done
    }
  }

  http_send_string_end(req);                        // Stop sending a string to the client

  return ESP_OK;
}

static esp_err_t service_get_json(httpd_req_t *req)
{
  char argument[CONFIG_HTTPD_MAX_URI_LEN + 1];      // JSON command taken from the URI (never longer than the URI)

  DLT(DLT_HTTP, SEND(CONSOLE, sprintf(_xs, "service_get_json(%s)", req->uri);))
  squish(req->uri, argument);                       // Go through the uri and keep the argument portion

  if ( !json_read_only(argument) )
  {
    httpd_resp_set_status(req, "405 Method Not Allowed");
    httpd_resp_set_hdr(req, "Allow", "POST");
    httpd_resp_sendstr(req, "Only ECHO, VERSION and MFS? can be a GET. Send other commands as a POST "
                            "with Content-Type: application/json.");
    return ESP_OK;
  }

  return run_json(req, argument);
}

static esp_err_t service_post_json(httpd_req_t *req)
{
  char command[JSON_POST_MAX + 1];                  // The JSON command from the body
  char type[48];                                    // Content-Type header
  int  got;                                         // Bytes received so far
  int  n;
  TickType_t started = xTaskGetTickCount();

  DLT(DLT_HTTP, SEND(CONSOLE, sprintf(_xs, "service_post_json(%d bytes)", (int)req->content_len);))

  if ( (httpd_req_get_hdr_value_str(req, "Content-Type", type, sizeof(type)) != ESP_OK)
       || (strncasecmp(type, "application/json", 16) != 0) )
  {
    httpd_resp_set_status(req, "415 Unsupported Media Type");
    httpd_resp_sendstr(req, "Send the command with Content-Type: application/json.");
    return ESP_OK;
  }
  if ( (req->content_len == 0) || (req->content_len > JSON_POST_MAX) )
  {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "The command is empty or too long.");
    return ESP_OK;
  }

  for ( got = 0; got < (int)req->content_len; got += n )
  {
    if ( (TickType_t)(xTaskGetTickCount() - started) >= pdMS_TO_TICKS(JSON_BODY_MS) )
    {
      return ESP_FAIL;                              // Close incomplete requests instead of blocking port 80
    }
    n = httpd_req_recv(req, command + got, req->content_len - got);
    if ( n <= 0 )
    {
      return ESP_FAIL;                              // Includes receive timeout: close the socket
    }
  }
  command[got] = 0;

  return run_json(req, command);
}

/*----------------------------------------------------------------
 *
 * @function: service_get_FreeETarget_png
 *
 * @brief:    Send the FreeETarget icon file to the client
 *
 * @return:   esp_err_t, error type
 *
 *---------------------------------------------------------------
 *
 *
 *------------------------------------------------------------*/
static esp_err_t service_get_FreeETarget_png(httpd_req_t *req)
{
  const char *resp_str;                           // Reply to server
  char        my_name[SHORT_TEXT];                // Target name

  DLT(DLT_HTTP, SEND(CONSOLE, sprintf(_xs, "service_get_FreeETargetIcon_png(%s)", req->uri);))

  target_name(my_name);
  resp_str = (const char *)FreeETarget_png_start; // point to the target json file
  httpd_resp_set_hdr(req, "get_FreeETargetIcon_png", my_name);
  httpd_resp_send(req, resp_str, SIZEOF_FREEETARGET_PNG);

  return ESP_OK;
}
/*----------------------------------------------------------------
 *
 * @function: sample_post_post_handler
 *
 * @brief:    Entry point to handle a POST request
 *
 * @return:   esp_err_t, error type
 *
 *---------------------------------------------------------------
 *
 * Test POST hanbdler.  This uploads the POST page
 *
 *------------------------------------------------------------*/
static esp_err_t service_post_post(httpd_req_t *req)
{
  int  ret;                     // Number of bytes remaining to be proceesed
  int  remaining;               // Bytes remaining to be processed
  char buffer[MEDIUM_TEXT];     // Received data (not _xs, the receive may block)

  remaining = req->content_len; // Find out how long the transfer is

                                /*
                                 *  Loop and find out how long the transfer is
                                 */
  while ( remaining > 0 )
  {
    if ( (ret = httpd_req_recv(req, buffer, MIN(remaining, sizeof(buffer)))) <= 0 )
    {
      if ( ret == HTTPD_SOCK_ERR_TIMEOUT )
      {
        /* Retry receiving if timeout occurred */
        continue;
      }
      return ESP_FAIL; // Ran out of time waiting
    }

                       /*
                        *  The buffer contains (at most) sizeof(buffer) bytes
                        *  Do womething with it
                        */

    printf("\r\n=========== RECEIVED DATA ==========\r\n");
    printf("%.*s", ret, buffer);
    remaining -= ret;
    printf("\r\n====================================\r\n");
  }

  /*
   *  All done, end the respone and return
   */
  httpd_resp_send_chunk(req, NULL, 0);
  return ESP_OK;
}

#if ( 0 )
/*----------------------------------------------------------------
 *
 * @function: sample_post_get_handler
 *
 * @brief:    Use a GET to post a POST page
 *
 * @return:   esp_err_t, error type
 *
 *---------------------------------------------------------------
 *
 * Read the nonvol into RAM.
 *
 * If the results is uninitalized then force the factory default.
 * Then check for out of bounds and reset those values
 *
 *------------------------------------------------------------*/
static esp_err_t service_get_post2(httpd_req_t *req)
{
  const char *resp_str;                     // Reply to server
  char        name[SHORT_TEXT];

  target_name(name);                        // Get the target name
  resp_str = (const char *)&post_test_html; // point to the target HTML file
  httpd_resp_set_hdr(req, "FreeETarget", name);
  httpd_resp_send(req, resp_str, HTTPD_RESP_USE_STRLEN);

  return ESP_OK;
}
#endif

/*----------------------------------------------------------------
 *
 * @function: service_get_who
 *
 * @brief:    Display the target information
 *
 * @return:   esp_err_t, error type
 *
 *---------------------------------------------------------------
 *
 * Return the target name and other information
 *
 *------------------------------------------------------------*/
static esp_err_t service_get_who(httpd_req_t *req)
{
  char                   my_name[SHORT_TEXT];
  char                   reply[SCORE_TEXT];  // Reply to the client (not _xs, the send may block)
  const esp_partition_t *running_partition = esp_ota_get_running_partition();
  esp_app_desc_t         running_app_info;
  static int             cycle_count = 0;

  DLT(DLT_HTTP, SEND(CONSOLE, sprintf(_xs, "service_get_who(%s)", req->uri);))

  esp_ota_get_partition_description(running_partition, &running_app_info);

  target_name(my_name); // Get the target name
  httpd_resp_set_hdr(req, "get_who", my_name);

  snprintf(reply, sizeof(reply),
          "Serial Number: %d"
          "<br>Target ID: %s"
          "<br>Version: %s"
          "<br>OTA Version: %s"
          "<br>Athelete: %s"
          "<br>Target: %s"
          "<br>Event: %s"
          "<br>Cycle count: %d",
          json_serial_number, my_name, SOFTWARE_VERSION, running_app_info.version, json_athlete, json_target_name, json_event,
          cycle_count); // Fill in the target name

  httpd_resp_send(req, reply, HTTPD_RESP_USE_STRLEN);
  cycle_count++;
  return ESP_OK;
}
/*----------------------------------------------------------------
 *
 * @function: http_printf
 *
 * @brief:    Local version of printf to send data to the client
 *
 * @return:   None
 *
 *---------------------------------------------------------------
 *
 * Kluged up 'printf' to send data to an HTTP client. This allows
 * the programmer to incorporate variable data in the reply to
 * the client.
 *
 * The format string is a text string that contains formatting
 * fields that are replaced with the data.
 *
 * For example: %A will be replaced with the string "SELECTED"
 * if the radio button has been SELECTED.
 *
 *------------------------------------------------------------*/
#define SELECTED                                                                                                                           \
  strcat(buffer, " selected ");                                                                                                            \
  i = strlen(buffer);

#define HTTP_PRINTF_SIZE   512 // Size of the http_printf output buffer
#define HTTP_PRINTF_MARGIN 32  // Flush when this close to full (one step adds at most strlen(" selected ") bytes)

static void http_printf(const char  *format, // Format string
                        httpd_req_t *req,    // HTTP request to return to
                        unsigned int size    // Size of the format string
)
{
  int          i;
  unsigned int remainder;                    // Number of bytes remaining to be processed
  char         buffer[HTTP_PRINTF_SIZE];     // Output buffer (not _xs, the send may block)

  buffer[0] = 0;
  i         = 0;
  remainder = size;
  /*
   *  Loop and output the format string
   */

  while ( remainder != 0 )
  {
    if ( *format == '^' )             // Look for a format specifier
    {
      format++;                       // Skip the %
      remainder--;                    // Decrement the number of bytes remaining

      switch ( *format )
      {
        case '^':
          strcat(buffer, "^");        // If we have a double ^ then just output a single ^
          i = strlen(buffer);
          break;

        case 'A':
          if ( CHECK_A & check_mask ) // If the Air Pistol button is SELECTED
          {
            SELECTED
          }

          break;

        case 'B':
          if ( CHECK_B & check_mask ) // If the Air Pistol button is SELECTED
          {
            SELECTED
          }
          break;

        case 'C':
          if ( CHECK_C & check_mask ) // If the Air Pistol button is SELECTED
          {
            SELECTED
          }
          break;

        case 'D':
          if ( CHECK_D & check_mask ) // If the Air Pistol button is SELECTED
          {
            SELECTED
          }
          break;

        case 'E':
          if ( CHECK_E & check_mask ) // If the Air Pistol button is SELECTED
          {
            SELECTED
          }
          break;

        case 'F':
          if ( CHECK_F & check_mask ) // If the Air Pistol button is SELECTED
          {
            SELECTED
          }
          break;

        default:                      // Unknown format
          break;
      }
      format++;
      remainder--;                    // Decrement the number of bytes remaining
    }
    else
    {
      buffer[i++] = *format++;        // Copy the character to the buffer
      remainder--;                    // Decrement the number of bytes remaining
    }
    buffer[i] = 0;                    // Null terminate the string

    /*
     *  See if it is time to send out the buffer.
     */
    if ( i >= (int)(sizeof(buffer) - HTTP_PRINTF_MARGIN) ) // If we have almost filled the buffer?
    {
      printf("%s", buffer);       // Print the buffer to the console
      httpd_resp_send_chunk(req, buffer, i);
      i         = 0;              // Reset the index
      buffer[0] = 0;              // Null terminate the string
    }
  }

  /*
   *  Finished Send out the last
   */
  if ( i != 0 )                         // If we have something to send
  {
    buffer[i] = 0;                         // Null terminate the string
    httpd_resp_send_chunk(req, buffer, i); // Send the string to the client
  }
  httpd_resp_send_chunk(req, NULL, 0);  // End the response
  return;
}
