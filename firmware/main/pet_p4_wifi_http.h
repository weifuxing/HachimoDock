#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>

/* Public GET endpoints, with bounded HTTP/1.x response framing. */
typedef bool (*pet_wifi_http_read_t)(void *ctx, char *out, size_t bytes);

static bool pet_wifi_http_line(pet_wifi_http_read_t read, void *ctx, char *line,
                               size_t capacity, size_t *budget) {
  for (size_t i = 0; i < capacity && *budget; i++) {
    char c;
    if (!read(ctx, &c, 1)) return false;
    --*budget;
    if (c == '\r') {
      if (!*budget || !read(ctx, &c, 1) || c != '\n') return false;
      --*budget;
      line[i] = 0;
      return true;
    }
    if ((unsigned char)c < 32 && c != '\t') return false;
    line[i] = c;
  }
  return false;
}

static bool pet_wifi_http_size(const char *s, unsigned base, size_t limit, size_t *size) {
  size_t n = 0;
  if (!*s) return false;
  for (; *s; s++) {
    unsigned c = (unsigned char)*s;
    unsigned digit;
    if (c >= '0' && c <= '9') digit = c - '0';
    else if (base == 16 && (c | 32U) >= 'a' && (c | 32U) <= 'f') digit = (c | 32U) - 'a' + 10;
    else return false;
    if (digit > limit || n > (limit - digit) / base) return false;
    n = n * base + digit;
  }
  *size = n;
  return true;
}

#define PET_WIFI_HTTP_ETAG_SIZE 96

typedef struct {
  char etag[PET_WIFI_HTTP_ETAG_SIZE];
  bool no_store;
} pet_wifi_http_metadata_t;

static inline bool pet_wifi_http_valid_etag(const char *tag) {
  if (strlen(tag) >= PET_WIFI_HTTP_ETAG_SIZE) return false;
  if (!strncmp(tag, "W/", 2)) tag += 2;
  if (*tag++ != '"') return false;
  while (*tag && *tag != '"') {
    if ((unsigned char)*tag < 0x21 || (unsigned char)*tag > 0x7e) return false;
    tag++;
  }
  return tag[0] == '"' && !tag[1];
}

static inline bool pet_wifi_http_no_store(const char *value) {
  /* Treat a directive name conservatively even when its arguments are invalid. */
  while (*value) {
    value += strspn(value, " ,\t");
    size_t length = strcspn(value, " ,\t=");
    if ((length == 8 && !strncasecmp(value, "no-store", 8)) ||
        (length == 7 && !strncasecmp(value, "private", 7))) return true;
    value += strcspn(value, ",");
  }
  return false;
}

static inline bool pet_wifi_http_response(pet_wifi_http_read_t read, void *ctx, char *body,
                                         size_t limit, size_t *length, const char *sent_etag,
                                         pet_wifi_http_metadata_t *metadata) {
  char line[1024];
  size_t budget = 4096, content_length = 0;
  bool chunked = false, has_length = false, has_encoding = false, has_etag = false;
  if (!pet_wifi_http_line(read, ctx, line, sizeof(line), &budget)
      || (strncmp(line, "HTTP/1.1 ", 9) && strncmp(line, "HTTP/1.0 ", 9))) return false;
  const bool unchanged = !strncmp(line + 9, "304 ", 4);
  if (unchanged ? !sent_etag : strncmp(line + 9, "200 ", 4) != 0) return false;
  if (metadata) *metadata = (pet_wifi_http_metadata_t){0};

  for (;;) {
    if (!pet_wifi_http_line(read, ctx, line, sizeof(line), &budget)) return false;
    if (!*line) break;
    char *value = strchr(line, ':');
    if (!value || value == line || *line == ' ' || *line == '\t' || value[-1] == ' ' || value[-1] == '\t')
      return false;
    *value++ = 0;
    value += strspn(value, " \t");
    char *end = value + strlen(value);
    while (end > value && (end[-1] == ' ' || end[-1] == '\t')) *--end = 0;
    if (!strcasecmp(line, "Content-Length")) {
      if (has_length || !pet_wifi_http_size(value, 10, unchanged ? SIZE_MAX : limit, &content_length)) return false;
      has_length = true;
    } else if (!strcasecmp(line, "Transfer-Encoding")) {
      if (chunked || strcasecmp(value, "chunked")) return false;
      chunked = true;
    } else if (!strcasecmp(line, "Content-Encoding")) {
      if (has_encoding || strcasecmp(value, "identity")) return false;
      has_encoding = true;
    } else if (metadata && !strcasecmp(line, "ETag")) {
      if (has_etag || !pet_wifi_http_valid_etag(value)) metadata->etag[0] = 0;
      else strcpy(metadata->etag, value);
      has_etag = true;
    } else if (metadata && !strcasecmp(line, "Cache-Control")) {
      metadata->no_store |= pet_wifi_http_no_store(value);
    }
  }
  if (unchanged) {
    if (chunked || (has_etag && (!metadata || !metadata->etag[0] ||
        strcmp(sent_etag + (!strncmp(sent_etag, "W/", 2) ? 2 : 0),
               metadata->etag + (!strncmp(metadata->etag, "W/", 2) ? 2 : 0))))) return false;
    *length = 0;
    return true;
  }
  if (chunked == has_length) return false;

  size_t used = 0;
  if (has_length) {
    if (!read(ctx, body, content_length)) return false;
    used = content_length;
  } else {
    /* Bound aggregate framing as well as payload, including tiny chunks. */
    budget = 8192;
    for (;;) {
      size_t chunk;
      if (!pet_wifi_http_line(read, ctx, line, sizeof(line), &budget)) return false;
      char *extension = strchr(line, ';');
      if (extension) *extension = 0;
      if (!pet_wifi_http_size(line, 16, limit - used, &chunk)) return false;
      if (!chunk) break;
      char delimiter[2];
      if (!read(ctx, body + used, chunk) || budget < 2 || !read(ctx, delimiter, sizeof(delimiter))
          || delimiter[0] != '\r' || delimiter[1] != '\n') return false;
      budget -= 2;
      used += chunk;
    }
    do {
      if (!pet_wifi_http_line(read, ctx, line, sizeof(line), &budget)) return false;
    } while (*line);
  }
  if (!used || memchr(body, 0, used)) return false;
  body[used] = 0; /* Caller reserves limit + 1 bytes. */
  *length = used;
  return true;
}


static inline bool pet_wifi_http_body(pet_wifi_http_read_t read, void *ctx, char *body,
                                      size_t limit, size_t *length) {
  return pet_wifi_http_response(read, ctx, body, limit, length, NULL, NULL);
}
