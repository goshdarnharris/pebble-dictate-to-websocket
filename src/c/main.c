#include <pebble.h>

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define PROTOCOL_VERSION_VALUE 2
#define ERROR_DISPLAY_MS 5000
#define REQUEST_ID_LENGTH 8
#define SERVER_RESPONSE_MAX_BYTES 1024

typedef enum {
  MESSAGE_TYPE_SESSION_BEGIN = 1,
  MESSAGE_TYPE_TRANSCRIPT_CHUNK = 2,
  MESSAGE_TYPE_SESSION_END = 3,
  MESSAGE_TYPE_SEND_STARTED = 10,
  MESSAGE_TYPE_FAILURE = 12,
  MESSAGE_TYPE_SERVER_RESULT_CHUNK = 13,
} MessageType;

typedef enum {
  STATUS_NORMAL = 0,
  STATUS_CANCELLED = 1,
  STATUS_ERROR_DICTATION = 2,
  STATUS_ERROR_NO_SPEECH = 3,
  STATUS_ERROR_PHONE_CONNECTIVITY = 4,
  STATUS_ERROR_PROTOCOL = 5,
  STATUS_ERROR_TRANSFER = 6,
  STATUS_ERROR_SERVER = 7,
  STATUS_ERROR_SERVER_RESULT_TIMEOUT = 8,
  STATUS_ERROR_TRANSCRIPT_DELIVERY_TIMEOUT = 9,
  STATUS_ERROR_WS_REQUEST_TIMEOUT = 10,
  STATUS_ERROR_RESULT_TRANSFER_TIMEOUT = 11,
} AppStatusCode;

typedef enum {
  APP_STATE_INITIALIZING,
  APP_STATE_DICTATING,
  APP_STATE_BRIDGING,
  APP_STATE_DISPLAYING_RESULT,
  APP_STATE_ERROR,
} AppState;

typedef enum {
  WATCH_PHONE_BRIDGE_STATE_IDLE,
  WATCH_PHONE_BRIDGE_STATE_SESSION_BEGIN_PENDING,
  WATCH_PHONE_BRIDGE_STATE_READY,
  WATCH_PHONE_BRIDGE_STATE_TRANSFERRING,
  WATCH_PHONE_BRIDGE_STATE_AWAITING_SEND_START,
  WATCH_PHONE_BRIDGE_STATE_AWAITING_RESULT,
  WATCH_PHONE_BRIDGE_STATE_RECEIVING_RESULT,
  WATCH_PHONE_BRIDGE_STATE_COMPLETE,
  WATCH_PHONE_BRIDGE_STATE_FAILED,
} WatchPhoneBridgeState;

typedef enum {
  WATCH_PHONE_BRIDGE_ERROR_NONE,
  WATCH_PHONE_BRIDGE_ERROR_CONNECTIVITY,
  WATCH_PHONE_BRIDGE_ERROR_PROTOCOL,
  WATCH_PHONE_BRIDGE_ERROR_TRANSFER,
  WATCH_PHONE_BRIDGE_ERROR_SEND_START_TIMEOUT,
  WATCH_PHONE_BRIDGE_ERROR_RESULT_TRANSFER_TIMEOUT,
} WatchPhoneBridgeError;

typedef enum {
  PHONE_SERVER_BRIDGE_STATE_IDLE,
  PHONE_SERVER_BRIDGE_STATE_PREPARING,
  PHONE_SERVER_BRIDGE_STATE_AWAITING_RESULT,
  PHONE_SERVER_BRIDGE_STATE_COMPLETE,
  PHONE_SERVER_BRIDGE_STATE_FAILED,
} PhoneServerBridgeState;

typedef enum {
  PHONE_SERVER_BRIDGE_ERROR_NONE,
  PHONE_SERVER_BRIDGE_ERROR_PROTOCOL,
  PHONE_SERVER_BRIDGE_ERROR_SERVER,
  PHONE_SERVER_BRIDGE_ERROR_RESULT_TIMEOUT,
  PHONE_SERVER_BRIDGE_ERROR_REQUEST_TIMEOUT,
} PhoneServerBridgeError;

typedef enum {
  OUTBOX_NONE,
  OUTBOX_SESSION_BEGIN,
  OUTBOX_TRANSCRIPT_CHUNK,
  OUTBOX_SESSION_END,
} OutboxMessage;

typedef struct {
  uint32_t duration_ms;
  AppTimer *timer;
} BridgeTimeout;

typedef struct {
  WatchPhoneBridgeState state;
  WatchPhoneBridgeError error;
  BridgeTimeout send_start_timeout;
  BridgeTimeout result_chunk_timeout;
  OutboxMessage outbox_message;
  char *server_response;
  size_t server_response_bytes;
  uint32_t response_chunk_count;
  uint32_t next_response_chunk;
  uint32_t response_total_bytes;
  bool server_success;
  bool app_message_ready;
  bool begin_delivered;
  bool session_end_pending;
} WatchPhoneBridgeContext;

typedef struct {
  PhoneServerBridgeState state;
  PhoneServerBridgeError error;
  BridgeTimeout request_timeout;
} PhoneServerBridgeContext;

typedef struct {
  WatchPhoneBridgeContext watch_phone;
  PhoneServerBridgeContext phone_server;
} BridgeContext;

typedef struct {
  Window *window;
  TextLayer *status_layer;
  TextLayer *result_heading_layer;
  TextLayer *result_text_layer;
  ScrollLayer *result_scroll_layer;
  DictationSession *dictation;
  AppTimer *exit_timer;
  BridgeContext bridge;
  char *transcript;
  size_t transcript_bytes;
  size_t chunk_capacity;
  size_t next_chunk_offset;
  size_t outbox_chunk_bytes;
  uint32_t chunk_count;
  uint32_t next_chunk_index;
  uint32_t outbox_size;
  char request_id[REQUEST_ID_LENGTH + 1];
  AppState state;
  AppStatusCode terminal_status;
  bool dictation_active;
  bool initialization_failed;
  bool shutting_down;
} AppContext;

static AppContext s_app;

static void prv_fail(AppStatusCode status);
static void prv_send_next_chunk(void);
static bool prv_tuple_to_uint32(const Tuple *tuple, uint32_t *value);

static void prv_bridge_init(BridgeContext *bridge) {
  bridge->watch_phone.state = WATCH_PHONE_BRIDGE_STATE_IDLE;
  bridge->watch_phone.error = WATCH_PHONE_BRIDGE_ERROR_NONE;
  bridge->watch_phone.send_start_timeout.duration_ms = 2000;
  bridge->watch_phone.result_chunk_timeout.duration_ms = 3000;
  bridge->phone_server.state = PHONE_SERVER_BRIDGE_STATE_IDLE;
  bridge->phone_server.error = PHONE_SERVER_BRIDGE_ERROR_NONE;
  bridge->phone_server.request_timeout.duration_ms = 62000;
}

static void prv_cancel_bridge_timeout(BridgeTimeout *timeout) {
  if (timeout->timer) {
    app_timer_cancel(timeout->timer);
    timeout->timer = NULL;
  }
}

static void prv_cancel_timer(AppTimer **timer) {
  if (*timer) {
    app_timer_cancel(*timer);
    *timer = NULL;
  }
}

static AppStatusCode prv_watch_phone_status(WatchPhoneBridgeError error) {
  switch (error) {
    case WATCH_PHONE_BRIDGE_ERROR_CONNECTIVITY:
      return STATUS_ERROR_PHONE_CONNECTIVITY;
    case WATCH_PHONE_BRIDGE_ERROR_PROTOCOL:
      return STATUS_ERROR_PROTOCOL;
    case WATCH_PHONE_BRIDGE_ERROR_SEND_START_TIMEOUT:
      return STATUS_ERROR_TRANSCRIPT_DELIVERY_TIMEOUT;
    case WATCH_PHONE_BRIDGE_ERROR_RESULT_TRANSFER_TIMEOUT:
      return STATUS_ERROR_RESULT_TRANSFER_TIMEOUT;
    case WATCH_PHONE_BRIDGE_ERROR_TRANSFER:
    case WATCH_PHONE_BRIDGE_ERROR_NONE:
    default:
      return STATUS_ERROR_TRANSFER;
  }
}

static AppStatusCode prv_phone_server_status(PhoneServerBridgeError error) {
  switch (error) {
    case PHONE_SERVER_BRIDGE_ERROR_PROTOCOL:
      return STATUS_ERROR_PROTOCOL;
    case PHONE_SERVER_BRIDGE_ERROR_SERVER:
      return STATUS_ERROR_SERVER;
    case PHONE_SERVER_BRIDGE_ERROR_RESULT_TIMEOUT:
      return STATUS_ERROR_SERVER_RESULT_TIMEOUT;
    case PHONE_SERVER_BRIDGE_ERROR_REQUEST_TIMEOUT:
      return STATUS_ERROR_WS_REQUEST_TIMEOUT;
    case PHONE_SERVER_BRIDGE_ERROR_NONE:
    default:
      return STATUS_ERROR_SERVER;
  }
}

static void prv_watch_phone_fail(WatchPhoneBridgeError error) {
  s_app.bridge.watch_phone.state = WATCH_PHONE_BRIDGE_STATE_FAILED;
  s_app.bridge.watch_phone.error = error;
  prv_fail(prv_watch_phone_status(error));
}

static void prv_phone_server_fail(PhoneServerBridgeError error) {
  s_app.bridge.phone_server.state = PHONE_SERVER_BRIDGE_STATE_FAILED;
  s_app.bridge.phone_server.error = error;
  prv_fail(prv_phone_server_status(error));
}

static void prv_set_status(const char *text) {
  if (s_app.status_layer) {
    text_layer_set_text(s_app.status_layer, text);
  }
}

static const char *prv_status_text(AppStatusCode status) {
  switch (status) {
    case STATUS_CANCELLED:
      return "Dictation\ncancelled";
    case STATUS_ERROR_DICTATION:
      return "Dictation\nerror";
    case STATUS_ERROR_NO_SPEECH:
      return "No speech\nheard";
    case STATUS_ERROR_PHONE_CONNECTIVITY:
      return "Phone\nunavailable";
    case STATUS_ERROR_SERVER_RESULT_TIMEOUT:
      return "WS server\ntimed out";
    case STATUS_ERROR_PROTOCOL:
    case STATUS_ERROR_TRANSFER:
    case STATUS_ERROR_SERVER:
      return "Delivery\nfailed";
    case STATUS_ERROR_TRANSCRIPT_DELIVERY_TIMEOUT:
      return "Delivery\ntimed out";
    case STATUS_ERROR_RESULT_TRANSFER_TIMEOUT:
      return "Result\ntimed out";
    case STATUS_ERROR_WS_REQUEST_TIMEOUT:
      return "WS request\ntimed out";
    case STATUS_NORMAL:
    default:
      return "Unknown\nerror";
  }
}

static void prv_free_transcript(void) {
  free(s_app.transcript);
  s_app.transcript = NULL;
  s_app.transcript_bytes = 0;
  s_app.chunk_capacity = 0;
  s_app.chunk_count = 0;
  s_app.next_chunk_index = 0;
  s_app.next_chunk_offset = 0;
  s_app.outbox_chunk_bytes = 0;
}

static void prv_free_server_response(void) {
  WatchPhoneBridgeContext *bridge = &s_app.bridge.watch_phone;
  free(bridge->server_response);
  bridge->server_response = NULL;
  bridge->server_response_bytes = 0;
  bridge->response_chunk_count = 0;
  bridge->next_response_chunk = 0;
  bridge->response_total_bytes = 0;
  bridge->server_success = false;
}

static void prv_generate_request_id(void) {
  time_t seconds;
  uint16_t milliseconds;
  time_ms(&seconds, &milliseconds);

  uint32_t value = (uint32_t)seconds;
  value ^= (uint32_t)milliseconds << 16;
  if (value == 0) {
    value = 1;
  }
  snprintf(s_app.request_id, sizeof(s_app.request_id), "%08lx",
           (unsigned long)value);
}

static bool prv_write_common(DictionaryIterator *iterator, MessageType type) {
  return dict_write_uint32(iterator, MESSAGE_KEY_PROTOCOL_VERSION,
                           PROTOCOL_VERSION_VALUE) == DICT_OK &&
         dict_write_uint32(iterator, MESSAGE_KEY_MESSAGE_TYPE, type) == DICT_OK &&
         dict_write_cstring(iterator, MESSAGE_KEY_REQUEST_ID,
                            s_app.request_id) == DICT_OK;
}

static AppMessageResult prv_begin_outbox(DictionaryIterator **iterator, MessageType type) {
  WatchPhoneBridgeContext *bridge = &s_app.bridge.watch_phone;
  if (!bridge->app_message_ready || bridge->outbox_message != OUTBOX_NONE) {
    return APP_MSG_BUSY;
  }

  AppMessageResult result = app_message_outbox_begin(iterator);
  if (result != APP_MSG_OK) {
    return result;
  }

  if(prv_write_common(*iterator, type)) {
    return APP_MSG_OK;
  } else {
    return APP_MSG_INTERNAL_ERROR;
  }
}

static AppMessageResult prv_send_session_begin(void) {
  DictionaryIterator *iterator;
  AppMessageResult result = prv_begin_outbox(&iterator, MESSAGE_TYPE_SESSION_BEGIN);
  if (result != APP_MSG_OK) {
    return result;
  }

  result = app_message_outbox_send();
  if (result != APP_MSG_OK) {
    return result;
  }

  s_app.bridge.watch_phone.outbox_message = OUTBOX_SESSION_BEGIN;
  return APP_MSG_OK;
}

static void prv_try_send_session_end(void) {
  WatchPhoneBridgeContext *bridge = &s_app.bridge.watch_phone;
  if (!bridge->session_end_pending || !bridge->app_message_ready ||
      bridge->outbox_message != OUTBOX_NONE) {
    return;
  }

  bridge->session_end_pending = false;

  DictionaryIterator *iterator;
  if (prv_begin_outbox(&iterator, MESSAGE_TYPE_SESSION_END) != APP_MSG_OK ||
      dict_write_uint32(iterator, MESSAGE_KEY_STATUS_CODE,
                        s_app.terminal_status) != DICT_OK ||
      app_message_outbox_send() != APP_MSG_OK) {
    return;
  }

  bridge->outbox_message = OUTBOX_SESSION_END;
}

static size_t prv_utf8_codepoint_size(const unsigned char *text,
                                      size_t remaining) {
  if (remaining == 0) {
    return 0;
  }

  const unsigned char first = text[0];
  if (first <= 0x7f) {
    return 1;
  }

  if (first >= 0xc2 && first <= 0xdf && remaining >= 2 &&
      (text[1] & 0xc0) == 0x80) {
    return 2;
  }

  if (remaining >= 3 && (text[2] & 0xc0) == 0x80) {
    const bool valid_second =
        (first == 0xe0 && text[1] >= 0xa0 && text[1] <= 0xbf) ||
        ((first >= 0xe1 && first <= 0xec) && (text[1] & 0xc0) == 0x80) ||
        (first == 0xed && text[1] >= 0x80 && text[1] <= 0x9f) ||
        ((first >= 0xee && first <= 0xef) && (text[1] & 0xc0) == 0x80);
    if (valid_second) {
      return 3;
    }
  }

  if (remaining >= 4 && (text[2] & 0xc0) == 0x80 &&
      (text[3] & 0xc0) == 0x80) {
    const bool valid_second =
        (first == 0xf0 && text[1] >= 0x90 && text[1] <= 0xbf) ||
        ((first >= 0xf1 && first <= 0xf3) && (text[1] & 0xc0) == 0x80) ||
        (first == 0xf4 && text[1] >= 0x80 && text[1] <= 0x8f);
    if (valid_second) {
      return 4;
    }
  }

  return 0;
}

static size_t prv_next_chunk_length(size_t offset) {
  size_t length = 0;

  while (offset + length < s_app.transcript_bytes) {
    const size_t remaining = s_app.transcript_bytes - offset - length;
    const size_t codepoint_size = prv_utf8_codepoint_size(
        (const unsigned char *)s_app.transcript + offset + length, remaining);

    if (codepoint_size == 0) {
      return 0;
    }
    if (length + codepoint_size > s_app.chunk_capacity) {
      break;
    }
    length += codepoint_size;
  }

  return length;
}

static bool prv_prepare_chunks(void) {
  if (!s_app.transcript || s_app.transcript_bytes == 0 ||
      s_app.transcript_bytes > UINT32_MAX) {
    return false;
  }

  const uint32_t dictionary_overhead = dict_calc_buffer_size(
      7, (uint32_t)sizeof(uint32_t), (uint32_t)sizeof(uint32_t),
      (uint32_t)sizeof(s_app.request_id), (uint32_t)sizeof(uint32_t),
      (uint32_t)sizeof(uint32_t), (uint32_t)sizeof(uint32_t), 1U);

  if (dictionary_overhead >= s_app.outbox_size) {
    return false;
  }

  s_app.chunk_capacity = s_app.outbox_size - dictionary_overhead;
  if (s_app.chunk_capacity > UINT16_MAX - 1U) {
    s_app.chunk_capacity = UINT16_MAX - 1U;
  }

  size_t offset = 0;
  uint32_t count = 0;
  while (offset < s_app.transcript_bytes) {
    const size_t length = prv_next_chunk_length(offset);
    if (length == 0 || count == UINT32_MAX) {
      return false;
    }
    offset += length;
    count++;
  }

  s_app.chunk_count = count;
  s_app.next_chunk_index = 0;
  s_app.next_chunk_offset = 0;
  return count > 0;
}

static AppMessageResult prv_send_chunk(size_t length) {
  DictionaryIterator *iterator;
  AppMessageResult result = prv_begin_outbox(&iterator, MESSAGE_TYPE_TRANSCRIPT_CHUNK);
  if (result != APP_MSG_OK) {
    return result;
  }

  char *chunk_end = s_app.transcript + s_app.next_chunk_offset + length;
  const char saved = *chunk_end;
  *chunk_end = '\0';

  const bool wrote_message =
      dict_write_uint32(iterator, MESSAGE_KEY_CHUNK_INDEX,
                        s_app.next_chunk_index) == DICT_OK &&
      dict_write_uint32(iterator, MESSAGE_KEY_CHUNK_COUNT,
                        s_app.chunk_count) == DICT_OK &&
      dict_write_uint32(iterator, MESSAGE_KEY_TOTAL_BYTES,
                        (uint32_t)s_app.transcript_bytes) == DICT_OK &&
      dict_write_cstring(iterator, MESSAGE_KEY_CHUNK_TEXT,
                         s_app.transcript + s_app.next_chunk_offset) == DICT_OK;

  *chunk_end = saved;

  if (!wrote_message) {
    return APP_MSG_INTERNAL_ERROR;
  }
  
  result = app_message_outbox_send();
  if (result != APP_MSG_OK) {
    return result;
  }

  s_app.outbox_chunk_bytes = length;
  s_app.bridge.watch_phone.outbox_message = OUTBOX_TRANSCRIPT_CHUNK;
  return APP_MSG_OK;
}

static void prv_transcript_delivery_timeout(void *context) {
  (void)context;
  s_app.bridge.watch_phone.send_start_timeout.timer = NULL;
  prv_watch_phone_fail(WATCH_PHONE_BRIDGE_ERROR_SEND_START_TIMEOUT);
}

static void prv_ws_request_timeout(void *context) {
  (void)context;
  s_app.bridge.phone_server.request_timeout.timer = NULL;
  prv_phone_server_fail(PHONE_SERVER_BRIDGE_ERROR_REQUEST_TIMEOUT);
}

static void prv_result_chunk_timeout(void *context) {
  (void)context;
  s_app.bridge.watch_phone.result_chunk_timeout.timer = NULL;
  prv_watch_phone_fail(WATCH_PHONE_BRIDGE_ERROR_RESULT_TRANSFER_TIMEOUT);
}

static void prv_exit_after_error(void *context) {
  (void)context;
  s_app.exit_timer = NULL;
  window_stack_pop_all(false);
}

static bool prv_valid_utf8(const char *text, size_t bytes) {
  size_t offset = 0;
  while (offset < bytes) {
    const size_t codepoint_size = prv_utf8_codepoint_size(
        (const unsigned char *)text + offset, bytes - offset);
    if (codepoint_size == 0) {
      return false;
    }
    offset += codepoint_size;
  }
  return true;
}

static void prv_destroy_result_layers(void) {
  if (s_app.result_text_layer) {
    text_layer_destroy(s_app.result_text_layer);
    s_app.result_text_layer = NULL;
  }
  if (s_app.result_scroll_layer) {
    scroll_layer_destroy(s_app.result_scroll_layer);
    s_app.result_scroll_layer = NULL;
  }
  if (s_app.result_heading_layer) {
    text_layer_destroy(s_app.result_heading_layer);
    s_app.result_heading_layer = NULL;
  }
}

static void prv_result_click_config_provider(void *context);

static void prv_vibrate_for_server_result(bool success) {
  if (success) {
    vibes_short_pulse();
    return;
  }

  static const uint32_t segments[] = {100, 100, 100, 100, 100};
  vibes_enqueue_custom_pattern((VibePattern){
      .durations = segments,
      .num_segments = ARRAY_LENGTH(segments),
  });
}

static bool prv_show_server_result(void) {
  WatchPhoneBridgeContext *bridge = &s_app.bridge.watch_phone;
  Layer *root_layer = window_get_root_layer(s_app.window);
  const GRect bounds = layer_get_bounds(root_layer);
  const int16_t margin = 8;
  const int16_t heading_height = 42;
  const int16_t text_measurement_height = 2000;
  const GRect scroll_frame = GRect(
      margin, heading_height, bounds.size.w - margin * 2,
      bounds.size.h - heading_height - margin);

  s_app.result_heading_layer = text_layer_create(
      GRect(0, 4, bounds.size.w, heading_height - 4));
  s_app.result_scroll_layer = scroll_layer_create(scroll_frame);
  s_app.result_text_layer = text_layer_create(
      GRect(0, 0, scroll_frame.size.w, text_measurement_height));
  if (!s_app.result_heading_layer || !s_app.result_scroll_layer ||
      !s_app.result_text_layer) {
    prv_destroy_result_layers();
    return false;
  }

  text_layer_set_background_color(s_app.result_heading_layer, GColorClear);
  text_layer_set_text_color(s_app.result_heading_layer, GColorWhite);
  text_layer_set_font(s_app.result_heading_layer,
                      fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
  text_layer_set_text_alignment(s_app.result_heading_layer,
                                GTextAlignmentCenter);
  text_layer_set_text(s_app.result_heading_layer,
                      bridge->server_success ? "Success" : "Failure");

  text_layer_set_background_color(s_app.result_text_layer, GColorClear);
  text_layer_set_text_color(s_app.result_text_layer, GColorWhite);
  text_layer_set_font(s_app.result_text_layer,
                      fonts_get_system_font(FONT_KEY_GOTHIC_18));
  text_layer_set_overflow_mode(s_app.result_text_layer,
                               GTextOverflowModeWordWrap);
  text_layer_set_text(s_app.result_text_layer, bridge->server_response);
  GSize content_size = text_layer_get_content_size(s_app.result_text_layer);
  if (content_size.h < scroll_frame.size.h) {
    content_size.h = scroll_frame.size.h;
  }
  layer_set_frame(text_layer_get_layer(s_app.result_text_layer),
                  GRect(0, 0, scroll_frame.size.w, content_size.h));
  scroll_layer_add_child(s_app.result_scroll_layer,
                         text_layer_get_layer(s_app.result_text_layer));
  scroll_layer_set_content_size(
      s_app.result_scroll_layer,
      GSize(scroll_frame.size.w, content_size.h));
  scroll_layer_set_content_offset(s_app.result_scroll_layer, GPointZero,
                                  false);
  scroll_layer_set_callbacks(
      s_app.result_scroll_layer,
      (ScrollLayerCallbacks){
          .click_config_provider = prv_result_click_config_provider,
      });
  scroll_layer_set_click_config_onto_window(s_app.result_scroll_layer,
                                             s_app.window);

  layer_set_hidden(text_layer_get_layer(s_app.status_layer), true);
  layer_add_child(root_layer,
                  text_layer_get_layer(s_app.result_heading_layer));
  layer_add_child(root_layer,
                  scroll_layer_get_layer(s_app.result_scroll_layer));
  s_app.exit_timer = app_timer_register(
      ERROR_DISPLAY_MS, prv_exit_after_error, NULL);
  if (!s_app.exit_timer) {
    layer_set_hidden(text_layer_get_layer(s_app.status_layer), false);
    prv_destroy_result_layers();
    return false;
  }
  s_app.state = APP_STATE_DISPLAYING_RESULT;
  light_enable_interaction();
  return true;
}

static bool prv_receive_server_result_chunk(DictionaryIterator *iterator) {
  WatchPhoneBridgeContext *bridge = &s_app.bridge.watch_phone;
  if (s_app.state != APP_STATE_BRIDGING ||
      (bridge->state != WATCH_PHONE_BRIDGE_STATE_AWAITING_RESULT &&
       bridge->state != WATCH_PHONE_BRIDGE_STATE_RECEIVING_RESULT)) {
    return false;
  }

  uint32_t chunk_index;
  uint32_t chunk_count;
  uint32_t total_bytes;
  uint32_t success;
  const Tuple *chunk = dict_find(iterator, MESSAGE_KEY_CHUNK_TEXT);
  if (!prv_tuple_to_uint32(dict_find(iterator, MESSAGE_KEY_CHUNK_INDEX),
                           &chunk_index) ||
      !prv_tuple_to_uint32(dict_find(iterator, MESSAGE_KEY_CHUNK_COUNT),
                           &chunk_count) ||
      !prv_tuple_to_uint32(dict_find(iterator, MESSAGE_KEY_TOTAL_BYTES),
                           &total_bytes) ||
      !prv_tuple_to_uint32(dict_find(iterator, MESSAGE_KEY_SERVER_SUCCESS),
                           &success) ||
      !chunk || chunk->type != TUPLE_CSTRING || chunk->length == 0 ||
      chunk_count == 0 || chunk_index >= chunk_count || success > 1 ||
      total_bytes > SERVER_RESPONSE_MAX_BYTES) {
    return false;
  }

  const char *chunk_text = (const char *)chunk->value;
  const size_t chunk_bytes = chunk->length - 1;
  if (strlen(chunk_text) != chunk_bytes ||
      !prv_valid_utf8(chunk_text, chunk_bytes)) {
    return false;
  }

  if (bridge->state == WATCH_PHONE_BRIDGE_STATE_AWAITING_RESULT) {
    if (chunk_index != 0) {
      return false;
    }
    bridge->server_response = malloc((size_t)total_bytes + 1);
    if (!bridge->server_response) {
      bridge->error = WATCH_PHONE_BRIDGE_ERROR_TRANSFER;
      return false;
    }
    bridge->response_chunk_count = chunk_count;
    bridge->response_total_bytes = total_bytes;
    bridge->server_success = success == 1;
    bridge->state = WATCH_PHONE_BRIDGE_STATE_RECEIVING_RESULT;
    s_app.bridge.phone_server.state = PHONE_SERVER_BRIDGE_STATE_COMPLETE;
    prv_cancel_bridge_timeout(&s_app.bridge.phone_server.request_timeout);
  } else if (chunk_count != bridge->response_chunk_count ||
             total_bytes != bridge->response_total_bytes ||
             (success == 1) != bridge->server_success) {
    return false;
  }

  if (chunk_index != bridge->next_response_chunk ||
      bridge->server_response_bytes + chunk_bytes >
          bridge->response_total_bytes) {
    return false;
  }

  memcpy(bridge->server_response + bridge->server_response_bytes,
         chunk_text, chunk_bytes);
  bridge->server_response_bytes += chunk_bytes;
  bridge->next_response_chunk++;

  if (bridge->next_response_chunk == bridge->response_chunk_count) {
    if (bridge->server_response_bytes != bridge->response_total_bytes) {
      return false;
    }
    bridge->server_response[bridge->server_response_bytes] = '\0';
    bridge->state = WATCH_PHONE_BRIDGE_STATE_COMPLETE;
    prv_cancel_bridge_timeout(&bridge->result_chunk_timeout);
    prv_vibrate_for_server_result(bridge->server_success);
    if (!prv_show_server_result()) {
      bridge->error = WATCH_PHONE_BRIDGE_ERROR_TRANSFER;
      return false;
    }
    return true;
  }

  if (bridge->server_response_bytes >= bridge->response_total_bytes) {
    return false;
  }
  prv_cancel_bridge_timeout(&bridge->result_chunk_timeout);
  bridge->result_chunk_timeout.timer = app_timer_register(
      bridge->result_chunk_timeout.duration_ms,
      prv_result_chunk_timeout, NULL);
  if (!bridge->result_chunk_timeout.timer) {
    bridge->error = WATCH_PHONE_BRIDGE_ERROR_TRANSFER;
    return false;
  }
  return true;
}

static void prv_send_next_chunk(void) {
  WatchPhoneBridgeContext *bridge = &s_app.bridge.watch_phone;
  if (s_app.state != APP_STATE_BRIDGING ||
      bridge->state != WATCH_PHONE_BRIDGE_STATE_TRANSFERRING ||
      !bridge->begin_delivered || bridge->outbox_message != OUTBOX_NONE) {
    return;
  }

  const size_t length = prv_next_chunk_length(s_app.next_chunk_offset);
  if(length == 0) {
    prv_fail(STATUS_ERROR_TRANSFER);
    return;
  }
  AppMessageResult result = prv_send_chunk(length);
  if(result & APP_MSG_NOT_CONNECTED) {
    prv_watch_phone_fail(WATCH_PHONE_BRIDGE_ERROR_CONNECTIVITY);
  } else if(result != APP_MSG_OK) {
    prv_watch_phone_fail(WATCH_PHONE_BRIDGE_ERROR_TRANSFER);
  }
}

static void prv_fail(AppStatusCode status) {
  if (s_app.shutting_down || s_app.state == APP_STATE_ERROR ||
  s_app.state == APP_STATE_DISPLAYING_RESULT) {
    return;
  }

  APP_LOG(APP_LOG_LEVEL_ERROR, "Request failed with status %d", status);
  s_app.state = APP_STATE_ERROR;
  s_app.terminal_status = status;

  prv_cancel_bridge_timeout(&s_app.bridge.watch_phone.send_start_timeout);
  prv_cancel_bridge_timeout(&s_app.bridge.watch_phone.result_chunk_timeout);
  prv_cancel_bridge_timeout(&s_app.bridge.phone_server.request_timeout);

  if (s_app.dictation_active && s_app.dictation) {
    dictation_session_stop(s_app.dictation);
    s_app.dictation_active = false;
  }

  prv_free_transcript();
  prv_free_server_response();
  s_app.bridge.watch_phone.session_end_pending = true;
  prv_try_send_session_end();
  prv_set_status(prv_status_text(status));
  light_enable_interaction();

  prv_cancel_timer(&s_app.exit_timer);
  s_app.exit_timer =
      app_timer_register(ERROR_DISPLAY_MS, prv_exit_after_error, NULL);
  if (!s_app.exit_timer) {
    window_stack_pop_all(false);
  }
}

static bool prv_tuple_to_uint32(const Tuple *tuple, uint32_t *value) {
  if (!tuple || !value) {
    return false;
  }

  if (tuple->type == TUPLE_UINT) {
    switch (tuple->length) {
      case 1:
        *value = tuple->value->uint8;
        return true;
      case 2:
        *value = tuple->value->uint16;
        return true;
      case 4:
        *value = tuple->value->uint32;
        return true;
      default:
        return false;
    }
  }

  if (tuple->type == TUPLE_INT) {
    int32_t signed_value;
    switch (tuple->length) {
      case 1:
        signed_value = tuple->value->int8;
        break;
      case 2:
        signed_value = tuple->value->int16;
        break;
      case 4:
        signed_value = tuple->value->int32;
        break;
      default:
        return false;
    }

    if (signed_value < 0) {
      return false;
    }
    *value = (uint32_t)signed_value;
    return true;
  }

  return false;
}

static bool prv_request_id_matches(const Tuple *tuple) {
  if (!tuple || tuple->type != TUPLE_CSTRING ||
      tuple->length != sizeof(s_app.request_id)) {
    return false;
  }

  const char *value = (const char *)tuple->value;
  return memcmp(value, s_app.request_id, sizeof(s_app.request_id)) == 0;
}

static void prv_inbox_received(DictionaryIterator *iterator, void *context) {
  (void)context;

  if (s_app.state == APP_STATE_ERROR ||
      s_app.state == APP_STATE_DISPLAYING_RESULT) {
    return;
  }

  const Tuple *request_id = dict_find(iterator, MESSAGE_KEY_REQUEST_ID);
  if (!request_id) {
    prv_fail(STATUS_ERROR_PROTOCOL);
    return;
  }
  if (!prv_request_id_matches(request_id)) {
    return;
  }

  uint32_t version;
  uint32_t message_type;
  if (!prv_tuple_to_uint32(
          dict_find(iterator, MESSAGE_KEY_PROTOCOL_VERSION), &version) ||
      version != PROTOCOL_VERSION_VALUE ||
      !prv_tuple_to_uint32(dict_find(iterator, MESSAGE_KEY_MESSAGE_TYPE),
                           &message_type)) {
    prv_fail(STATUS_ERROR_PROTOCOL);
    return;
  }

  switch (message_type) {
    case MESSAGE_TYPE_SEND_STARTED:
      if (s_app.state != APP_STATE_BRIDGING ||
          s_app.bridge.watch_phone.state !=
              WATCH_PHONE_BRIDGE_STATE_AWAITING_SEND_START ||
          s_app.bridge.phone_server.state !=
              PHONE_SERVER_BRIDGE_STATE_PREPARING) {
        prv_watch_phone_fail(WATCH_PHONE_BRIDGE_ERROR_PROTOCOL);
        return;
      }
      prv_cancel_bridge_timeout(
          &s_app.bridge.watch_phone.send_start_timeout);
        s_app.bridge.watch_phone.state =
          WATCH_PHONE_BRIDGE_STATE_AWAITING_RESULT;
      s_app.bridge.phone_server.state =
          PHONE_SERVER_BRIDGE_STATE_AWAITING_RESULT;
      BridgeTimeout *timeout = &s_app.bridge.phone_server.request_timeout;
      timeout->timer = app_timer_register(
          timeout->duration_ms, prv_ws_request_timeout, NULL);
      if (!timeout->timer) {
        prv_watch_phone_fail(WATCH_PHONE_BRIDGE_ERROR_TRANSFER);
      }
      prv_set_status("Thinking...");
      return;

    case MESSAGE_TYPE_SERVER_RESULT_CHUNK:
      if (!prv_receive_server_result_chunk(iterator)) {
        const WatchPhoneBridgeError error =
            s_app.bridge.watch_phone.error == WATCH_PHONE_BRIDGE_ERROR_NONE
                ? WATCH_PHONE_BRIDGE_ERROR_PROTOCOL
                : s_app.bridge.watch_phone.error;
        prv_watch_phone_fail(error);
      }
      return;

    case MESSAGE_TYPE_FAILURE: {
      uint32_t status;
      if (!prv_tuple_to_uint32(dict_find(iterator, MESSAGE_KEY_STATUS_CODE),
                               &status) ||
          status == STATUS_NORMAL ||
          status > STATUS_ERROR_SERVER_RESULT_TIMEOUT) {
        prv_watch_phone_fail(WATCH_PHONE_BRIDGE_ERROR_PROTOCOL);
        return;
      }
      switch (status) {
        case STATUS_ERROR_SERVER:
          prv_phone_server_fail(PHONE_SERVER_BRIDGE_ERROR_SERVER);
          break;
        case STATUS_ERROR_SERVER_RESULT_TIMEOUT:
          prv_phone_server_fail(PHONE_SERVER_BRIDGE_ERROR_RESULT_TIMEOUT);
          break;
        case STATUS_ERROR_PHONE_CONNECTIVITY:
          prv_watch_phone_fail(WATCH_PHONE_BRIDGE_ERROR_CONNECTIVITY);
          break;
        case STATUS_ERROR_PROTOCOL:
          if (s_app.bridge.phone_server.state ==
              PHONE_SERVER_BRIDGE_STATE_AWAITING_RESULT) {
            prv_phone_server_fail(PHONE_SERVER_BRIDGE_ERROR_PROTOCOL);
          } else {
            prv_watch_phone_fail(WATCH_PHONE_BRIDGE_ERROR_PROTOCOL);
          }
          break;
        case STATUS_ERROR_TRANSFER:
          prv_watch_phone_fail(WATCH_PHONE_BRIDGE_ERROR_TRANSFER);
          break;
        default:
          prv_watch_phone_fail(WATCH_PHONE_BRIDGE_ERROR_PROTOCOL);
          break;
      }
      return;
    }

    default:
      prv_watch_phone_fail(WATCH_PHONE_BRIDGE_ERROR_PROTOCOL);
  }
}

static void prv_inbox_dropped(AppMessageResult reason, void *context) {
  (void)context;
  APP_LOG(APP_LOG_LEVEL_ERROR, "Inbox dropped: %d", reason);
  prv_watch_phone_fail(WATCH_PHONE_BRIDGE_ERROR_TRANSFER);
}

static void prv_outbox_sent(DictionaryIterator *iterator, void *context) {
  (void)iterator;
  (void)context;

  WatchPhoneBridgeContext *bridge = &s_app.bridge.watch_phone;
  const OutboxMessage sent_message = bridge->outbox_message;
  bridge->outbox_message = OUTBOX_NONE;

  if (s_app.state == APP_STATE_ERROR) {
    prv_try_send_session_end();
    return;
  }

  switch (sent_message) {
    case OUTBOX_SESSION_BEGIN:
      bridge->begin_delivered = true;
      if (bridge->state == WATCH_PHONE_BRIDGE_STATE_SESSION_BEGIN_PENDING) {
        bridge->state = WATCH_PHONE_BRIDGE_STATE_READY;
      }
      prv_send_next_chunk();
      break;

    case OUTBOX_TRANSCRIPT_CHUNK:
      if (s_app.state != APP_STATE_BRIDGING ||
          bridge->state != WATCH_PHONE_BRIDGE_STATE_TRANSFERRING) {
        prv_watch_phone_fail(WATCH_PHONE_BRIDGE_ERROR_PROTOCOL);
        return;
      }

      s_app.next_chunk_offset += s_app.outbox_chunk_bytes;
      s_app.next_chunk_index++;
      s_app.outbox_chunk_bytes = 0;

      if (s_app.next_chunk_index < s_app.chunk_count) {
        prv_send_next_chunk();
        return;
      }

      prv_free_transcript();
      bridge->state = WATCH_PHONE_BRIDGE_STATE_AWAITING_SEND_START;
      BridgeTimeout *timeout = &bridge->send_start_timeout;
      timeout->timer = app_timer_register(
          timeout->duration_ms, prv_transcript_delivery_timeout, NULL);
      if (!timeout->timer) {
        prv_watch_phone_fail(WATCH_PHONE_BRIDGE_ERROR_TRANSFER);
      }
      break;

    case OUTBOX_SESSION_END:
    case OUTBOX_NONE:
      break;
  }
}

static void prv_outbox_failed(DictionaryIterator *iterator,
                              AppMessageResult reason, void *context) {
  (void)iterator;
  (void)context;

  APP_LOG(APP_LOG_LEVEL_ERROR, "Outbox failed: %d", reason);
  WatchPhoneBridgeContext *bridge = &s_app.bridge.watch_phone;
  const OutboxMessage failed_message = bridge->outbox_message;
  bridge->outbox_message = OUTBOX_NONE;

  if (s_app.state == APP_STATE_ERROR) {
    if (failed_message != OUTBOX_SESSION_END) {
      prv_try_send_session_end();
    }
    return;
  }

  prv_watch_phone_fail(
      (reason & APP_MSG_NOT_CONNECTED) != 0
          ? WATCH_PHONE_BRIDGE_ERROR_CONNECTIVITY
          : WATCH_PHONE_BRIDGE_ERROR_TRANSFER);
}

static void prv_dictation_status(DictationSession *session,
                                 DictationSessionStatus status,
                                 char *transcription, void *context) {
  (void)session;
  (void)context;
  s_app.dictation_active = false;

  if (s_app.state != APP_STATE_DICTATING) {
    return;
  }

  if (status != DictationSessionStatusSuccess) {
    switch (status) {
      case DictationSessionStatusFailureTranscriptionRejected:
        prv_fail(STATUS_CANCELLED);
        break;
      case DictationSessionStatusFailureNoSpeechDetected:
        prv_fail(STATUS_ERROR_NO_SPEECH);
        break;
      case DictationSessionStatusFailureConnectivityError:
        prv_fail(STATUS_ERROR_PHONE_CONNECTIVITY);
        break;
      case DictationSessionStatusFailureDisabled:
      case DictationSessionStatusFailureTranscriptionRejectedWithError:
      case DictationSessionStatusFailureSystemAborted:
      case DictationSessionStatusFailureInternalError:
      case DictationSessionStatusFailureRecognizerError:
      default:
        prv_fail(STATUS_ERROR_DICTATION);
        break;
    }
    return;
  }

  if (!transcription || transcription[0] == '\0') {
    prv_fail(STATUS_ERROR_NO_SPEECH);
    return;
  }

  s_app.transcript_bytes = strlen(transcription);
  s_app.transcript = malloc(s_app.transcript_bytes + 1);
  if (!s_app.transcript) {
    prv_fail(STATUS_ERROR_TRANSFER);
    return;
  }
  memcpy(s_app.transcript, transcription, s_app.transcript_bytes + 1);

  if (!prv_prepare_chunks()) {
    prv_fail(STATUS_ERROR_TRANSFER);
    return;
  }

  s_app.state = APP_STATE_BRIDGING;
  s_app.bridge.watch_phone.state = WATCH_PHONE_BRIDGE_STATE_TRANSFERRING;
  prv_set_status("Sending...");
  prv_send_next_chunk();
}

static void prv_back_click_handler(ClickRecognizerRef recognizer,
                                   void *context) {
  (void)recognizer;
  (void)context;

  if (s_app.state != APP_STATE_ERROR &&
      s_app.state != APP_STATE_DISPLAYING_RESULT) {
    prv_fail(STATUS_CANCELLED);
  }
}

static void prv_cancel_result_timeout(void) {
  if (s_app.state == APP_STATE_DISPLAYING_RESULT) {
    prv_cancel_timer(&s_app.exit_timer);
  }
}

static void prv_result_button_down_handler(ClickRecognizerRef recognizer,
                                           void *context) {
  (void)recognizer;
  (void)context;
  prv_cancel_result_timeout();
}

static void prv_result_back_click_handler(ClickRecognizerRef recognizer,
                                          void *context) {
  (void)recognizer;
  (void)context;
  if (s_app.state == APP_STATE_DISPLAYING_RESULT) {
    prv_cancel_result_timeout();
    window_stack_pop_all(false);
  }
}

static void prv_result_click_config_provider(void *context) {
  // Raw handlers observe presses without replacing ScrollLayer's repeating
  // UP/DOWN click handlers, so scrolling keeps its standard behavior.
  window_raw_click_subscribe(BUTTON_ID_UP, prv_result_button_down_handler,
                             NULL, context);
  window_raw_click_subscribe(BUTTON_ID_SELECT,
                             prv_result_button_down_handler, NULL, context);
  window_raw_click_subscribe(BUTTON_ID_DOWN, prv_result_button_down_handler,
                             NULL, context);
  window_single_click_subscribe(BUTTON_ID_BACK,
                                prv_result_back_click_handler);
}

static void prv_click_config_provider(void *context) {
  (void)context;
  window_single_click_subscribe(BUTTON_ID_BACK, prv_back_click_handler);
}

static void prv_window_load(Window *window) {
  Layer *root_layer = window_get_root_layer(window);
  const GRect bounds = layer_get_bounds(root_layer);

  const int16_t status_height = 80;
  const int16_t status_y = (bounds.size.h - status_height) / 2;
  s_app.status_layer = text_layer_create(
      GRect(0, status_y, bounds.size.w, status_height));
  if (!s_app.status_layer) {
    s_app.initialization_failed = true;
    return;
  }

  text_layer_set_background_color(s_app.status_layer, GColorClear);
  text_layer_set_text_color(s_app.status_layer, GColorWhite);
  text_layer_set_font(s_app.status_layer,
                      fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
  text_layer_set_text_alignment(s_app.status_layer, GTextAlignmentCenter);
  text_layer_set_overflow_mode(s_app.status_layer, GTextOverflowModeWordWrap);
  text_layer_set_text(s_app.status_layer, "Starting...");
  layer_add_child(root_layer, text_layer_get_layer(s_app.status_layer));
}

static void prv_window_unload(Window *window) {
  (void)window;
  prv_destroy_result_layers();
  if (s_app.status_layer) {
    text_layer_destroy(s_app.status_layer);
  }
  s_app.status_layer = NULL;
}

static bool prv_init(void) {
  memset(&s_app, 0, sizeof(s_app));
  s_app.state = APP_STATE_INITIALIZING;
  prv_bridge_init(&s_app.bridge);
  prv_generate_request_id();
  APP_LOG(APP_LOG_LEVEL_INFO, "Request ID: %s", s_app.request_id);

  s_app.window = window_create();
  if (!s_app.window) {
    return false;
  }
  window_set_background_color(s_app.window, GColorBlack);
  window_set_click_config_provider(s_app.window, prv_click_config_provider);
  window_set_window_handlers(s_app.window, (WindowHandlers){
                                               .load = prv_window_load,
                                               .unload = prv_window_unload,
                                           });
  window_stack_push(s_app.window, false);
  if (s_app.initialization_failed) {
    return false;
  }

  app_message_register_inbox_received(prv_inbox_received);
  app_message_register_inbox_dropped(prv_inbox_dropped);
  app_message_register_outbox_sent(prv_outbox_sent);
  app_message_register_outbox_failed(prv_outbox_failed);

  const uint32_t inbox_size = app_message_inbox_size_maximum();
  s_app.outbox_size = app_message_outbox_size_maximum();

  AppMessageResult result = app_message_open(inbox_size, s_app.outbox_size);
  if(result & APP_MSG_NOT_CONNECTED) {
    prv_fail(STATUS_ERROR_PHONE_CONNECTIVITY);
    return true;
  } else if(result != APP_MSG_OK) {
    prv_fail(STATUS_ERROR_TRANSFER);
    return true;
  }
  s_app.bridge.watch_phone.app_message_ready = true;

  s_app.dictation = dictation_session_create(0, prv_dictation_status, NULL);
  if (!s_app.dictation) {
    prv_fail(STATUS_ERROR_DICTATION);
    return true;
  }
  dictation_session_enable_confirmation(s_app.dictation, false);
  dictation_session_enable_error_dialogs(s_app.dictation, false);

  s_app.state = APP_STATE_DICTATING;
  s_app.bridge.watch_phone.state =
      WATCH_PHONE_BRIDGE_STATE_SESSION_BEGIN_PENDING;
  s_app.bridge.phone_server.state = PHONE_SERVER_BRIDGE_STATE_PREPARING;
  result = prv_send_session_begin();
  if(result & APP_MSG_NOT_CONNECTED) {
    prv_fail(STATUS_ERROR_PHONE_CONNECTIVITY);
    return true;
  } else if(result != APP_MSG_OK) {
    prv_fail(STATUS_ERROR_TRANSFER);
    return true;
  }

  if (dictation_session_start(s_app.dictation) !=
      DictationSessionStatusSuccess) {
    prv_fail(STATUS_ERROR_DICTATION);
    return true;
  }
  s_app.dictation_active = true;
  return true;
}

static void prv_deinit(void) {
  s_app.shutting_down = true;
  prv_cancel_bridge_timeout(&s_app.bridge.watch_phone.send_start_timeout);
  prv_cancel_bridge_timeout(&s_app.bridge.watch_phone.result_chunk_timeout);
  prv_cancel_bridge_timeout(&s_app.bridge.phone_server.request_timeout);
  prv_cancel_timer(&s_app.exit_timer);

  if (s_app.dictation_active && s_app.dictation) {
    dictation_session_stop(s_app.dictation);
    s_app.dictation_active = false;
  }
  if (s_app.dictation) {
    dictation_session_destroy(s_app.dictation);
    s_app.dictation = NULL;
  }

  prv_free_transcript();
  prv_free_server_response();
  if (s_app.window) {
    window_destroy(s_app.window);
    s_app.window = NULL;
  }
}

int main(void) {
  if (!prv_init()) {
    prv_deinit();
    return 1;
  }

  app_event_loop();
  prv_deinit();
  return 0;
}
