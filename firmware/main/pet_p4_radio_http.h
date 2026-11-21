#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>

#define PET_RADIO_HEADER_MAX 4096
#define PET_RADIO_CHUNK_MAX (128u * 1024u)

typedef enum {
  PET_NETWORK_AUDIO_UNKNOWN,
  PET_NETWORK_AUDIO_MP3,
  PET_NETWORK_AUDIO_WAV,
} pet_network_audio_format_t;

/* The caller bounds the header and supplies one terminated line at a time. */
typedef struct {
  pet_network_audio_format_t format;
  uint32_t length;
  bool has_length;
  bool chunked;
  bool invalid;
} pet_radio_http_t;

static inline bool pet_radio_http_ok_status(const char *line) {
  return !strncmp(line, "HTTP/1.1 200 ", 13) || !strncmp(line, "HTTP/1.0 200 ", 13);
}

static inline void pet_radio_http_header(pet_radio_http_t *http, char *line) {
  char *value = strchr(line, ':');
  if (!value || value == line || *line == ' ' || *line == '\t') {
    http->invalid = true;
    return;
  }
  *value++ = 0;
  while (*value == ' ' || *value == '\t') ++value;
  char *end = value + strlen(value);
  while (end > value && (end[-1] == ' ' || end[-1] == '\t')) *--end = 0;
  if (!strcasecmp(line, "Content-Type")) {
    if (http->format) http->invalid = true;
    else if (!strcasecmp(value, "audio/mpeg")) http->format = PET_NETWORK_AUDIO_MP3;
    else if (!strcasecmp(value, "audio/wav") || !strcasecmp(value, "audio/x-wav"))
      http->format = PET_NETWORK_AUDIO_WAV;
    else http->invalid = true;
  } else if (!strcasecmp(line, "Transfer-Encoding")) {
    if (http->chunked || strcasecmp(value, "chunked")) http->invalid = true;
    http->chunked = true;
  } else if (!strcasecmp(line, "Content-Length")) {
    if (http->has_length || !*value) http->invalid = true;
    http->has_length = true;
    for (; *value; ++value) {
      unsigned digit = (unsigned char)*value - '0';
      if (digit > 9 || http->length > (UINT32_MAX - digit) / 10) {
        http->invalid = true;
        break;
      }
      http->length = http->length * 10 + digit;
    }
  } else if (!strcasecmp(line, "Content-Encoding")) {
    if (strcasecmp(value, "identity")) http->invalid = true;
  } else if (!strcasecmp(line, "icy-metaint")) {
    http->invalid = true;
  }
}

static inline bool pet_radio_http_chunk_size(const char *line, unsigned *size) {
  unsigned n = 0;
  if (!*line) return false;
  for (; *line; ++line) {
    unsigned c = (unsigned char)*line;
    unsigned digit;
    if (c >= '0' && c <= '9') digit = c - '0';
    else if ((c | 32u) >= 'a' && (c | 32u) <= 'f') digit = (c | 32u) - 'a' + 10u;
    else return false;
    if (digit >= 16u || n > (PET_RADIO_CHUNK_MAX - digit) / 16u) return false;
    n = n * 16u + digit;
  }
  *size = n;
  return true;
}
