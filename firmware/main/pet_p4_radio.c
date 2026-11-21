#include "pet_p4_radio.h"

#if PET_P4_SYSTEM_WIFI
#include "pet_p4_device_wifi.h"
#include "pet_p4_live_tls.h"
#include "pet_p4_radio_decode.h"
#include "pet_p4_radio_http.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_tls.h"
#include "esp_tls_errors.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <stdio.h>
#include <time.h>

#define RADIO_HOST "lhttp.qingting.fm"
#define RADIO_CONNECT_MS 6000
#define RADIO_PROGRESS_MS 6000
#define RADIO_WIFI_RECOVERY_MS 30000
#define RADIO_FINITE_MAX_BYTES (256u * 1024u * 1024u)
#define RADIO_RECEIVE_BYTES (32 * 1024)
#define RADIO_INPUT_BYTES 4096
#define RADIO_DECODE_LOOKAHEAD 2048
#define RADIO_INVALID_BYTES (64u * 1024u)

#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_SIMD
#define MINIMP3_IMPLEMENTATION
#include "radio_vendor/minimp3.h"

static const uint8_t radio_root_ca[] = {
#include "pet_p4_radio_root.inc"
};
static const pet_network_audio_endpoint_t qingting = {
  RADIO_HOST, radio_root_ca, sizeof(radio_root_ca),
};

static const pet_radio_station_t stations[] = {
#include "pet_p4_radio_catalog.inc"
};

const pet_radio_station_t *pet_radio_station_at(unsigned index) {
  return index < sizeof(stations) / sizeof(stations[0]) ? &stations[index] : NULL;
}

const pet_radio_station_t *pet_radio_station_find(const char *id) {
  for (unsigned i = 0; i < sizeof(stations) / sizeof(stations[0]); ++i)
    if (!strcmp(stations[i].id, id)) return &stations[i];
  return NULL;
}

typedef struct {
  const pet_network_audio_endpoint_t *endpoint;
  pet_wifi_endpoint_t public_endpoint;
  char path[160];
  uint32_t generation;
  bool live_stream;
} radio_job_t;

typedef struct {
  esp_tls_t *tls;
  uint32_t generation;
  pet_radio_error_t failure;
  int64_t deadline;
  bool chunked, chunk_crlf, received_http, io_interrupted;
  bool bounded, body_complete, delivered, wifi_lost;
  uint32_t remaining, body_bytes, body_limit;
  unsigned chunk_remaining;
  pet_network_audio_format_t format;
  size_t pos, received;
  bool receive_ended;
  int receive_error;
  uint8_t receive[RADIO_RECEIVE_BYTES];
  char line[PET_RADIO_HEADER_MAX];
  uint8_t input[RADIO_INPUT_BYTES];
  mp3dec_t decoder;
  int16_t decoded[MINIMP3_MAX_SAMPLES_PER_FRAME];
  int16_t pcm[PET_RADIO_PCM_MAX];
  pet_radio_resampler_t resampler;
} radio_stream_t;

static QueueHandle_t jobs;

static int64_t now_ms(void) {
  return esp_timer_get_time() / 1000;
}

static bool live(radio_stream_t *s) {
  if (!pet_device_wifi_online()) s->wifi_lost = true;
  return !s->wifi_lost && pet_p4_media_radio_current(s->generation);
}

static bool live_connection(void *context) {
  return live(context);
}

static void poll_delay(void) {
  vTaskDelay(pdMS_TO_TICKS(10) + 1);
}

static bool waitable(ssize_t result) {
  return result == ESP_TLS_ERR_SSL_WANT_READ || result == ESP_TLS_ERR_SSL_WANT_WRITE;
}

/* Retain unread HTTP bytes while the PCM output is full. The stream lives
 * in PSRAM and only the radio worker reads or writes it. */
static bool receive_available(radio_stream_t *s) {
  if (s->receive_ended) return false;
  if (s->pos == s->received || s->pos >= sizeof(s->receive) / 2) {
    s->received -= s->pos;
    memmove(s->receive, s->receive + s->pos, s->received);
    s->pos = 0;
  }
  if (s->received == sizeof(s->receive)) return false;

  ssize_t n = esp_tls_conn_read(s->tls, s->receive + s->received,
                                sizeof(s->receive) - s->received);
  if (n > 0) {
    if (!s->received_http) {
      s->received_http = true;
      pet_device_wifi_note_https(PET_WIFI_HTTPS_READY, 0);
    }
    s->received += (size_t)n;
    return true;
  }
  if (!waitable(n)) {
    s->receive_ended = true;
    s->receive_error = (int)n;
  }
  return false;
}

static bool fill(radio_stream_t *s) {
  if (s->pos < s->received) return true;
  while (!s->receive_ended && now_ms() < s->deadline && live(s)) {
    if (receive_available(s)) return true;
    if (s->receive_ended) break;
    poll_delay();
  }
  if (live(s)) {
    s->io_interrupted = true;
    pet_device_wifi_note_https(PET_WIFI_HTTPS_IO, s->receive_error);
  }
  return false;
}

static int read_byte(radio_stream_t *s) {
  return fill(s) ? s->receive[s->pos++] : -1;
}

static int read_line(radio_stream_t *s, size_t limit) {
  for (size_t i = 0; i < limit; ++i) {
    int byte = read_byte(s);
    if (byte == '\r') {
      if (read_byte(s) != '\n') return -1;
      s->line[i] = 0;
      return (int)i;
    }
    if (byte < 32 && byte != '\t') return -1;
    s->line[i] = (char)byte;
  }
  return -1;
}

#if PET_P4_SYSTEM_WIFI && defined(__GNUC__) && !defined(__clang__)
__attribute__((noinline, optimize("Oz")))
#endif
static bool connect_stream(radio_stream_t *s, const radio_job_t *job) {
  /* TLS uses the system UTC clock set by SNTP. No credentials or redirects. */
  s->failure = PET_RADIO_CLOCK_UNAVAILABLE;
  const int64_t clock_deadline = now_ms() + RADIO_CONNECT_MS;
  while (time(NULL) < 1577836800 && live(s) && now_ms() < clock_deadline) poll_delay();
  if (!live(s) || time(NULL) < 1577836800) return false;
  const esp_tls_cfg_t config = {
    .non_block = true,
    .timeout_ms = RADIO_CONNECT_MS,
    .cacert_buf = job->endpoint ? job->endpoint->ca : NULL,
    .cacert_bytes = job->endpoint ? job->endpoint->ca_bytes : 0,
  };
  s->failure = PET_RADIO_CONNECT_FAILED;
  const int64_t deadline = now_ms() + RADIO_CONNECT_MS * 2 + 300;
  const char *host = job->public_endpoint.host;
  const char *path = job->public_endpoint.path;
  if (job->endpoint) {
    host = job->endpoint->host;
    path = job->path;
    s->tls = pet_live_tls_connect(host, &config, deadline, live_connection, s);
  } else {
#ifdef ESP_PLATFORM
    s->tls = pet_live_tls_connect_public(host, &config, deadline, live_connection, s);
#endif
  }
  if (!s->tls) return false;

  s->failure = PET_RADIO_REQUEST_FAILED;
  char request[544];
  int len = snprintf(request, sizeof(request),
                     "GET %s HTTP/1.1\r\nHost: %s\r\nAccept: %s\r\nAccept-Encoding: identity\r\n"
                     "Icy-MetaData: 0\r\nConnection: close\r\n\r\n",
                     path, host, job->live_stream ? "audio/mpeg" : "audio/mpeg, audio/wav, audio/x-wav");
  if (len < 0 || len >= (int)sizeof(request)) return false;
  s->deadline = now_ms() + RADIO_PROGRESS_MS;
  int sent = 0;
  while (sent < len && now_ms() < s->deadline && live(s)) {
    ssize_t n = esp_tls_conn_write(s->tls, request + sent, len - sent);
    if (n > 0) sent += (int)n;
    else if (waitable(n)) poll_delay();
    else return false;
  }
  return sent == len;
}

#if PET_P4_SYSTEM_WIFI && defined(__GNUC__) && !defined(__clang__)
__attribute__((noinline, optimize("Oz")))
#endif
static bool read_headers(radio_stream_t *s, bool live_stream, pet_radio_error_t *error) {
  int n = read_line(s, sizeof(s->line) - 2);
  if (n < 0 || !pet_radio_http_ok_status(s->line)) return false;
  size_t remaining = sizeof(s->line) - (size_t)n - 2;
  pet_radio_http_t headers = {0};
  while (remaining >= 2) {
    n = read_line(s, remaining - 1);
    if (n < 0) return false;
    if (!n) {
      if (headers.chunked && headers.has_length) return false;
      s->chunked = headers.chunked;
      s->bounded = headers.has_length;
      s->remaining = headers.length;
      s->format = headers.format;
      s->body_limit = live_stream ? 0 : RADIO_FINITE_MAX_BYTES;
      if (s->body_limit && s->remaining > s->body_limit) return false;
      if (!headers.invalid && headers.format &&
          (live_stream ? headers.format == PET_NETWORK_AUDIO_MP3 : (headers.has_length || headers.chunked)))
        return true;
      *error = PET_RADIO_FORMAT_UNSUPPORTED;
      return false;
    }
    remaining -= (size_t)n + 2;
    pet_radio_http_header(&headers, s->line);
  }
  return false;
}

static size_t read_body(radio_stream_t *s, uint8_t *out, size_t capacity) {
  if (s->body_complete) return 0;
  if (s->bounded && !s->remaining) {
    s->body_complete = true;
    return 0;
  }
  if (s->chunked && !s->chunk_remaining) {
    if (s->chunk_crlf && (read_byte(s) != '\r' || read_byte(s) != '\n')) return 0;
    if (read_line(s, 16) < 0 || !pet_radio_http_chunk_size(s->line, &s->chunk_remaining)) return 0;
    if (!s->chunk_remaining) {
      s->body_complete = read_line(s, 2) == 0;
      return 0;
    }
    s->chunk_crlf = true;
  }
  if (!fill(s)) return 0;
  size_t n = s->received - s->pos;
  if (n > capacity) n = capacity;
  if (s->bounded) {
    if (n > s->remaining) n = s->remaining;
    s->remaining -= n;
    s->body_complete = s->remaining == 0;
  }
  if (s->chunked) {
    if (n > s->chunk_remaining) n = s->chunk_remaining;
    s->chunk_remaining -= n;
  }
  if (s->body_limit) {
    if (n > s->body_limit - s->body_bytes) {
      s->body_complete = false;
      return 0;
    }
    s->body_bytes += n;
  }
  memcpy(out, s->receive + s->pos, n);
  s->pos += n;
  return n;
}

static bool deliver(radio_stream_t *s, size_t samples) {
  while (live(s)) {
    pet_radio_write_t result = pet_p4_media_radio_write(s->generation, s->pcm, samples);
    if (result == PET_RADIO_WRITTEN) {
      s->delivered = true;
      s->deadline = now_ms() + RADIO_PROGRESS_MS;
      return true;
    }
    if (result == PET_RADIO_CANCELLED) return false;
    if (now_ms() >= s->deadline) return false;
    receive_available(s);
    poll_delay();
  }
  return false;
}

static pet_radio_error_t decode_mp3(radio_stream_t *s, bool live_stream) {
  size_t buffered = 0, invalid = 0;
  mp3dec_init(&s->decoder);
  s->deadline = now_ms() + RADIO_PROGRESS_MS;
  while (live(s) && now_ms() < s->deadline) {
    while (buffered < RADIO_DECODE_LOOKAHEAD && !s->body_complete) {
      size_t bytes = read_body(s, s->input + buffered, sizeof(s->input) - buffered);
      if (!bytes && !s->body_complete) return PET_RADIO_SOURCE_FAILED;
      buffered += bytes;
    }
    if (!buffered) break;

    mp3dec_frame_info_t info = {0};
    int frames = mp3dec_decode_frame(&s->decoder, s->input, (int)buffered, s->decoded, &info);
    /* Retain a possible partial frame at the edge of unsynchronized data. */
    size_t consumed = info.frame_bytes > 0 && info.frame_offset < info.frame_bytes
      ? (size_t)info.frame_bytes : buffered < RADIO_DECODE_LOOKAHEAD ? buffered : buffered - RADIO_DECODE_LOOKAHEAD + 512;
    size_t samples = frames > 0 && info.layer == 3
      ? pet_radio_resample(&s->resampler, s->decoded, frames, info.channels, info.hz, s->pcm) : 0;
    if (samples) {
      if (!deliver(s, samples)) return PET_RADIO_SOURCE_FAILED;
      invalid = 0;
    } else if ((invalid += consumed) >= RADIO_INVALID_BYTES) {
      return PET_RADIO_FORMAT_UNSUPPORTED;
    }
    buffered -= consumed;
    memmove(s->input, s->input + consumed, buffered);
  }
  return !live_stream && s->body_complete && !buffered && s->delivered
    ? PET_RADIO_COMPLETE : PET_RADIO_SOURCE_FAILED;
}

static uint32_t wav_u32(const uint8_t *p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static unsigned wav_u16(const uint8_t *p) {
  return (unsigned)p[0] | (unsigned)p[1] << 8;
}

static bool read_exact(radio_stream_t *s, uint8_t *out, size_t size) {
  while (size && live(s)) {
    size_t n = read_body(s, out, size);
    if (!n) return false;
    out += n;
    size -= n;
  }
  return size == 0;
}

static bool skip_wav_metadata(radio_stream_t *s, uint32_t size) {
  while (size) {
    size_t n = size < sizeof(s->input) ? size : sizeof(s->input);
    if (!read_exact(s, s->input, n)) return false;
    size -= n;
  }
  return true;
}

static pet_radio_error_t decode_wav(radio_stream_t *s) {
  uint8_t header[16];
  if (!read_exact(s, header, 12)) return PET_RADIO_SOURCE_FAILED;
  uint32_t riff_size = wav_u32(header + 4);
  if (memcmp(header, "RIFF", 4) || memcmp(header + 8, "WAVE", 4) || riff_size < 4)
    return PET_RADIO_FORMAT_UNSUPPORTED;
  uint32_t remaining = riff_size - 4, metadata = 0;
  if (s->body_limit && remaining > s->body_limit - 12) return PET_RADIO_FORMAT_UNSUPPORTED;
  if (s->bounded && s->remaining != remaining) return PET_RADIO_SOURCE_FAILED;
  unsigned channels = 0, rate = 0;
  while (remaining >= 8 && !s->delivered) {
    if (!read_exact(s, header, 8)) return PET_RADIO_SOURCE_FAILED;
    remaining -= 8;
    uint32_t size = wav_u32(header + 4);
    if (size > remaining || (size == remaining && (size & 1u)))
      return PET_RADIO_FORMAT_UNSUPPORTED;
    remaining -= size + (size & 1u);
    if (!memcmp(header, "data", 4)) {
      unsigned frame_size = channels * 2;
      if (!frame_size || !size || size % frame_size) return PET_RADIO_FORMAT_UNSUPPORTED;
      while (size) {
        unsigned frames = size / frame_size;
        if (frames > 512) frames = 512;
        unsigned bytes = frames * frame_size;
        if (!read_exact(s, s->input, bytes)) return PET_RADIO_SOURCE_FAILED;
        for (unsigned i = 0; i < frames * channels; ++i)
          s->decoded[i] = (int16_t)wav_u16(s->input + i * 2);
        size_t samples = pet_radio_resample(&s->resampler, s->decoded, frames, channels, rate, s->pcm);
        if (!deliver(s, samples)) return PET_RADIO_SOURCE_FAILED;
        size -= bytes;
      }
      break;
    }
    if (size > 65536 - metadata) return PET_RADIO_FORMAT_UNSUPPORTED;
    metadata += size;
    uint32_t skip = size + (size & 1u);
    if (!memcmp(header, "fmt ", 4)) {
      if (channels || size < sizeof(header)) return PET_RADIO_FORMAT_UNSUPPORTED;
      if (!read_exact(s, header, sizeof(header))) return PET_RADIO_SOURCE_FAILED;
      channels = wav_u16(header + 2);
      rate = wav_u32(header + 4);
      if (wav_u16(header) != 1 || (channels != 1 && channels != 2) ||
          rate < 8000 || rate > PET_RADIO_PCM_RATE || wav_u16(header + 14) != 16 ||
          wav_u16(header + 12) != channels * 2 || wav_u32(header + 8) != rate * channels * 2)
        return PET_RADIO_FORMAT_UNSUPPORTED;
      skip -= sizeof(header);
    }
    if (!skip_wav_metadata(s, skip)) return PET_RADIO_SOURCE_FAILED;
  }
  if (!s->delivered || remaining > 65536 - metadata) return PET_RADIO_FORMAT_UNSUPPORTED;
  if (!skip_wav_metadata(s, remaining)) return PET_RADIO_SOURCE_FAILED;
  return !read_body(s, header, 1) && s->body_complete ? PET_RADIO_COMPLETE : PET_RADIO_SOURCE_FAILED;
}

#if PET_P4_SYSTEM_WIFI && defined(__GNUC__) && !defined(__clang__)
__attribute__((noinline, optimize("Oz")))
#endif
static bool wait_for_wifi(uint32_t generation) {
  const int64_t deadline = now_ms() + RADIO_WIFI_RECOVERY_MS;
  do {
    vTaskDelay(pdMS_TO_TICKS(500) + 1);
    if (!pet_p4_media_radio_current(generation)) return false;
    if (pet_device_wifi_online()) return true;
  } while (now_ms() < deadline);
  return false;
}

static void radio_task(void *arg) {
  (void)arg;
  radio_job_t job;
  while (xQueueReceive(jobs, &job, portMAX_DELAY) == pdTRUE) {
    if (!pet_p4_media_radio_current(job.generation)) continue;
    pet_radio_error_t error = PET_RADIO_OUT_OF_MEMORY;
    radio_stream_t *s = heap_caps_calloc(1, sizeof(*s), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s) {
      for (unsigned attempt = 0; attempt < 2; ++attempt) {
        s->generation = job.generation;
        if (!connect_stream(s, &job)) {
          error = s->failure;
        } else {
          error = PET_RADIO_RESPONSE_FAILED;
          if (read_headers(s, job.live_stream, &error))
            error = s->format == PET_NETWORK_AUDIO_WAV ? decode_wav(s) : decode_mp3(s, job.live_stream);
        }
        if (s->tls) esp_tls_conn_destroy(s->tls);

        const bool offline = s->wifi_lost || !pet_device_wifi_online();
        const bool interrupted = s->io_interrupted ||
          (job.live_stream && s->delivered && s->body_complete);
        if (error == PET_RADIO_COMPLETE || error == PET_RADIO_FORMAT_UNSUPPORTED) break;
        if (offline) error = PET_RADIO_WIFI_DISCONNECTED;
        if (attempt || (!offline && !interrupted) || (!job.live_stream && (offline || s->delivered)))
          break;
        if (job.live_stream && !pet_p4_media_radio_reconnecting(job.generation)) break;
        if (!wait_for_wifi(job.generation)) {
          if (!pet_device_wifi_online()) error = PET_RADIO_WIFI_DISCONNECTED;
          break;
        }
        memset(s, 0, sizeof(*s));
      }
      heap_caps_free(s);
    }
    pet_p4_media_radio_finished(job.generation, error);
  }
  vTaskDelete(NULL);
}

/* Called under the media lock. One worker and one replaceable pending request. */
static bool submit(const radio_job_t *job) {
  if (!jobs) {
    jobs = xQueueCreate(1, sizeof(radio_job_t));
    if (!jobs) return false;
    if (xTaskCreate(radio_task, "city_radio", 20 * 1024, NULL, 4, NULL) != pdPASS) {
      vQueueDelete(jobs);
      jobs = NULL;
      return false;
    }
  }
  return xQueueOverwrite(jobs, job) == pdTRUE;
}

bool pet_network_audio_submit(const pet_network_audio_endpoint_t *endpoint, const char *path,
                              bool live_stream, uint32_t generation) {
  radio_job_t job = {.endpoint = endpoint, .generation = generation, .live_stream = live_stream};
  if (!endpoint || !endpoint->host || !endpoint->ca_bytes || !endpoint->ca ||
      !path || path[0] != '/' || path[1] == '/' || !generation) return false;
  size_t length = 0;
  while (path[length]) {
    unsigned char c = path[length];
    if (length >= sizeof(job.path) - 1 || c <= ' ' || c > '~' || c == '#' || c == '\\') return false;
    job.path[length++] = (char)c;
  }
  return submit(&job);
}

bool pet_network_audio_submit_public(const pet_wifi_endpoint_t *endpoint, bool live_stream,
                                     uint32_t generation) {
  if (!endpoint || !endpoint->host[0] || endpoint->path[0] != '/' || !generation) return false;
  radio_job_t job = {.public_endpoint = *endpoint, .generation = generation, .live_stream = live_stream};
  return submit(&job);
}

bool pet_radio_submit(const pet_radio_station_t *station, uint32_t generation) {
  char path[48];
  int length = snprintf(path, sizeof(path), "/live/%s/64k.mp3", station->channel);
  return length > 0 && length < (int)sizeof(path) && pet_network_audio_submit(&qingting, path, true, generation);
}
#endif
