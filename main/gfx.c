#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_websocket_client.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <http_parser.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <webp/demux.h>

#include "assets.h"
#include "display.h"
#include "gfx.h"
#include "esp_timer.h"
#include "nvs_settings.h"
#include "remote.h"
#include "version.h"

static const char *TAG = "gfx";

#define GFX_TASK_CORE 1
#define GFX_TASK_PRIO 2
#define GFX_TASK_STACK_SIZE 4092

struct gfx_state {
  TaskHandle_t task;
  SemaphoreHandle_t mutex;
  void *buf;
  size_t len;
  int32_t dwell_secs;
  int counter;
  int loaded_counter;  // Counter that tracks which image has been loaded by gfx
                       // task
  esp_websocket_client_handle_t
      ws_handle;  // Websocket handle for sending notifications
  volatile bool paused;
  struct app_meta meta;  // rotation position of buf (count 0 = none)
  char displaying_id[32];  // app ID of the image on screen ("" = none)
  int32_t displaying_index;      // its rotation position
  int64_t displaying_since_us;   // when it appeared on screen
  int64_t displaying_since_unix_ms;  // same in wall-clock time, 0 if unsynced
};

// Wall-clock time in Unix ms, or 0 while SNTP has not set the clock yet.
static int64_t unix_ms_now(void) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  if (tv.tv_sec < 1700000000) return 0;  // before Nov 2023: not synced
  return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static struct gfx_state *_state = NULL;

#ifdef CONFIG_IMAGE_CACHE
// One slot per app in the server rotation, indexed by Tronbyt-App-Index.
// Only touched by the gfx task.
#define IMAGE_CACHE_MAX_APPS 256  // sanity limit on Tronbyt-App-Count

struct cache_slot {
  char id[32];
  uint8_t *buf;
  size_t len;
  int32_t dwell_secs;
  bool shown;  // on screen (fresh or cached) since the last fresh image
};
static struct cache_slot *s_cache = NULL;
static int32_t s_cache_count = 0;

// Playback position, protected by the gfx mutex: s_cursor is the slot on
// screen, s_fresh the slot of the last image shown fresh from the server,
// and s_played how many cached images were shown since (can exceed the
// rotation length during a long stall).
static int32_t s_cursor = -1;
static int32_t s_fresh = -1;
static int32_t s_played = 0;

static void cache_clear(void) {
  for (int32_t i = 0; i < s_cache_count; i++) {
    free(s_cache[i].buf);
  }
  free(s_cache);
  s_cache = NULL;
  s_cache_count = 0;
  s_cursor = -1;
  s_fresh = -1;
  s_played = 0;
}

// The next filled slot after the one on screen, or -1 if there is none.
static int32_t cache_next_slot(void) {
  if (s_cursor < 0 || s_cache_count <= 0) return -1;
  for (int32_t step = 1; step < s_cache_count; step++) {
    int32_t i = (s_cursor + step) % s_cache_count;
    if (s_cache[i].buf != NULL) return i;
  }
  return -1;
}

// Whether a fresh image for this slot should be shown, or was already shown
// from the cache while its download was pending (then showing it would step
// back in the rotation).
static bool cache_is_due(int32_t index) {
  // Nothing shown from the cache since the last fresh image: normal case.
  if (s_fresh < 0 || s_played == 0 || s_cache_count <= 1) return true;
  // Exactly the app the cache would show next: show the fresh copy instead.
  if (index == cache_next_slot()) return true;
  // Otherwise only if it has not been on screen since the last fresh image.
  return !s_cache[index].shown;
}

// True once every app in the rotation has arrived at least once. Until then
// cache playback would repeat the few apps it has, out of order.
static bool cache_complete(void) {
  if (s_cache_count <= 0) return false;
  for (int32_t i = 0; i < s_cache_count; i++) {
    if (s_cache[i].buf == NULL) return false;
  }
  return true;
}

static void cache_store(const void *webp, size_t len, int32_t dwell_secs,
                        const struct app_meta *meta) {
  if (webp == NULL || len == 0 || meta == NULL || meta->count <= 0) return;
  if (meta->count > IMAGE_CACHE_MAX_APPS) {
    ESP_LOGW(TAG, "Image cache: %d apps exceeds limit, not cached",
             (int)meta->count);
    return;
  }

  // The rotation changed size (app added or removed): start over.
  if (meta->count != s_cache_count) {
    if (s_cache_count > 0) {
      ESP_LOGI(TAG, "Image cache: rotation changed from %d to %d apps, clearing",
               (int)s_cache_count, (int)meta->count);
    }
    cache_clear();
    s_cache = heap_caps_calloc(meta->count, sizeof(*s_cache),
                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_cache == NULL) {
      ESP_LOGW(TAG, "Image cache: no memory for %d slots", (int)meta->count);
      return;
    }
    s_cache_count = meta->count;
  }

  uint8_t *copy = heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (copy == NULL) {
    ESP_LOGW(TAG, "Image cache: no memory for %zu bytes, not cached", len);
    return;
  }
  memcpy(copy, webp, len);

  struct cache_slot *slot = &s_cache[meta->index];
  free(slot->buf);
  strlcpy(slot->id, meta->id, sizeof(slot->id));
  slot->buf = copy;
  slot->len = len;
  slot->dwell_secs = dwell_secs;
  ESP_LOGI(TAG, "Image cache: stored id=%s in slot %d/%d (%zu bytes)",
           slot->id, (int)meta->index, (int)s_cache_count - 1, len);
}
#endif

static void gfx_loop(void *arg);
static int draw_webp(const uint8_t *buf, size_t len, int32_t dwell_secs,
                     volatile int32_t *isAnimating);
static void send_websocket_notification(int counter);

int gfx_initialize(const char *img_url) {
  // Only initialize once
  if (_state) {
    ESP_LOGE(TAG, "Already initialized");
    return 1;
  }

  int heapl = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);

  ESP_LOGI(TAG, "largest heap %d", heapl);
  // ESP_LOGI(TAG, "calling calloc");
  // Initialize state
  ESP_LOGI(TAG, "Allocating buffer of size: %d", ASSET_BOOT_WEBP_LEN);

  _state = calloc(1, sizeof(struct gfx_state));
  _state->paused = false;
  if (!nvs_get_skip_boot_animation()) {
    _state->len = ASSET_BOOT_WEBP_LEN;
    ESP_LOGI(TAG, "calloc buff");
    _state->buf = calloc(1, ASSET_BOOT_WEBP_LEN);
    ESP_LOGI(TAG, "done calloc, copying");
    if (_state->buf == NULL) {
      ESP_LOGE("gfx", "Memory allocation failed!");
      return 1;
    }
    memcpy(_state->buf, ASSET_BOOT_WEBP, ASSET_BOOT_WEBP_LEN);
    ESP_LOGI(TAG, "done, copying");
  }

  _state->mutex = xSemaphoreCreateMutex();
  if (_state->mutex == NULL) {
    ESP_LOGE(TAG, "Could not create gfx mutex");
    return 1;
  }
  ESP_LOGI(TAG, "done with gfx init");

  // Initialize the display
  if (display_initialize()) {
    return 1;
  }

  if (nvs_get_skip_boot_animation()) {
    display_clear();
  }

  // Display version if not skipped
  if (!nvs_get_skip_display_version()) {
    // Display version and image_url for 1 second
    display_clear();
    char version_text[32];
    snprintf(version_text, sizeof(version_text), "v%s", FIRMWARE_VERSION);

    // Parse URL to extract host and last two path components
    if (img_url != NULL && strlen(img_url) > 0) {
      ESP_LOGI(TAG, "Full URL: %s", img_url);
      char host_only[64] = {0};
      char last_two_components[32] = {0};

      struct http_parser_url u;
      http_parser_url_init(&u);

      if (http_parser_parse_url(img_url, strlen(img_url), 0, &u) == 0) {
        if (u.field_set & (1 << UF_HOST)) {
          size_t host_len = u.field_data[UF_HOST].len;
          if (host_len >= sizeof(host_only)) host_len = sizeof(host_only) - 1;
          memcpy(host_only, img_url + u.field_data[UF_HOST].off, host_len);
          host_only[host_len] = '\0';
        }

        if (u.field_set & (1 << UF_PATH)) {
          const char *path = img_url + u.field_data[UF_PATH].off;
          size_t path_len = u.field_data[UF_PATH].len;
          const char *last_slash = NULL;
          const char *second_last_slash = NULL;

          for (size_t i = 0; i < path_len; i++) {
            if (path[i] == '/') {
              second_last_slash = last_slash;
              last_slash = path + i;
            }
          }

          if (second_last_slash != NULL) {
            size_t len = (path + path_len) - second_last_slash;
            if (len >= sizeof(last_two_components))
              len = sizeof(last_two_components) - 1;
            memcpy(last_two_components, second_last_slash, len);
            last_two_components[len] = '\0';
          } else {
            size_t len = path_len;
            if (len >= sizeof(last_two_components))
              len = sizeof(last_two_components) - 1;
            memcpy(last_two_components, path, len);
            last_two_components[len] = '\0';
          }
        }
      }

      // Display host at the top, left-aligned
      if (strlen(host_only) > 0) {
        ESP_LOGI(TAG, "Displaying host: '%s' at y=0", host_only);
        display_text(host_only, 0, 0, 255, 255, 255, 1);
      }

      // Display last 11 chars of path components in the middle, left-aligned
      if (strlen(last_two_components) > 0) {
        const char *display_path = last_two_components;
        size_t path_len = strlen(last_two_components);

        // If longer than 11 chars, show only the last 11
        if (path_len > 11) {
          display_path = last_two_components + (path_len - 11);
        }

        ESP_LOGI(TAG, "Displaying path components: '%s' at y=10", display_path);
        display_text(display_path, 0, 10, 255, 255, 255, 1);
      } else {
        ESP_LOGW(TAG, "No path components found to display");
      }
    }

    // Display 3 colored boxes RGB horizontally centered above version
    int box_x = (64 - 11) / 2;  // Center 11 pixels (3 boxes + 2 gaps)
    display_fill_rect(box_x, 20, 3, 3, 255, 0, 0);      // Red box
    display_fill_rect(box_x + 4, 20, 3, 3, 0, 255, 0);  // Green box
    display_fill_rect(box_x + 8, 20, 3, 3, 0, 0, 255);  // Blue box

    // Display version at the bottom, centered
    int text_width = strlen(version_text) * 6;
    int x = (64 - text_width) / 2;
    display_text(version_text, x, 24, 255, 255, 255, 1);

    // Flip the buffer once to show all three text lines at the same time
    display_flip();

    vTaskDelay(pdMS_TO_TICKS(2000));
  }

  // Launch the graphics loop in separate task
  BaseType_t ret =
      xTaskCreatePinnedToCore(gfx_loop,              // pvTaskCode
                              "gfx_loop",            // pcName
                              GFX_TASK_STACK_SIZE,   // usStackDepth
                              (void *)&isAnimating,  // pvParameters
                              GFX_TASK_PRIO,         // uxPriority
                              &_state->task,         // pxCreatedTask
                              GFX_TASK_CORE          // xCoreID
      );
  if (ret != pdPASS) {
    ESP_LOGE(TAG, "Could not create gfx task");
    return 1;
  }
  ESP_LOGI(TAG, "Gfx task created successfully");

  return 0;
}

void gfx_set_websocket_handle(esp_websocket_client_handle_t ws_handle) {
  if (_state) {
    _state->ws_handle = ws_handle;
    ESP_LOGI(TAG, "Websocket handle set for notifications");
  } else {
    ESP_LOGW(TAG, "Cannot set websocket handle - gfx not initialized");
  }
}

static void send_websocket_notification(int counter) {
  if (!_state || !_state->ws_handle) {
    // No websocket handle set, skip notification
    return;
  }

  if (!esp_websocket_client_is_connected(_state->ws_handle)) {
    ESP_LOGW(TAG, "Websocket not connected, skipping notification");
    return;
  }

  // Create JSON message: {"displaying": 42}
  char message[128];
  int len = snprintf(message, sizeof(message), "{\"displaying\":%d}", counter);

  if (len < 0 || len >= sizeof(message)) {
    ESP_LOGE(TAG, "Failed to format websocket notification message");
    return;
  }

  int sent = esp_websocket_client_send_text(_state->ws_handle, message, len,
                                            portMAX_DELAY);
  if (sent < 0) {
    ESP_LOGE(TAG, "Failed to send websocket notification");
  } else {
    ESP_LOGI(TAG, "WS send: %s", message);
  }
}

int gfx_update(void *webp, size_t len, int32_t dwell_secs) {
  return gfx_update_with_meta(webp, len, dwell_secs, NULL);
}

int gfx_update_with_meta(void *webp, size_t len, int32_t dwell_secs,
                         const struct app_meta *meta) {
  if (pdTRUE != xSemaphoreTake(_state->mutex, portMAX_DELAY)) {
    ESP_LOGE(TAG, "Could not take gfx mutex");
    return -1;  // Return negative on error
  }

#ifdef CONFIG_IMAGE_CACHE
  if (meta != NULL && meta->count > 0) {
    cache_store(webp, len, dwell_secs, meta);
    if (!cache_is_due(meta->index)) {
      // Shown from the cache while this download was pending: keep it as the
      // slot's fresh copy, but don't show the same app twice in a row.
      ESP_LOGI(TAG, "Image cache: id=%s (slot %d/%d) already shown, not queued",
               meta->id, (int)meta->index, (int)meta->count - 1);
      free(webp);
      xSemaphoreGive(_state->mutex);
      return GFX_UPDATE_CACHED_ONLY;
    }
  }
#endif

  // If a new frame arrives before the previous one is consumed by the gfx task,
  // free the old buffer here to prevent a memory leak (frame-dropping
  // strategy).
  if (_state->buf) {
    ESP_LOGW(TAG,
             "Dropping queued image (counter %d) - new image arrived before it "
             "was displayed",
             _state->counter);
    free(_state->buf);
    _state->buf = NULL;
  }

  // Take ownership of new buffer (no copy)
  _state->buf = webp;
  _state->len = len;
  _state->dwell_secs = dwell_secs;
  if (meta != NULL) {
    _state->meta = *meta;
  } else {
    memset(&_state->meta, 0, sizeof(_state->meta));
  }
  _state->counter++;
  int counter = _state->counter;
  ESP_LOGI(TAG, "Queued image counter=%d size=%zu dwell=%d", counter, len,
           dwell_secs);

  if (pdTRUE != xSemaphoreGive(_state->mutex)) {
    ESP_LOGE(TAG, "Could not give gfx mutex");
    return -1;  // Return negative on error
  }

  // Send "queued" notification immediately when image is queued
  if (_state->ws_handle &&
      esp_websocket_client_is_connected(_state->ws_handle)) {
    char message[64];
    int msg_len =
        snprintf(message, sizeof(message), "{\"queued\":%d}", counter);
    if (msg_len > 0 && msg_len < sizeof(message)) {
      esp_websocket_client_send_text(_state->ws_handle, message, msg_len,
                                     portMAX_DELAY);
      ESP_LOGI(TAG, "WS Send: %s", message);
    }
  }

  return counter;  // Return the counter value (>= 0) so caller can wait for it
                   // to be loaded
}

int gfx_get_loaded_counter(void) {
  if (!_state) return -1;

  if (pdTRUE != xSemaphoreTake(_state->mutex, portMAX_DELAY)) {
    ESP_LOGE(TAG, "Could not take gfx mutex");
    return -1;
  }

  int loaded = _state->loaded_counter;

  if (pdTRUE != xSemaphoreGive(_state->mutex)) {
    ESP_LOGE(TAG, "Could not give gfx mutex");
    return -1;
  }

  return loaded;
}

bool gfx_get_displaying(char *id, size_t id_len, int32_t *index,
                        uint32_t *elapsed_ms, int64_t *since_unix_ms) {
  if (!_state || id == NULL || id_len == 0) return false;
  if (pdTRUE != xSemaphoreTake(_state->mutex, portMAX_DELAY)) {
    ESP_LOGE(TAG, "Could not take gfx mutex");
    return false;
  }
  strlcpy(id, _state->displaying_id, id_len);
  if (index != NULL) *index = _state->displaying_index;
  if (elapsed_ms != NULL) {
    *elapsed_ms =
        (uint32_t)((esp_timer_get_time() - _state->displaying_since_us) / 1000);
  }
  if (since_unix_ms != NULL) *since_unix_ms = _state->displaying_since_unix_ms;
  xSemaphoreGive(_state->mutex);
  return id[0] != '\0';
}

int gfx_display_asset(const char *asset_type) {
  const uint8_t *asset_data = NULL;
  size_t asset_len = 0;

  // Determine which asset to display
  if (strcmp(asset_type, "config") == 0) {
    asset_data = ASSET_CONFIG_WEBP;
    asset_len = ASSET_CONFIG_WEBP_LEN;
  } else if (strcmp(asset_type, "error_404") == 0) {
    asset_data = ASSET_404_WEBP;
    asset_len = ASSET_404_WEBP_LEN;
  } else if (strcmp(asset_type, "no_connect") == 0) {
    asset_data = ASSET_NOCONNECT_WEBP;
    asset_len = ASSET_NOCONNECT_WEBP_LEN;
  } else if (strcmp(asset_type, "oversize") == 0) {
    ESP_LOGI(TAG, "DISPLAYING OVERSIZE GRAPHIC");
    asset_data = ASSET_OVERSIZE_WEBP;
    asset_len = ASSET_OVERSIZE_WEBP_LEN;
  } else {
    ESP_LOGE(TAG, "Unknown asset type: %s", asset_type);
    return 1;
  }

  // Allocate heap memory and copy asset data
  uint8_t *asset_heap_copy = (uint8_t *)malloc(asset_len);
  if (asset_heap_copy == NULL) {
    ESP_LOGE(TAG, "Failed to allocate memory for %s asset copy", asset_type);
    return 1;
  }

  memcpy(asset_heap_copy, asset_data, asset_len);

  // Interrupt current animation to display asset immediately
  isAnimating = -1;

  // Display the asset with no dwell time (static display)
  int result = gfx_update(asset_heap_copy, asset_len, 0);
  if (result < 0) {
    // Only free if gfx_update failed to take ownership (returned negative
    // error)
    ESP_LOGE(TAG, "Failed to update graphics with %s asset", asset_type);
    free(asset_heap_copy);
    return 1;
  }

  // gfx_update now owns the asset_heap_copy buffer (returns counter >= 0 on
  // success)
  return 0;
}

void gfx_display_text(const char *text, int x, int y, uint8_t r, uint8_t g,
                      uint8_t b, int scale) {
  display_text(text, x, y, r, g, b, scale);
}

void gfx_shutdown(void) { display_shutdown(); }

static void gfx_loop(void *args) {
  ESP_LOGI(TAG, "gfx_loop ENTERED");
  void *webp = NULL;
  size_t len = 0;
  int32_t dwell_secs = 0;
  int counter = -1;
  static struct app_meta meta;  // static: keeps it off the small task stack
  ESP_LOGI(TAG, "Graphics loop running on core %d", xPortGetCoreID());

  for (;;) {
    if (_state->paused) {
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    if (pdTRUE != xSemaphoreTake(_state->mutex, portMAX_DELAY)) {
      ESP_LOGE(TAG, "Could not take gfx mutex");
      if (webp) {
        free(webp);
        webp = NULL;
      }
      break;
    }

    bool loaded = false;

    // If there's new data, take ownership of buffer
    if (counter != _state->counter) {
      ESP_LOGI(TAG, "Displaying image counter=%d", _state->counter);
      if (webp) free(webp);
      webp = _state->buf;
      len = _state->len;
      dwell_secs = _state->dwell_secs;
      meta = _state->meta;
      strlcpy(_state->displaying_id, meta.count > 0 ? meta.id : "",
              sizeof(_state->displaying_id));
      _state->displaying_index = meta.count > 0 ? meta.index : -1;
      _state->displaying_since_us = esp_timer_get_time();
      _state->displaying_since_unix_ms = unix_ms_now();
      _state->buf = NULL;  // gfx_loop now owns the buffer
      counter = _state->counter;
      _state->loaded_counter = counter;  // Signal that we've loaded this image
      if (isAnimating == -1 && !_state->paused) isAnimating = 1;
      loaded = true;
#ifdef CONFIG_IMAGE_CACHE
      if (meta.count > 0 && meta.count == s_cache_count) {
        s_fresh = meta.index;
        s_cursor = meta.index;
        s_played = 0;
        for (int32_t i = 0; i < s_cache_count; i++) s_cache[i].shown = false;
        s_cache[meta.index].shown = true;
      }
#endif
    }
#ifdef CONFIG_IMAGE_CACHE
    else if (webp != NULL && meta.count > 0 && cache_complete()) {
      // Nothing new arrived during the last dwell time (download pending):
      // show the next cached app instead of repeating this one.
      int32_t next = cache_next_slot();
      if (next >= 0) {
        struct cache_slot *slot = &s_cache[next];
        uint8_t *copy =
            heap_caps_malloc(slot->len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (copy != NULL) {
          memcpy(copy, slot->buf, slot->len);
          free(webp);
          webp = copy;
          len = slot->len;
          dwell_secs = slot->dwell_secs;
          strlcpy(meta.id, slot->id, sizeof(meta.id));
          meta.index = next;
          s_cursor = next;
          s_played++;
          slot->shown = true;
          strlcpy(_state->displaying_id, meta.id,
                  sizeof(_state->displaying_id));
          _state->displaying_index = next;
          _state->displaying_since_us = esp_timer_get_time();
          _state->displaying_since_unix_ms = unix_ms_now();
          ESP_LOGI(TAG, "Image cache: download pending, showing id=%s from slot %d/%d",
                   meta.id, (int)next, (int)s_cache_count - 1);
        }
      }
    }
#endif

    if (pdTRUE != xSemaphoreGive(_state->mutex)) {
      ESP_LOGE(TAG, "Could not give gfx mutex");
      continue;
    }

    if (loaded) {
      // Send websocket notification that we're now displaying this image
      send_websocket_notification(counter);
    }

    static UBaseType_t last_stack_free = 0;
    UBaseType_t stack_free = uxTaskGetStackHighWaterMark(NULL);
    if (stack_free != last_stack_free) {
      ESP_LOGI(TAG, "Stack remaining: %u bytes", stack_free);
      last_stack_free = stack_free;
    }

    if (webp && len > 0) {
      if (draw_webp(webp, len, dwell_secs, &isAnimating)) {
        ESP_LOGE(TAG, "Could not draw webp");
        draw_error_indicator_pixel();
        vTaskDelay(pdMS_TO_TICKS(1 * 1000));
        isAnimating = 0;
        // Free the invalid buffer to prevent re-drawing it
        free(webp);
        webp = NULL;
        len = 0;
      }
      // keep webp around to loop until the next image arrives
    } else {
      vTaskDelay(pdMS_TO_TICKS(100));
    }
  }
}

static int draw_webp(const uint8_t *buf, size_t len, int32_t dwell_secs,
                     volatile int32_t *isAnimating) {
  // Set up WebP decoder
  // ESP_LOGI(TAG, "starting draw_webp");
  int app_dwell_secs = dwell_secs;

  int64_t dwell_us;

  if (app_dwell_secs <= 0) {
    ESP_LOGW(TAG, "dwell_secs is 0. Looping one more time while we wait.");
    dwell_us = 1 * 1000000;  // default to 1s if it's zero so we loop again or
                             // show the image for 1 more second.
  } else {
    // ESP_LOGI(TAG, "dwell_secs : %d", app_dwell_secs);
    dwell_us = app_dwell_secs * 1000000;
  }
  // ESP_LOGI(TAG, "frame count: %d", animation.frame_count);

  WebPData webpData;
  WebPDataInit(&webpData);
  webpData.bytes = buf;
  webpData.size = len;

  WebPAnimDecoderOptions decoderOptions;
  WebPAnimDecoderOptionsInit(&decoderOptions);
  decoderOptions.color_mode = MODE_RGBA;

  WebPAnimDecoder *decoder = WebPAnimDecoderNew(&webpData, &decoderOptions);
  if (decoder == NULL) {
    ESP_LOGE(TAG, "Could not create WebP decoder");
    draw_error_indicator_pixel();
    return 1;
  }

  WebPAnimInfo animation;
  if (!WebPAnimDecoderGetInfo(decoder, &animation)) {
    ESP_LOGE(TAG, "Could not get WebP animation");
    draw_error_indicator_pixel();
    WebPAnimDecoderDelete(decoder);  // Clean up decoder before returning
    return 1;
  }
  // ESP_LOGI(TAG, "frame count: %d", animation.frame_count);
  int64_t start_us = esp_timer_get_time();

  while (esp_timer_get_time() - start_us < dwell_us && *isAnimating != -1 && !_state->paused) {
    int64_t run_start_us = esp_timer_get_time();
    int lastTimestamp = 0;
    int delay = 0;
    TickType_t drawStartTick = xTaskGetTickCount();

    // Draw each frame, and sleep for the delay
    while (WebPAnimDecoderHasMoreFrames(decoder) && *isAnimating != -1 && !_state->paused) {
      uint8_t *pix;
      int timestamp;
      WebPAnimDecoderGetNext(decoder, &pix, &timestamp);
      if (delay > 0) {
        xTaskDelayUntil(&drawStartTick, pdMS_TO_TICKS(delay));
      } else {
        vTaskDelay(10);  // small delay for yield.
      }
      drawStartTick = xTaskGetTickCount();
      display_draw(pix, animation.canvas_width, animation.canvas_height, 4, 0,
                   1, 2);
      delay = timestamp - lastTimestamp;
      lastTimestamp = timestamp;
    }

    // reset decoder to start from the beginning
    WebPAnimDecoderReset(decoder);

    if (delay > 0) {
      xTaskDelayUntil(&drawStartTick, pdMS_TO_TICKS(delay));
    } else {
      vTaskDelay(
          pdMS_TO_TICKS(100));  // Add a small fallback delay to yield CPU
    }

    // In case of a single frame, sleep for app_dwell_secs
    if (animation.frame_count == 1) {
      // For static images, we need to check isAnimating periodically during the
      // dwell time Break the dwell time into 100ms chunks so we can respond to
      // immediate commands
      int64_t static_start_us = esp_timer_get_time();
      while (esp_timer_get_time() - static_start_us < dwell_us) {
        if (*isAnimating == -1 || _state->paused) {
          // Immediate command received, break out of dwell time
          break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));  // Check every 100ms
      }
      break;
    }

    // Animations always play at least once and are never cut off. Start
    // another run only if it fits completely in the remaining dwell time,
    // so a long animation does not overrun its dwell by almost a full run.
    int64_t now_us = esp_timer_get_time();
    int64_t run_us = now_us - run_start_us;
    if (now_us - start_us + run_us > dwell_us) {
      break;
    }
  }
  WebPAnimDecoderDelete(decoder);

  // ESP_LOGI(TAG, "Setting isAnimating to 0");
  if (*isAnimating != -1) {
    *isAnimating = 0;
  }
  return 0;
}

void gfx_stop(void) {
  if (_state) {
    isAnimating = -1;  // Signal current draw to stop
    _state->paused = true;
    ESP_LOGI(TAG, "Graphics loop paused");
  }
}

void gfx_start(void) {
  if (_state) {
    isAnimating = 0;
    _state->paused = false;
    ESP_LOGI(TAG, "Graphics loop resumed");
  }
}
