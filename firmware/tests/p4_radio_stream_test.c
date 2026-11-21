#include "pet_p4_radio.c"
#include <assert.h>

static char response[32768];
static size_t response_len, response_pos, played;
static bool current, stall, reject_certificate, wifi = true;
static int64_t wifi_return_at, cancel_at;
static bool drop_on_pcm;
static unsigned reconnects;
static pet_radio_error_t finished;
static radio_job_t next_job;
static const char *expected_host = RADIO_HOST;
static unsigned connections, first_timeout;
static unsigned resume_on_connection;
static bool queued_job, delivery_waiting, stop_early = true;
static radio_job_t submitted_job;
static const radio_job_t station_job = {
  .endpoint = &qingting, .path = "/live/5021381/64k.mp3", .generation = 1, .live_stream = true,
};
static unsigned backpressure, loud;
static size_t prefetched;
static int64_t simulated_ms;

int64_t esp_timer_get_time(void) { return simulated_ms * 1000; }
void vTaskDelay(int ticks) { simulated_ms += ticks; }
bool pet_device_wifi_online(void) {
  return wifi || (wifi_return_at && simulated_ms >= wifi_return_at);
}
void pet_device_wifi_note_https(pet_wifi_https_stage_t stage, int error) {
  (void)stage;
  (void)error;
}
bool pet_p4_media_radio_current(uint32_t generation) {
  return current && generation == 1 && (!cancel_at || simulated_ms < cancel_at);
}
bool pet_p4_media_radio_reconnecting(uint32_t generation) {
  if (!pet_p4_media_radio_current(generation)) return false;
  ++reconnects;
  return true;
}
void pet_p4_media_radio_finished(uint32_t generation, pet_radio_error_t error) {
  assert(generation == 1);
  finished = error;
}
pet_radio_write_t pet_p4_media_radio_write(uint32_t generation, const int16_t *pcm, size_t samples) {
  assert(generation == 1 && samples <= PET_RADIO_PCM_MAX);
  if (backpressure) {
    --backpressure;
    delivery_waiting = true;
    return PET_RADIO_BACKPRESSURE;
  }
  delivery_waiting = false;
  for (size_t i = 0; i < samples; ++i) {
    if (pcm[i] > 500 || pcm[i] < -500) ++loud;
  }
  played += samples;
  if (drop_on_pcm) {
    drop_on_pcm = false;
    wifi = false;
    wifi_return_at = now_ms() + 1500;
  }
  if (stop_early && played >= 48000) current = false;
  return PET_RADIO_WRITTEN;
}

QueueHandle_t xQueueCreate(int depth, size_t size) { (void)depth; (void)size; return NULL; }
int xQueueReceive(QueueHandle_t q, void *data, int ticks) {
  (void)q;
  (void)ticks;
  if (!queued_job) return 0;
  queued_job = false;
  *(radio_job_t *)data = next_job;
  return pdTRUE;
}
int xQueueOverwrite(QueueHandle_t q, const void *data) {
  assert(q);
  submitted_job = *(const radio_job_t *)data;
  return pdTRUE;
}
void vQueueDelete(QueueHandle_t q) { (void)q; }
int xTaskCreate(void (*fn)(void *), const char *name, int stack, void *arg, int priority, void *handle) {
  (void)fn; (void)name; (void)stack; (void)arg; (void)priority; (void)handle;
  return 0;
}
void vTaskDelete(void *task) { (void)task; }

esp_tls_t *esp_tls_init(void) {
  static esp_tls_t tls;
  if (++connections == resume_on_connection) {
    stall = false;
    response_pos = 0;
  }
  return &tls;
}
esp_err_t esp_tls_get_error_handle(esp_tls_t *tls, esp_tls_error_handle_t *handle) {
  assert(tls);
  static int error = ESP_ERR_ESP_TLS_CONNECTION_TIMEOUT;
  *handle = &error;
  return ESP_OK;
}
esp_err_t esp_tls_get_and_clear_last_error(esp_tls_error_handle_t handle, int *code, int *flags) {
  (void)code;
  *flags = reject_certificate ? 1 : 0;
  return *handle;
}
int esp_tls_conn_new_async(const char *host, int length, int port,
                          const esp_tls_cfg_t *cfg, esp_tls_t *tls) {
  assert(tls && length == (int)strlen(host) && port == 443 && cfg->non_block);
  if (!strcmp(expected_host, RADIO_HOST)) {
    assert(!strcmp(host, RADIO_HOST) && cfg->cacert_buf && cfg->cacert_bytes == 914);
  } else {
    assert(!strcmp(host, "8.8.4.4") && !strcmp(cfg->common_name, expected_host));
    assert(!cfg->skip_common_name && !cfg->cacert_buf && cfg->crt_bundle_attach);
  }
  if (reject_certificate) return -1;
  if (connections == first_timeout) return 0;
  return 1;
}
ssize_t esp_tls_conn_read(esp_tls_t *tls, void *out, size_t max) {
  assert(tls);
  if (stall) return ESP_TLS_ERR_SSL_WANT_READ;
  size_t n = response_len - response_pos;
  if (n > max) n = max;
  if (n > 17) n = 17;
  if (delivery_waiting) prefetched += n;
  memcpy(out, response + response_pos, n);
  response_pos += n;
  return n;
}
ssize_t esp_tls_conn_write(esp_tls_t *tls, const void *data, size_t bytes) {
  char header[150];
  snprintf(header, sizeof(header), "Host: %s\r\n", expected_host);
  assert(tls && bytes && strstr(data, header) && strstr(data, "Icy-MetaData: 0\r\n"));
  return bytes;
}
void esp_tls_conn_destroy(esp_tls_t *tls) { assert(tls); }

static const char *dns_address = "8.8.4.4";
int getaddrinfo(const char *host, const char *service, const struct addrinfo *hints,
                struct addrinfo **out) {
  assert(!strcmp(host, expected_host) && !service && hints->ai_family == AF_INET);
  static struct sockaddr_in address;
  static struct addrinfo result;
  address.sin_family = AF_INET;
  assert(inet_pton(AF_INET, dns_address, &address.sin_addr) == 1);
  result.ai_family = AF_INET;
  result.ai_addrlen = sizeof(address);
  result.ai_addr = (struct sockaddr *)&address;
  *out = &result;
  return 0;
}
void freeaddrinfo(struct addrinfo *addr) { assert(addr); }
esp_err_t esp_crt_bundle_attach(void *cfg) { assert(cfg); return ESP_OK; }
esp_err_t esp_tls_get_conn_state(esp_tls_t *tls, esp_tls_conn_state_t *state) {
  assert(tls);
  *state = ESP_TLS_CONNECTING;
  return ESP_OK;
}

static radio_stream_t *open_response(const char *headers) {
  radio_stream_t *s = calloc(1, sizeof(*s));
  assert(s);
  s->generation = 1;
  current = true;
  response_len = strlen(headers);
  memcpy(response, headers, response_len);
  response_pos = 0;
  assert(connect_stream(s, &station_job));
  return s;
}

static void finite_fixture(const char *path, const char *type, bool wav) {
  FILE *file = fopen(path, "rb");
  assert(file && fseek(file, 0, SEEK_END) == 0);
  long length = ftell(file);
  assert(length > 0 && length < (long)sizeof(response) - 160 && fseek(file, 0, SEEK_SET) == 0);
  char header[160];
  snprintf(header, sizeof(header), "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %ld\r\n\r\n",
           type, length);
  radio_stream_t *s = open_response(header);
  size_t header_len = response_len;
  assert(fread(response + response_len, 1, length, file) == (size_t)length);
  fclose(file);
  response_len += length;
  stop_early = false;
  played = 0;
  pet_radio_error_t error = PET_RADIO_SOURCE_FAILED;
  assert(read_headers(s, false, &error));
  assert((wav ? decode_wav(s) : decode_mp3(s, false)) == PET_RADIO_COMPLETE);
  assert((wav ? played == 11998 : played >= 12000 && played <= 16000) && response_pos == response_len);
  free(s);

  s = open_response(header);
  response_len = header_len + length - 17;
  played = 0;
  assert(read_headers(s, false, &error));
  assert((wav ? decode_wav(s) : decode_mp3(s, false)) == PET_RADIO_SOURCE_FAILED);
  free(s);
  stop_early = true;
}

int main(int argc, char **argv) {
  assert(argc == 4);
  jobs = &submitted_job;
  assert(!pet_network_audio_submit(&qingting, "//unapproved.invalid/track", false, 1));
  assert(!pet_network_audio_submit(&qingting, "/track\r\nHost: unapproved.invalid", false, 1));
  assert(!pet_network_audio_submit(&qingting, "/track#fragment", false, 1));
  assert(!pet_network_audio_submit(&qingting, "/track", false, 0));
  assert(submitted_job.generation == 0);
  char source_path[] = "/track.wav";
  assert(pet_network_audio_submit(&qingting, source_path, false, 1));
  source_path[1] = 'x';
  assert(!strcmp(submitted_job.path, "/track.wav") && submitted_job.endpoint == &qingting);
  assert(pet_radio_submit(pet_radio_station_at(0), 2));
  assert(submitted_job.live_stream && submitted_job.generation == 2 &&
         !strcmp(submitted_job.path, "/live/5021381/64k.mp3"));
  finite_fixture(argv[2], "audio/mpeg", false);
  finite_fixture(argv[3], "audio/wav", true);
  played = loud = 0;
  FILE *file = fopen(argv[1], "rb");
  assert(file);
  radio_stream_t *s = open_response("HTTP/1.1 200 OK\r\nContent-Type: audio/mpeg\r\n"
                                   "Transfer-Encoding: chunked\r\n\r\n");
  char chunk[219];
  size_t n;
  while ((n = fread(chunk, 1, sizeof(chunk), file))) {
    assert(response_len + n + 12 < sizeof(response));
    response_len += snprintf(response + response_len, sizeof(response) - response_len, "%zx\r\n", n);
    memcpy(response + response_len, chunk, n);
    response_len += n;
    memcpy(response + response_len, "\r\n", 2);
    response_len += 2;
  }
  fclose(file);
  pet_radio_error_t error = PET_RADIO_SOURCE_FAILED;
  assert(read_headers(s, true, &error) && s->chunked);
  backpressure = 2;
  decode_mp3(s, true);
  assert(played >= 48000 && played <= 49300 && loud > 1000 && !backpressure);
  assert(prefetched > 0);
  free(s);

  // The original connection stalls. The same generation resumes once with
  // a new connection and sends audio without requiring a second user action.
  unsigned resume_start = connections;
  resume_on_connection = resume_start + 2;
  next_job = station_job;
  current = stall = queued_job = true;
  response_pos = played = 0;
  radio_task(NULL);
  assert(connections == resume_start + 2 && played >= 48000);

  // The public transport uses the original HTTP/TLS hostname and the checked
  // numeric address. Recover from a Wi-Fi outage after starting a live stream.
  pet_wifi_endpoint_t endpoint;
  assert(pet_wifi_endpoint_parse("https://audio.example/radio", &endpoint));
  assert(pet_network_audio_submit_public(&endpoint, true, 1));
  next_job = submitted_job;
  expected_host = "audio.example";
  strcpy(endpoint.path, "/changed");
  assert(!strcmp(next_job.public_endpoint.path, "/radio"));
  drop_on_pcm = true;
  unsigned before_recovery = reconnects;
  unsigned before_drop = connections;
  resume_on_connection = before_drop + 2;
  current = queued_job = true;
  response_pos = played = 0;
  radio_task(NULL);
  assert(connections == before_drop + 2 && reconnects == before_recovery + 1 && played >= 48000);

  wifi_return_at = 0;
  current = queued_job = true;
  cancel_at = now_ms() + 1000;
  unsigned before_cancel = connections;
  radio_task(NULL);
  assert(connections == before_cancel && now_ms() < cancel_at + 501);
  cancel_at = 0;

  current = queued_job = true;
  radio_task(NULL);
  assert(finished == PET_RADIO_WIFI_DISCONNECTED && connections == before_cancel);
  wifi = true;

  dns_address = "192.168.1.1";
  current = queued_job = true;
  radio_task(NULL);
  assert(finished == PET_RADIO_CONNECT_FAILED && connections == before_cancel);
  dns_address = "8.8.4.4";
  expected_host = RADIO_HOST;

  s = open_response("HTTP/1.1 200 OK\r\nContent-Type: application/vnd.apple.mpegurl\r\n\r\n");
  assert(!read_headers(s, true, &error) && error == PET_RADIO_FORMAT_UNSUPPORTED);
  free(s);
  s = open_response("HTTP/1.1 302 Found\r\nLocation: https://elsewhere.invalid\r\n\r\n");
  assert(!read_headers(s, true, &error));
  free(s);
  s = open_response("HTTP/1.1 200 OK\r\nContent-Type: audio/mpeg\r\nicy-metaint: 2048\r\n\r\n");
  assert(!read_headers(s, true, &error) && error == PET_RADIO_FORMAT_UNSUPPORTED);
  free(s);
  s = open_response("HTTP/1.1 200 OK\r\nContent-Type: audio/mpeg\r\n"
                    "Transfer-Encoding: chunked\r\n\r\n20001\r\n");
  assert(read_headers(s, true, &error));
  uint8_t output[128];
  assert(!read_body(s, output, sizeof(output)));
  free(s);

  s = open_response("");
  stall = true;
  int64_t start = now_ms();
  assert(!read_headers(s, true, &error));
  assert(now_ms() - start >= RADIO_PROGRESS_MS && now_ms() - start <= RADIO_PROGRESS_MS + 11);
  free(s);
  stall = false;

  const unsigned before = connections;
  first_timeout = before + 1;
  s = open_response("");
  assert(connections == before + 2);
  free(s);

  reject_certificate = true;
  s = calloc(1, sizeof(*s));
  assert(s);
  s->generation = 1;
  assert(!connect_stream(s, &station_job));
  assert(s->failure == PET_RADIO_CONNECT_FAILED && connections == before + 3);
  free(s);
}
