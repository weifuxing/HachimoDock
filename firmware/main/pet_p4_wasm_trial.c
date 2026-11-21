#include "pet_p4_wasm_trial.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include "nvs.h"
#include "mbedtls/sha256.h"
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_pthread.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "hachi_runtime.h"
#include "pet_p4_media.h"
#include "pet_p4_miniapp.h"
#if PET_P4_WASM_AUDIO
#include "pet_p4_audio.h"
#include "pet_p4_component_media.h"
#endif
static const char radio_source[] = "pc.city-radio";
typedef struct { unsigned session; int type, a, b; } input_t;
typedef struct { unsigned session; int action; } command_t;

static atomic_uint requested, requested_revision, failed_session;
static atomic_bool worker_failed;
static atomic_uint dropped_inputs;
enum { INPUT_QUEUE_SIZE = 24, INPUT_EDGE_RESERVE = 4 };
static QueueHandle_t inputs, commands, install_checks;
static pthread_mutex_t start_lock = PTHREAD_MUTEX_INITIALIZER;
static portMUX_TYPE failure_lock = portMUX_INITIALIZER_UNLOCKED;
static char last_failure_id[48], last_failure_hash[65], last_failure_error[128];

typedef struct {
  SemaphoreHandle_t done;
  atomic_uint owners;
  size_t size;
  hachi_saves_t saves;
  bool radio, ok;
  char error[128];
  uint8_t bytes[];
} install_check_t;

static void release_install_check(install_check_t *check) {
  if (atomic_fetch_sub(&check->owners, 1) != 1) return;
  vSemaphoreDelete(check->done);
  free(check);
}

static void remember_failure(const char *id, const char *hash, const char *error) {
  portENTER_CRITICAL(&failure_lock);
  snprintf(last_failure_id, sizeof(last_failure_id), "%s", id);
  snprintf(last_failure_hash, sizeof(last_failure_hash), "%s", hash);
  snprintf(last_failure_error, sizeof(last_failure_error), "%s", error ? error : "unknown WASM error");
  portEXIT_CRITICAL(&failure_lock);
}

void pet_p4_wasm_trial_diagnostics(cJSON *parent) {
  char id[48], hash[65], error[128];
  portENTER_CRITICAL(&failure_lock);
  memcpy(id, last_failure_id, sizeof(id));
  memcpy(hash, last_failure_hash, sizeof(hash));
  memcpy(error, last_failure_error, sizeof(error));
  portEXIT_CRITICAL(&failure_lock);
  cJSON_AddNumberToObject(parent, "wasmDroppedInputs", atomic_load(&dropped_inputs));
  if (!id[0]) return;
  cJSON *failure = cJSON_AddObjectToObject(parent, "wasmLastFailure");
  if (!failure) return;
  cJSON_AddStringToObject(failure, "componentId", id);
  cJSON_AddStringToObject(failure, "wasmSha256", hash);
  cJSON_AddStringToObject(failure, "error", error);
}
static SemaphoreHandle_t frame_lock;
static uint16_t *shown, *drawing;
static unsigned shown_session, shown_revision;
static int shown_width;
static EXT_RAM_BSS_ATTR hachi_text_layer_t shown_text, render_text;
enum { FRAME_PIXELS = HACHI_NATIVE_W * HACHI_NATIVE_H };
static bool started, attempted;
static const char *TAG = "wasm-trial";

typedef struct { char id[48]; hachi_saves_t slots; } save_record_t;

static void save_key(const char *id, char key[16]) {
  uint64_t hash=0xcbf29ce484222325ULL;
  for(;*id;++id)hash=(hash^(unsigned char)*id)*0x100000001b3ULL;
  snprintf(key,16,"%015llx",(unsigned long long)(hash&0xfffffffffffffffULL));
}

static bool read_save(nvs_handle_t handle, const char *key, const char *id, save_record_t *record) {
  size_t length=sizeof(*record);
  return nvs_get_blob(handle,key,record,&length)==ESP_OK && length==sizeof(*record)
      && memchr(record->id,0,sizeof(record->id)) && !strcmp(record->id,id);
}

static hachi_saves_t load_save(const char *id) {
  save_record_t record={0};
  nvs_handle_t handle;
  char key[16];
  save_key(id,key);
  if(nvs_open("hachiwasm",NVS_READONLY,&handle)!=ESP_OK)return record.slots;
  bool valid=read_save(handle,key,id,&record);
  nvs_close(handle);
  return valid?record.slots:(hachi_saves_t){0};
}

static bool write_save(const char *id, const hachi_saves_t *slots) {
  nvs_handle_t handle;
  if(nvs_open("hachiwasm",NVS_READWRITE,&handle)!=ESP_OK)return false;
  char key[16];
  save_key(id,key);
  save_record_t record={0};
  size_t size=0;
  esp_err_t existing=nvs_get_blob(handle,key,NULL,&size);
  bool owned=existing==ESP_ERR_NVS_NOT_FOUND || (existing==ESP_OK && read_save(handle,key,id,&record));
  snprintf(record.id,sizeof(record.id),"%s",id);
  record.slots=*slots;
  bool saved=owned&&nvs_set_blob(handle,key,&record,sizeof(record))==ESP_OK&&nvs_commit(handle)==ESP_OK;
  nvs_close(handle);
  return saved;
}

#if PET_P4_WASM_AUDIO
static void apply_sample(int index, unsigned session, unsigned revision, bool music) {
  if (atomic_load(&requested) != session || atomic_load(&requested_revision) != revision) return;
  if (index == HACHI_SAMPLE_STOP) {
    pet_p4_audio_wasm_channel_stop(session, music);
  } else if (index >= 0) {
    uint32_t generation = pet_p4_wasm_speech_generation(revision);
    pet_cmedia_speech_lease_t cue = {0};
    if (pet_cmedia_speech_pin_try_at((unsigned)index, generation, &cue)) {
      if (atomic_load(&requested) == session && atomic_load(&requested_revision) == revision &&
          pet_p4_wasm_speech_generation(revision) == generation)
        (void)pet_p4_audio_wasm_channel_play(session, cue.data, cue.bytes, music ? 30 : 70, music);
      (void)pet_cmedia_unpin_deferred(cue.token);
    }
  }
}
#endif

static void apply_effects(const hachi_runtime_t *runtime, unsigned session) {
  if (atomic_load(&requested) != session) return;
  if (runtime->tone_count) {
    hachi_tone_t tone = runtime->tones[runtime->tone_count - 1];
    pet_p4_wasm_trial_tone(tone.hz, tone.ms, tone.volume);
  }
  if (runtime->media_command) {
    command_t command = {session, runtime->media_command};
    xQueueSend(commands, &command, 0);
  }
}

static portMUX_TYPE host_lock = portMUX_INITIALIZER_UNLOCKED;
static unsigned host_session, uv_session;
static uint64_t uv_requested_at;
static uint32_t uv_sample_seconds;
static int32_t uv_index;
enum { UV_FRESH_MS = 30 * 60 * 1000, UV_STALE_MS = 60 * 60 * 1000 };

__attribute__((optimize("Os")))
bool pet_p4_wasm_uv_requested(uint64_t now_ms) {
  portENTER_CRITICAL(&host_lock);
  bool active = uv_session && uv_session == atomic_load(&requested)
    && atomic_load(&requested_revision) && (now_ms < uv_requested_at || now_ms - uv_requested_at < 6000);
  portEXIT_CRITICAL(&host_lock);
  return active;
}

__attribute__((optimize("Os")))
void pet_p4_wasm_uv_update(const cJSON *value) {
  uint32_t sample_seconds = 0;
  int32_t index = 0;
  if (cJSON_IsObject(value)) {
    const cJSON *time = cJSON_GetObjectItemCaseSensitive(value, "sampleSeconds");
    const cJSON *uvi = cJSON_GetObjectItemCaseSensitive(value, "indexCenti");
    if (!cJSON_IsNumber(time) || !(time->valuedouble >= 1577836800.0
        && time->valuedouble <= 4102444799.0)
        || time->valuedouble != (double)(uint32_t)time->valuedouble
        || !cJSON_IsNumber(uvi) || uvi->valuedouble != uvi->valueint
        || uvi->valueint < 0 || uvi->valueint > 2500) return;
    sample_seconds = (uint32_t)time->valuedouble;
    index = uvi->valueint;
  } else if (!cJSON_IsNull(value)) return;
  portENTER_CRITICAL(&host_lock);
  uv_sample_seconds = sample_seconds;
  uv_index = index;
  portEXIT_CRITICAL(&host_lock);
}

__attribute__((optimize("Os")))
static int read_host_snapshot(int kind, hachi_snapshot_t *out, bool request_uv) {
  uint64_t now_ms = esp_timer_get_time() / 1000;
  if (kind == HACHI_HOST_MONOTONIC) {
    out->time_ms = now_ms;
    return HACHI_HOST_READY;
  }
  if (kind == HACHI_HOST_LOCAL) {
    return pet_p4_miniapp_clock_snapshot(now_ms, &out->time_ms, &out->value, &out->age_ms)
      ? HACHI_HOST_READY : HACHI_HOST_UNAVAILABLE;
  }
  portENTER_CRITICAL(&host_lock);
  if (request_uv) {
    uv_session = host_session;
    uv_requested_at = now_ms;
  }
  uint32_t seconds = uv_sample_seconds;
  int32_t index = uv_index;
  portEXIT_CRITICAL(&host_lock);
  uint64_t utc_ms;
  int32_t offset;
  uint32_t age;
  if (!seconds || !pet_p4_miniapp_clock_snapshot(now_ms, &utc_ms, &offset, &age))
    return HACHI_HOST_UNAVAILABLE;
  uint64_t sample = (uint64_t)seconds * 1000;
  int64_t delta = (int64_t)utc_ms - (int64_t)sample;
  if (delta < -60000 || delta > UV_STALE_MS) return HACHI_HOST_UNAVAILABLE;
  out->time_ms = sample;
  out->value = index;
  out->age_ms = delta > 0 ? (uint32_t)delta : 0;
  return out->age_ms > UV_FRESH_MS ? HACHI_HOST_STALE : HACHI_HOST_READY;
}

static int read_host(int kind, hachi_snapshot_t *out) {
  return read_host_snapshot(kind, out, true);
}

static int read_install_host(int kind, hachi_snapshot_t *out) {
  return read_host_snapshot(kind, out, false);
}

static void check_install(install_check_t *check) {
  uint16_t *scratch = heap_caps_malloc(FRAME_PIXELS * sizeof(*scratch), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!scratch) {
    snprintf(check->error, sizeof(check->error), "WASM validation framebuffer unavailable");
  } else {
    hachi_runtime_t candidate = {0};
    if (hachi_open_host(&candidate, check->bytes, check->size, scratch, FRAME_PIXELS, &check->saves,
                        read_install_host, esp_random, check->error, sizeof(check->error))) {
      candidate.media_allowed = check->radio;
      check->ok = hachi_frame_run(&candidate, 33);
      if (!check->ok) {
        const char *error = wasm_runtime_get_exception(candidate.instance);
        snprintf(check->error, sizeof(check->error), "%s", error ? error : "WASM first frame failed");
      }
      hachi_close(&candidate);
    }
    free(scratch);
  }
  xSemaphoreGive(check->done);
  release_install_check(check);
}

static void module_hash(const uint8_t *bytes, size_t size, char hash[65]) {
  uint8_t digest[32];
  hash[0] = 0;
  if (mbedtls_sha256(bytes, size, digest, 0) != 0) return;
  for (unsigned i = 0; i < sizeof(digest); ++i)
    snprintf(hash + i * 2, 3, "%02x", digest[i]);
}

static void *worker(void *arg) {
  (void)arg;
  const unsigned flags = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
  uint8_t *pool=NULL,*binary=NULL;
  for (;;) {
    pool = heap_caps_malloc(2 * HACHI_POOL_BYTES, flags);
    binary = heap_caps_malloc(HACHI_MAX_MODULE, flags);
    drawing = heap_caps_malloc(FRAME_PIXELS * sizeof(*drawing), flags);
    shown = heap_caps_malloc(FRAME_PIXELS * sizeof(*shown), flags);
    if (pool && binary && drawing && shown && hachi_runtime_init(pool,2*HACHI_POOL_BYTES)) break;
    atomic_store(&worker_failed,true);
    free(pool); free(binary); free(drawing); free(shown);
    pool=binary=NULL; drawing=shown=NULL;
    remember_failure("runtime","","runtime memory unavailable; retrying");
    for (unsigned retry=0;retry<20;++retry) {
      install_check_t *check;
      if(xQueueReceive(install_checks,&check,pdMS_TO_TICKS(100))==pdTRUE){
        snprintf(check->error,sizeof(check->error),"WASM runtime recovering");
        xSemaphoreGive(check->done);release_install_check(check);
      }
    }
  }
  atomic_store(&worker_failed,false);

  hachi_runtime_t runtime = {0};
  hachi_saves_t saves={0}, persisted={0};
  char active_id[48]="", active_hash[65]="";
  unsigned session=0, loaded_revision=0;
  uint64_t prior_ms=0,last_write_ms=0;
  while (true) {
    install_check_t *check;
    if (xQueueReceive(install_checks, &check, 0) == pdTRUE) check_install(check);
    unsigned target=atomic_load(&requested);
    unsigned revision=atomic_load(&requested_revision);
    if (target!=session) {
      if(active_id[0]&&memcmp(&saves,&persisted,sizeof(saves))){
        if(!write_save(active_id,&saves)){
          remember_failure(active_id,active_hash,"save persistence failed; retrying");
          atomic_store(&failed_session,target);
          vTaskDelay(pdMS_TO_TICKS(500));continue;
        }
        persisted=saves;
      }
      hachi_close(&runtime);
      session=target;
      loaded_revision=revision;
      char id[48]="";
      bool radio=false;
      size_t bytes=0;
      bool package=revision && pet_p4_wasm_package(binary,HACHI_MAX_MODULE,&bytes,revision,id,&radio);
      if(package){
        module_hash(binary, bytes, active_hash);
        if(strcmp(active_id,id)){
          saves=persisted=load_save(id);
          memcpy(active_id,id,sizeof(active_id));
        }
        char error[128]="invalid WASM";
        host_session = session;
        if(hachi_open_host(&runtime,binary,bytes,drawing,FRAME_PIXELS,&saves,
                          read_host,esp_random,error,sizeof(error))){
          atomic_store(&failed_session,0);
          runtime.media_allowed=radio;
          ESP_LOGI(TAG,"opened %s",id);
        }else{
          ESP_LOGE(TAG,"%s: %s",id,error);
          remember_failure(id, active_hash, error);
          atomic_store(&failed_session, session);
        }
      }
      prior_ms=esp_timer_get_time()/1000;
    }
    uint64_t clock_ms=esp_timer_get_time()/1000;
    if(active_id[0]&&clock_ms-last_write_ms>=2000&&memcmp(&saves,&persisted,sizeof(saves))){
      if(write_save(active_id,&saves))persisted=saves;
      else remember_failure(active_id,active_hash,"save persistence failed; retrying");
      last_write_ms=clock_ms;
    }
    if (runtime.instance && loaded_revision == revision) {
      input_t input;
      bool ok = true;
#if PET_P4_WASM_AUDIO
      int sample_index = HACHI_SAMPLE_NONE, music_index = HACHI_SAMPLE_NONE;
#endif
      for (int i = 0; i < 12 && xQueueReceive(inputs, &input, 0); ++i) {
        if (input.session != session) continue;
        if (runtime.width == HACHI_W && (input.type <= 3 || input.type == 6)) {
          input.a /= 2;
          input.b /= 2;
        }
        if (!(ok = hachi_event_run(&runtime, input.type, input.a, input.b))) break;
        apply_effects(&runtime, session);
#if PET_P4_WASM_AUDIO
        if (runtime.sample_index != HACHI_SAMPLE_NONE) sample_index = runtime.sample_index;
        if (runtime.music_index != HACHI_SAMPLE_NONE) music_index = runtime.music_index;
#endif
      }
      uint64_t now_ms = esp_timer_get_time() / 1000;
      uint64_t dt = now_ms - prior_ms;
      prior_ms = now_ms;
      if (ok && runtime.media_allowed) {
        vTaskDelay(pdMS_TO_TICKS(10));
        continue;
      }
      if (ok && hachi_frame_run(&runtime, dt > 80 ? 80 : (int)dt)) {
        apply_effects(&runtime, session);
        xSemaphoreTake(frame_lock, portMAX_DELAY);
        uint16_t *old = shown;
        shown = drawing;
        drawing = old;
        shown_width = runtime.width;
        shown_text.count = runtime.text.count;
        memcpy(shown_text.items, runtime.text.items, shown_text.count * sizeof(shown_text.items[0]));
        shown_session = session;
        shown_revision = revision;
        xSemaphoreGive(frame_lock);
        runtime.screen = drawing;
#if PET_P4_WASM_AUDIO
        if (runtime.sample_index != HACHI_SAMPLE_NONE) sample_index = runtime.sample_index;
        if (runtime.music_index != HACHI_SAMPLE_NONE) music_index = runtime.music_index;
        apply_sample(music_index, session, loaded_revision, true);
        apply_sample(sample_index, session, loaded_revision, false);
#endif
      } else {
        const char *error = wasm_runtime_get_exception(runtime.instance);
        remember_failure(active_id, active_hash, error);
        ESP_LOGE(TAG, "%s trapped: %s", active_id, error);
#if PET_P4_WASM_AUDIO
        pet_p4_audio_wasm_speech_stop(session);
#endif
        xSemaphoreTake(frame_lock, portMAX_DELAY);
        shown_session = 0;
        atomic_store(&failed_session, session);
        xSemaphoreGive(frame_lock);
        hachi_close(&runtime);
      }
    }
    vTaskDelay(pdMS_TO_TICKS(revision ? 30 : 80));
  }
}

static bool start(void) {
  inputs = xQueueCreate(INPUT_QUEUE_SIZE, sizeof(input_t));
  commands = xQueueCreate(8, sizeof(command_t));
  frame_lock = xSemaphoreCreateMutex();
  install_checks = xQueueCreate(1, sizeof(install_check_t *));
  esp_pthread_cfg_t saved;
  bool had_config = esp_pthread_get_cfg(&saved) == ESP_OK;
  esp_pthread_cfg_t config = esp_pthread_get_default_config();
  config.stack_size = 16384;
  config.thread_name = "pet_wasm";
  pthread_t thread;
  bool ready = inputs && commands && frame_lock && install_checks &&
    esp_pthread_set_cfg(&config) == ESP_OK && pthread_create(&thread, NULL, worker, NULL) == 0;
  esp_pthread_cfg_t defaults = esp_pthread_get_default_config();
  esp_pthread_set_cfg(had_config ? &saved : &defaults);
  if (ready) pthread_detach(thread);
  else {
    if(inputs)vQueueDelete(inputs);if(commands)vQueueDelete(commands);
    if(install_checks)vQueueDelete(install_checks);if(frame_lock)vSemaphoreDelete(frame_lock);
    inputs=commands=install_checks=NULL;frame_lock=NULL;
  }
  return ready;
}

static bool ensure_started(bool needed) {
  pthread_mutex_lock(&start_lock);
  static int64_t last_attempt;
  if (needed && (!attempted || (!started && esp_timer_get_time()-last_attempt>5000000))) {
    attempted = true;
    last_attempt=esp_timer_get_time();
    started = start();
    if (!started) atomic_store(&worker_failed, true);
  }
  bool ready = started;
  pthread_mutex_unlock(&start_lock);
  return ready;
}

bool pet_p4_wasm_validate_install(const char *id, const uint8_t *module, size_t size,
                                  bool radio, char *error, size_t error_size) {
  if (!size || size > HACHI_MAX_MODULE || !ensure_started(true) || atomic_load(&worker_failed)) {
    snprintf(error, error_size, "WASM validation runtime unavailable; existing package preserved");
    return false;
  }
  install_check_t *check = heap_caps_calloc(1, sizeof(*check) + size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!check) {
    snprintf(error, error_size, "WASM validation memory unavailable; existing package preserved");
    return false;
  }
  check->done = xSemaphoreCreateBinary();
  if (!check->done) {
    free(check);
    snprintf(error, error_size, "WASM validation unavailable; existing package preserved");
    return false;
  }
  atomic_init(&check->owners, 2);
  check->saves = load_save(id);
  check->radio = radio;
  check->size = size;
  memcpy(check->bytes, module, size);
  if (xQueueSend(install_checks, &check, 0) != pdTRUE) {
    release_install_check(check);
    release_install_check(check);
    snprintf(error, error_size, "WASM validation busy; existing package preserved");
    return false;
  }
  bool finished = xSemaphoreTake(check->done, pdMS_TO_TICKS(2000)) == pdTRUE;
  bool ok = finished && check->ok;
  if (!ok) {
    char hash[65];
    module_hash(module, size, hash);
    const char *why = finished ? check->error : "validation timed out";
    remember_failure(id, hash, why);
    snprintf(error, error_size, "%s: WASM first frame: %s; existing package preserved", id, why);
  }
  release_install_check(check);
  return ok;
}

void pet_p4_wasm_trial_tick(bool visible) {
  unsigned revision=visible?pet_p4_wasm_package(NULL,0,NULL,0,NULL,NULL):0;
  if (!ensure_started(revision != 0)) return;
  if (atomic_load(&requested_revision)!=revision) {
    xQueueReset(inputs);
    pet_p4_wasm_trial_tone(0,0,0);
#if PET_P4_WASM_AUDIO
    unsigned next_session = atomic_load(&requested) + 1;
    pet_p4_audio_wasm_speech_session(revision ? next_session : 0);
#endif
    atomic_store(&requested_revision,revision);
    atomic_fetch_add(&requested,1);
  }
  static const char *const actions[] = {
    NULL, "media.previous", "media.toggle", "media.next", "media.down", "media.up", "media.queue", "media.select"
  };
  command_t cmd;
  for (int i = 0; i < 8 && xQueueReceive(commands, &cmd, 0); ++i) {
    if (cmd.session != atomic_load(&requested) || !revision ||
        cmd.action < 1 || (unsigned)cmd.action >= sizeof(actions) / sizeof(actions[0])) continue;
    char source[48];
    if (pet_p4_miniapp_media_config(source, NULL) && !strcmp(source, radio_source))
      pet_p4_media_live_action(source, actions[cmd.action]);
  }
}

static unsigned active_trial(void) {
  unsigned revision=atomic_load(&requested_revision);
  return revision&&pet_p4_wasm_package(NULL,0,NULL,revision,NULL,NULL)?atomic_load(&requested):0;
}

static bool active_radio(void) {
  char id[48];
  return pet_p4_miniapp_active_id(id,sizeof(id))&&!strcmp(id,"city-radio");
}

static unsigned canvas_session(void) {
  return active_radio()?0:active_trial();
}

bool pet_p4_wasm_trial_pointer_active(void) { return canvas_session() != 0; }

static bool submit(unsigned session, int type, int a, int b) {
  input_t input = {session, type, a, b};
  unsigned reserve = type == 2 ? INPUT_EDGE_RESERVE : (type == 3 || type == 6 ? 0 : 1);
  if (uxQueueSpacesAvailable(inputs) <= reserve ||
      xQueueSend(inputs, &input, 0) != pdTRUE) {
    atomic_fetch_add(&dropped_inputs, 1);
    return false;
  }
  return true;
}

bool pet_p4_wasm_trial_pointer(const pet_p4_pointer_event_t *event) {
  unsigned session = canvas_session();
  if (!session) return false;
  int type = event->kind == PET_P4_POINTER_CANCEL ? 6 : 1 + event->kind;
  (void)submit(session, type, event->x, event->y);
  return true;
}

unsigned pet_p4_wasm_action_key(const char *action, bool media) {
  if (!action) return 0;
  static const char *const local[] = {"lab.left", "lab.right", "lab.up", "lab.down", "lab.a", "lab.b", NULL};
  static const char *const remote[] = {"media.previous", "media.next", "media.up", "media.down", "media.toggle", "media.queue", "media.select"};
  const char *const *map = media ? remote : local;
  for (unsigned i = 0; i < 7; ++i)
    if (map[i] && !strcmp(map[i], action)) return i + 1;
  return 0;
}

bool pet_p4_wasm_trial_action(const char *action, bool *accepted) {
  unsigned session = active_trial();
  if (!session) return false;
  unsigned key = pet_p4_wasm_action_key(action, active_radio());
  if (!key) return false;
  *accepted = submit(session, 4, (int)key, 0);
  return true;
}

pet_wasm_draw_state_t pet_p4_wasm_trial_draw(
    const char *id, unsigned revision, uint16_t *frame, void (*draw_text)(const hachi_text_t *)) {
  if (!frame || !revision || !strcmp(id, "city-radio")) return PET_WASM_DRAW_NONE;
  if (atomic_load(&worker_failed)) return PET_WASM_DRAW_FAILED;
  unsigned session = atomic_load(&requested);
  if (!session || atomic_load(&requested_revision) != revision) return PET_WASM_DRAW_PENDING;
  if (!xSemaphoreTake(frame_lock, portMAX_DELAY)) return PET_WASM_DRAW_PENDING;
  bool ready = shown_session == session && shown_revision == revision;
  bool failed = atomic_load(&failed_session) == session;
  render_text.count = 0;
  if (ready && shown_width == HACHI_NATIVE_W) {
    memcpy(frame, shown, FRAME_PIXELS * sizeof(*shown));
    render_text.count = shown_text.count;
    memcpy(render_text.items, shown_text.items, render_text.count * sizeof(render_text.items[0]));
  } else if (ready) {
    for (int y = 0; y < HACHI_H; ++y) {
      uint32_t *row = (uint32_t *)(frame + y * 4 * HACHI_W);
      for (int x = 0; x < HACHI_W; ++x) {
        uint32_t pixel = shown[y * HACHI_W + x];
        row[x] = pixel | (pixel << 16);
      }
      memcpy(row + HACHI_W, row, HACHI_W * sizeof(*row));
    }
  }
  xSemaphoreGive(frame_lock);
  if (draw_text) {
    for (unsigned i = 0; i < render_text.count; ++i) draw_text(&render_text.items[i]);
  }
  if (ready) return PET_WASM_DRAW_READY;
  return failed ? PET_WASM_DRAW_FAILED : PET_WASM_DRAW_PENDING;
}
