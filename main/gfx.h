#pragma once

#include <esp_websocket_client.h>
#include <stdbool.h>
#include <stddef.h>

int gfx_initialize(const char* img_url);
void gfx_set_websocket_handle(esp_websocket_client_handle_t ws_handle);
struct app_meta;  // defined in remote.h

int gfx_update(void* webp, size_t len, int32_t dwell_secs);
// Returned by gfx_update_with_meta when the image was only stored in the
// image cache because it was already shown from there; nothing was queued.
#define GFX_UPDATE_CACHED_ONLY 0

// Like gfx_update, plus the image's position in the server rotation
// (meta may be NULL). Returns the queued counter (> 0),
// GFX_UPDATE_CACHED_ONLY, or a negative value on error.
int gfx_update_with_meta(void* webp, size_t len, int32_t dwell_secs,
                         const struct app_meta* meta);
int gfx_get_loaded_counter(void);
// The image on screen (fresh or cached): its server app ID, its position in
// the rotation, how long it has been shown and when it appeared in Unix ms
// (0 if the clock is not synced yet). Returns false if it has no ID
// (pushes, pinned apps, built-in screens).
bool gfx_get_displaying(char* id, size_t id_len, int32_t* index,
                        uint32_t* elapsed_ms, int64_t* since_unix_ms);
int gfx_display_asset(const char* asset_type);
void gfx_display_text(const char* text, int x, int y, uint8_t r, uint8_t g,
                      uint8_t b, int scale);
void gfx_stop(void);
void gfx_start(void);
void gfx_shutdown(void);
