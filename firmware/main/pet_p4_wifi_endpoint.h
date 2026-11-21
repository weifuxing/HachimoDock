#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct {
  char host[128];
  char path[256];
} pet_wifi_endpoint_t;

static inline bool pet_wifi_hostname_letter(unsigned char c) {
  c |= 32;
  return c >= 'a' && c <= 'z';
}

/* The transport supports public HTTPS DNS names on port 443. The endpoint
 * is a complete GET target, with no authority overrides or credentials. */
static inline bool pet_wifi_endpoint_parse(const char *url, pet_wifi_endpoint_t *out) {
  static const char scheme[] = "https://";
  if (!url || strncmp(url, scheme, sizeof(scheme) - 1)) return false;
  const char *host = url + sizeof(scheme) - 1;
  size_t size = 0, label = 0, dots = 0;
  bool tld_has_letter = false;
  while (host[size] && host[size] != '/' && host[size] != '?') {
    const unsigned char c = host[size];
    if (size >= sizeof(out->host) - 1) return false;
    if (c == '.') {
      if (!label || host[size - 1] == '-') return false;
      dots++;
      label = 0;
      tld_has_letter = false;
    } else {
      const bool letter = pet_wifi_hostname_letter(c);
      if (!(letter || (c >= '0' && c <= '9') || (c == '-' && label))) return false;
      if (++label > 63) return false;
      tld_has_letter |= letter;
    }
    size++;
  }
  if (!dots || !label || !tld_has_letter || host[size - 1] == '-') return false;
  const char *target = host + size;
  const bool prefix_slash = *target != '/';
  size_t length = 0;
  while (target[length]) {
    const unsigned char c = target[length];
    if (length + prefix_slash >= sizeof(out->path) - 1 ||
        c <= ' ' || c > '~' || c == '#' || c == '\\') return false;
    length++;
  }
  if (!prefix_slash && target[1] == '/') return false;
  if (out) {
    for (size_t i = 0; i < size; ++i)
      out->host[i] = pet_wifi_hostname_letter(host[i]) ? host[i] | 32 : host[i];
    out->host[size] = 0;
    if (prefix_slash) out->path[0] = '/';
    memcpy(out->path + prefix_slash, target, length + 1);
  }
  return true;
}

/* Host byte order. Exclude private, special-use and non-unicast addresses.
 * After validation connect to this exact address, retaining hostname TLS. */
static inline bool pet_wifi_endpoint_public_ipv4(uint32_t ip) {
  const unsigned a = ip >> 24, b = (ip >> 16) & 255, c = (ip >> 8) & 255;
  if (a == 0 || a == 10 || a == 127 || a >= 224 ||
      (a == 100 && b >= 64 && b <= 127) || (a == 169 && b == 254) ||
      (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168) ||
      (a == 192 && b == 0 && (c == 0 || c == 2)) ||
      (a == 192 && b == 88 && c == 99) ||
      (a == 198 && (b == 18 || b == 19 || (b == 51 && c == 100))) ||
      (a == 203 && b == 0 && c == 113)) return false;
  return true;
}
