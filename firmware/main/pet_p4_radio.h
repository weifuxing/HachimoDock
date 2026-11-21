#pragma once

#if PET_P4_SYSTEM_WIFI
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "pet_p4_wifi_endpoint.h"

#define PET_RADIO_SOURCE "pc.city-radio"
#define PET_RADIO_OFFLINE_COUNT 12

typedef struct {
  const char *id;
  const char *title;
  const char *channel;
} pet_radio_station_t;

typedef enum {
  PET_RADIO_COMPLETE,
  PET_RADIO_SOURCE_FAILED,
  PET_RADIO_CLOCK_UNAVAILABLE,
  PET_RADIO_CONNECT_FAILED,
  PET_RADIO_WIFI_DISCONNECTED,
  PET_RADIO_REQUEST_FAILED,
  PET_RADIO_RESPONSE_FAILED,
  PET_RADIO_FORMAT_UNSUPPORTED,
  PET_RADIO_OUT_OF_MEMORY,
} pet_radio_error_t;

typedef enum {
  PET_RADIO_WRITTEN,
  PET_RADIO_BACKPRESSURE,
  PET_RADIO_CANCELLED,
} pet_radio_write_t;

/* Immutable firmware-owned endpoint. Paths are copied into the worker queue. */
typedef struct {
  const char *host;
  const uint8_t *ca;
  size_t ca_bytes;
} pet_network_audio_endpoint_t;

bool pet_network_audio_submit(const pet_network_audio_endpoint_t *endpoint, const char *path,
                              bool live_stream, uint32_t generation);
bool pet_network_audio_submit_public(const pet_wifi_endpoint_t *endpoint, bool live_stream,
                                     uint32_t generation);
const pet_radio_station_t *pet_radio_station_at(unsigned index);
const pet_radio_station_t *pet_radio_station_find(const char *id);
bool pet_radio_submit(const pet_radio_station_t *station, uint32_t generation);

/* Media owns authorization, generation, view and the audio session. */
bool pet_p4_media_radio_current(uint32_t generation);
bool pet_p4_media_radio_reconnecting(uint32_t generation);
pet_radio_write_t pet_p4_media_radio_write(uint32_t generation, const int16_t *pcm, size_t samples);
void pet_p4_media_radio_finished(uint32_t generation, pet_radio_error_t result);
#endif
