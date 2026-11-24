/* Run the actual HTTP handlers with fragmented, stalled and slow request bodies. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

typedef int esp_err_t;
typedef uint32_t TickType_t;
typedef struct { size_t content_len; } httpd_req_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define HTTPD_SOCK_ERR_TIMEOUT -3
#define HTTPD_400_BAD_REQUEST 400
#define CONFIG_HTTPD_MAX_URI_LEN 512
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define DLT(level, statement)

static TickType_t ticks, receive_ms;
static int fragment, calls, commands;
static size_t position;
static const char *body;
static const char *content_type = "application/json";
static char executed[513], response[256], cache_control[32], response_type[32];
static float json_x_offset, json_y_offset;
static TickType_t xTaskGetTickCount(void) { return ticks; }
static int httpd_req_get_hdr_value_str(httpd_req_t *req, const char *key, char *out, size_t size)
{
  (void)req; (void)key;
  snprintf(out, size, "%s", content_type);
  return ESP_OK;
}
static int httpd_resp_set_status(httpd_req_t *req, const char *status) { (void)req; (void)status; return ESP_OK; }
static int httpd_resp_set_type(httpd_req_t *req, const char *type)
{ (void)req; snprintf(response_type, sizeof(response_type), "%s", type); return ESP_OK; }
static int httpd_resp_set_hdr(httpd_req_t *req, const char *key, const char *value)
{ (void)req; assert(!strcmp(key, "Cache-Control")); snprintf(cache_control, sizeof(cache_control), "%s", value); return ESP_OK; }
static int httpd_resp_sendstr(httpd_req_t *req, const char *text)
{ (void)req; snprintf(response, sizeof(response), "%s", text); return ESP_OK; }
static int httpd_resp_send_err(httpd_req_t *req, int code, const char *text)
{ (void)code; return httpd_resp_sendstr(req, text); }
static int httpd_req_recv(httpd_req_t *req, char *out, size_t size)
{
  (void)req;
  assert(++calls < 20);                     // An unbounded timeout retry fails promptly
  ticks += receive_ms;
  if (!body[position]) return HTTPD_SOCK_ERR_TIMEOUT;
  size_t n = strlen(body + position);
  if (n > size) n = size;
  if (fragment > 0 && n > (size_t)fragment) n = (size_t)fragment;
  memcpy(out, body + position, n);
  position += n;
  return (int)n;
}
static int run_json(httpd_req_t *req, char *command)
{ (void)req; commands++; snprintf(executed, sizeof(executed), "%s", command); return ESP_OK; }

#include "http_services_host.inc"

static int post(const char *input, size_t length, int chunk, TickType_t delay)
{
  body = input; position = 0; fragment = chunk; receive_ms = delay; calls = 0; commands = 0;
  httpd_req_t req = { .content_len = length };
  return service_post_json(&req);
}

int main(void)
{
  const char *command = "{\"X_OFFSET\":1.25}";
  assert(post(command, strlen(command), 2, 10) == ESP_OK);
  assert(commands == 1 && !strcmp(executed, command));
  assert(post("", strlen(command), 2, 5000) == ESP_FAIL);
  assert(calls == 1 && commands == 0);
  assert(post("{\"", strlen(command), 2, 100) == ESP_FAIL);
  assert(calls == 2 && commands == 0);
  assert(post(command, strlen(command), 1, 3000) == ESP_FAIL);
  assert(calls == 2 && commands == 0);        // Progress cannot evade the overall deadline
  ticks = UINT32_MAX - 5;
  assert(post(command, strlen(command), 2, 10) == ESP_OK && commands == 1);
  assert(post(command, 0, 2, 10) == ESP_OK && commands == 0);
  assert(post(command, 513, 2, 10) == ESP_OK && commands == 0);
  content_type = "text/plain";
  assert(post(command, strlen(command), 2, 10) == ESP_OK && commands == 0);
  content_type = "application/json";
  assert(post(command, strlen(command), 2, 10) == ESP_OK && commands == 1);

  httpd_req_t req = {0};
  json_x_offset = 1.25f; json_y_offset = -0.5f;
  assert(service_get_calibration(&req) == ESP_OK);
  assert(!strcmp(response, "{\"x_offset\":1.25,\"y_offset\":-0.50}"));
  json_x_offset = 2.5f;
  assert(service_get_calibration(&req) == ESP_OK);
  assert(strstr(response, "2.50"));            // Always reads current offsets
  assert(!strcmp(cache_control, "no-store") && !strcmp(response_type, "application/json"));
  puts("HTTP: fragmented bodies, receive timeouts, total deadline, tick wrap and fresh calibration passed");
  return 0;
}
