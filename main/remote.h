#pragma once

#include <stdbool.h>
#include <stdint.h>

// Position of an image in the server's app rotation, from the
// Tronbyt-App-Id / -Index / -Count headers. count is 0 when the server
// sent no (valid) position, e.g. for pushed images or older servers.
struct app_meta {
  char id[32];
  int32_t index;
  int32_t count;
};

// Retrieves url via HTTP GET. Caller is responsible for freeing buf,
// ota_url, image_url, and sound (if not NULL) on success.
int remote_get(const char* url, uint8_t** buf, size_t* len,
               uint8_t* brightness_pct, int32_t* dwell_secs, int* return_code,
               char** ota_url, char** image_url, bool* reboot_requested,
               char** sound, int16_t* volume, struct app_meta* app_meta);