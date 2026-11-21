#include "pet_p4_resources_device.h"
#include "pet_p4_audio.h"
#include "pet_p4_media_codec.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_STATE -2
#define ESP_ERR_INVALID_ARG -3
#define ESP_ERR_NO_MEM -4
#define portMAX_DELAY 0
#define pdMS_TO_TICKS(n) (n)
#define PET_P4_AUDIO_STREAM_RING_BYTES (96 * 1024)
#define PET_P4_AUDIO_STREAM_START_BYTES 6400
#define PET_P4_AUDIO_STREAM_WRITE_BYTES 640
#define PET_P4_AUDIO_FRAME_BYTES 640
#define PET_P4_AUDIO_MAX_CAPTURE_MS 30000ULL
#define pdTRUE 1
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
typedef pthread_mutex_t *SemaphoreHandle_t;
typedef struct { int sample_rate, channel, bits_per_sample; } esp_codec_dev_sample_info_t;
static pthread_mutex_t stream_lock;
static SemaphoreHandle_t g_stream_lock = &stream_lock;
static pthread_mutex_t ingress_lock = PTHREAD_MUTEX_INITIALIZER;
static SemaphoreHandle_t g_stream_ingress_lock = &ingress_lock;
static _Thread_local unsigned ingress_held;
static void xSemaphoreTake(SemaphoreHandle_t lock, int wait) {
  (void)wait;assert(!ingress_held);assert(!pthread_mutex_lock(lock));++ingress_held;
}
static void xSemaphoreGive(SemaphoreHandle_t lock) {
  assert(ingress_held==1);--ingress_held;assert(!pthread_mutex_unlock(lock));
}
static int g_speaker = 1, g_microphone = 2, g_audio_task = 1, g_playback_queue = 1;
static atomic_bool g_ready = true, g_enabled = true, g_playback_ready = true;
static atomic_bool g_capture_requested, g_capture_active, g_capture_session_queue_empty, g_playback_active;
static atomic_bool g_conversation, g_conversation_half_duplex, g_conversation_starting;
static portMUX_TYPE g_conversation_lock = portMUX_INITIALIZER_UNLOCKED;
static char g_conversation_session[64], g_capture_owner[64], g_stream_session[64];
static atomic_bool g_stream_active, g_stream_ended, g_stream_playing;
static atomic_bool g_music, g_music_paused, g_music_pause_requested;
static atomic_uint g_playback_generation, g_stream_played_bytes, g_stream_received_bytes, g_stream_underruns, g_music_volume = 65;
static atomic_uint_least64_t g_stream_last_play_ms;
static atomic_size_t g_stream_head, g_stream_tail;
static int32_t g_music_gain;
static uint8_t ring[PET_P4_AUDIO_STREAM_RING_BYTES], *g_stream_ring;
static bool allocation_fail, gain_fail, music_open_fail, codec_write_fail;
static unsigned resets;
static pthread_mutex_t write_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t write_changed = PTHREAD_COND_INITIALIZER;
static bool write_blocked, write_entered, write_released, flush_done, flush_waiting;
static bool expect_speech_write;
static unsigned speech_writes;
static int speech_first_sample;
static _Thread_local bool stopping;
static bool capture_loop_testing;
static int64_t capture_test_time(void);

static void xSemaphoreTakeRecursive(SemaphoreHandle_t lock, int wait) {
  (void)wait;
  if (stopping) {
    assert(!pthread_mutex_lock(&write_lock)); flush_waiting = true;
    assert(!pthread_cond_broadcast(&write_changed)); assert(!pthread_mutex_unlock(&write_lock));
  }
  assert(!pthread_mutex_lock(lock));
}
static void xSemaphoreGiveRecursive(SemaphoreHandle_t lock) { assert(!pthread_mutex_unlock(lock)); }
static void vTaskDelay(int ticks) { (void)ticks; }
static void xTaskNotifyGive(int task) { (void)task; }
static void xQueueReset(int queue) { (void)queue; }
static bool stream_ring_ensure(void) { if (allocation_fail) return false; g_stream_ring = ring; return true; }
static bool conversation_mic_gain(bool enabled) { (void)enabled; return !gain_fail; }
static int esp_codec_dev_close(int device) { (void)device; return ESP_OK; }
static int esp_codec_dev_open(int device, const esp_codec_dev_sample_info_t *format) {
  (void)device; return music_open_fail && format->sample_rate == 48000 ? ESP_FAIL : ESP_OK;
}
static int64_t esp_timer_get_time(void) { return capture_loop_testing?capture_test_time():1000000; }
static void pet_p4_afe_reference(const int16_t *pcm, size_t count) { (void)pcm; (void)count; }
static void pet_p4_afe_reference_abort(void) {}
static void pet_p4_afe_reset_stream(void) { resets++; }
static void send_audio_error(const char *session, const char *error, const char *detail) { (void)session; (void)error; (void)detail; }
void pet_p4_audio_send_status(void) {assert(!ingress_held);}
void pet_p4_audio_diagnostic(const char *event, const char *session, size_t bytes, bool ok) { assert(!ingress_held); (void)event; (void)session; (void)bytes; (void)ok; }
bool pet_p4_audio_ready(void) { return atomic_load(&g_ready); }
bool pet_p4_audio_enabled(void) { return atomic_load(&g_enabled); }
bool pet_p4_audio_playback_ready(void) { return atomic_load(&g_playback_ready); }
static unsigned reservations(void) { pet_resource_usage_t usage; pet_resources_device_usage(&usage); return usage.realtime_reservations; }
static void *heap_caps_malloc(size_t bytes, unsigned flags) {
  assert(flags == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  return malloc(bytes);
}
static int esp_codec_dev_write(int device, const void *data, int bytes) {
  (void)device;
  assert(reservations() > 0);
  if (expect_speech_write) {
    const uint8_t *samples = data;
    assert(bytes == PET_P4_AUDIO_FRAME_BYTES);
    speech_first_sample = (int16_t)((unsigned)samples[0] | (unsigned)samples[1] << 8);
    ++speech_writes;
  }
  assert(!pthread_mutex_lock(&write_lock));
  write_entered = true; assert(!pthread_cond_broadcast(&write_changed));
  while (write_blocked && !write_released) assert(!pthread_cond_wait(&write_changed, &write_lock));
  assert(!pthread_mutex_unlock(&write_lock));
  return codec_write_fail ? ESP_FAIL : ESP_OK;
}
// Production control now queries AFE readiness; its test implementation is below.
static bool pet_p4_afe_ready(void);
#include "production_audio_resources.inc"
#include "production_stream_buffered.inc"
#include "production_stream_drain.inc"
#include "production_wasm_speech.inc"
#include "production_audio_control.inc"

static void storage_denied(void) {
  pet_resource_owner_t owner = {0}; pet_resource_lease_t lease = {0};
  assert(pet_resources_device_owner_open(PET_RESOURCE_MASK(PET_RESOURCE_STORAGE_WORK), &owner) == PET_RESOURCE_OK);
  assert(pet_resources_device_acquire(owner, PET_RESOURCE_STORAGE_WORK, 1, &lease) == PET_RESOURCE_PROTECTED);
  pet_resources_device_revoke(owner);
}
static void capture_aba(void) {
  assert(pet_p4_audio_capture_start_owned(true, "old") == ESP_OK && reservations() == 1);
  pet_resource_realtime_t old_worker = {0}; assert(capture_worker_take(&old_worker,NULL,NULL));
  pet_resource_realtime_t stale = old_worker;
  assert(pet_p4_audio_capture_stop_owned("wrong") == ESP_ERR_INVALID_STATE);
  assert(pet_p4_audio_capture_stop_owned("old") == ESP_OK && reservations() == 1);
  assert(pet_p4_audio_capture_start(true) == ESP_OK && reservations() == 2);
  assert(!capture_worker_current(old_worker));capture_worker_cancel(old_worker);
  assert(atomic_load(&g_capture_requested)&&g_capture_pending.serial);
  capture_worker_done(); pet_resources_device_realtime_end(&old_worker);
  assert(reservations() == 1); storage_denied();
  pet_resource_realtime_t new_worker = {0}; assert(capture_worker_take(&new_worker,NULL,NULL));
  pet_resources_device_realtime_end(&stale); assert(reservations() == 1);
  assert(pet_p4_audio_capture_stop() == ESP_OK && reservations() == 1);
  capture_worker_done(); pet_resources_device_realtime_end(&new_worker); assert(!reservations());
  assert(pet_p4_audio_capture_start(true) == ESP_OK);
  assert(pet_p4_audio_set_enabled(false) == ESP_OK && !reservations());
  pet_resource_realtime_t disabled = {0}; assert(!capture_worker_take(&disabled,NULL,NULL));
  pet_resources_device_realtime_end(&disabled); assert(pet_p4_audio_set_enabled(true) == ESP_OK);
  assert(pet_p4_audio_capture_start(true)==ESP_OK);
  pet_resource_realtime_t failed={0};assert(capture_worker_take(&failed,NULL,NULL)&&capture_worker_current(failed));
  capture_worker_cancel(failed);assert(!atomic_load(&g_capture_requested)&&!capture_worker_current(failed));
  assert(reservations()==1);capture_worker_done();pet_resources_device_realtime_end(&failed);assert(!reservations());
}
static void overlapping_sessions(void) {
  assert(pet_p4_audio_conversation_set(true, false, "conversation") == ESP_OK);
  unsigned held = reservations(), old_resets = resets;
  assert(held == 2); /* Conversation intent and capture request. */
  assert(pet_p4_audio_conversation_set(true, false, "conversation") == ESP_OK);
  assert(reservations() == held && resets == old_resets);
  pet_resource_realtime_t capture = {0}; assert(capture_worker_take(&capture,NULL,NULL));
  assert(pet_p4_audio_stream_begin("voice") == ESP_OK && reservations() == 3);
  assert(pet_p4_audio_music_begin("music-no") == ESP_ERR_INVALID_STATE && reservations() == 3);
  assert(pet_p4_audio_conversation_set(false, false, "") == ESP_OK);
  assert(reservations() == 1); /* Stop cannot release the physical capture. */
  capture_worker_done(); pet_resources_device_realtime_end(&capture); assert(!reservations());
  assert(pet_p4_audio_music_begin("music-test") == ESP_OK && reservations() == 2);
  pet_p4_audio_music_pause(true); assert(reservations() == 2); storage_denied();
  pet_p4_audio_music_pause(false); assert(pet_p4_audio_stream_end() == ESP_OK);
  uint8_t pcm[640]; stream_drain_step(pcm, sizeof(pcm));
  assert(reservations() == 1); /* Music codec mode remains reserved until restore. */
  pet_p4_audio_stream_flush(); assert(!reservations());
  allocation_fail = true;
  assert(pet_p4_audio_stream_begin("failure") == ESP_ERR_NO_MEM && !reservations());
  allocation_fail = false; gain_fail = true;
  assert(pet_p4_audio_conversation_set(true, false, "failure") == ESP_FAIL && !reservations());
  gain_fail = false;
  music_open_fail = true;
  assert(pet_p4_audio_music_begin("codec-failure") == ESP_FAIL && !reservations());
  music_open_fail = false;
  assert(pet_p4_audio_stream_begin("write-failure") == ESP_OK);
  uint8_t queued[6400] = {0}; assert(pet_p4_audio_stream_push(queued, sizeof(queued)) == ESP_OK);
  codec_write_fail = true; stream_drain_step(pcm, sizeof(pcm));
  codec_write_fail = false; assert(!reservations() && !atomic_load(&g_stream_active));
}
static void *physical_write(void *unused) { (void)unused; uint8_t pcm[640]; stream_drain_step(pcm, sizeof(pcm)); return NULL; }
static void *flush_stream(void *unused) {
  (void)unused; stopping = true; pet_p4_audio_stream_flush();
  assert(!pthread_mutex_lock(&write_lock)); flush_done = true;
  assert(!pthread_cond_broadcast(&write_changed)); assert(!pthread_mutex_unlock(&write_lock)); return NULL;
}
static void actual_write_completion(void) {
  assert(pet_p4_audio_stream_begin("block-write") == ESP_OK);
  uint8_t pcm[6400] = {0}; assert(pet_p4_audio_stream_push(pcm, sizeof(pcm)) == ESP_OK);
  write_blocked = true; write_entered = false;
  pthread_t writer, stopper;
  assert(!pthread_create(&writer, NULL, physical_write, NULL));
  assert(!pthread_mutex_lock(&write_lock));
  while (!write_entered) assert(!pthread_cond_wait(&write_changed, &write_lock));
  assert(!pthread_mutex_unlock(&write_lock));
  // The real consumer is parked inside codec write while owning stream_lock.
  // Both protocol operations must finish before we permit that write to return.
  assert(pet_p4_audio_stream_matches("block-write"));
  size_t old_head=atomic_load(&g_stream_head);
  uint8_t fresh[]={1,2,3,4};
  assert(pet_p4_audio_stream_push_session("block-write",fresh,sizeof(fresh))==ESP_OK);
  assert(atomic_load(&g_stream_head)==old_head+sizeof(fresh));
  assert(!memcmp(ring+old_head,fresh,sizeof(fresh)));
  assert(pet_p4_audio_stream_push_session("stale",fresh,sizeof(fresh))==ESP_OK);
  assert(atomic_load(&g_stream_head)==old_head+sizeof(fresh));
  assert(!pthread_create(&stopper, NULL, flush_stream, NULL));
  assert(!pthread_mutex_lock(&write_lock));
  while (!flush_waiting) assert(!pthread_cond_wait(&write_changed, &write_lock));
  assert(!pthread_mutex_unlock(&write_lock));
  assert(reservations() == 1); storage_denied();
  assert(!pthread_mutex_lock(&write_lock)); assert(!flush_done);
  write_released = true; assert(!pthread_cond_broadcast(&write_changed)); assert(!pthread_mutex_unlock(&write_lock));
  assert(!pthread_join(writer, NULL)); assert(!pthread_join(stopper, NULL));
  assert(flush_done && !reservations());
  write_blocked = false;
  assert(!pet_p4_audio_stream_matches("block-write"));
  assert(pet_p4_audio_stream_push_session("block-write",fresh,sizeof(fresh))==ESP_OK);
  assert(!stream_buffered());
  assert(pet_p4_audio_stream_begin("replacement")==ESP_OK);
  assert(pet_p4_audio_stream_push_session("block-write",fresh,sizeof(fresh))==ESP_OK);
  assert(!stream_buffered());
  assert(pet_p4_audio_stream_push_session("replacement",fresh,sizeof(fresh))==ESP_OK);
  assert(stream_buffered()==sizeof(fresh)&&!memcmp(ring,fresh,sizeof(fresh)));
  // Capacity rejection cannot alter bytes, head or counters; wrap preserves order.
  atomic_store(&g_stream_head,PET_P4_AUDIO_STREAM_RING_BYTES-2);
  atomic_store(&g_stream_tail,PET_P4_AUDIO_STREAM_RING_BYTES-2);
  assert(pet_p4_audio_stream_push_session("replacement",fresh,sizeof(fresh))==ESP_OK);
  assert(ring[PET_P4_AUDIO_STREAM_RING_BYTES-2]==1&&ring[PET_P4_AUDIO_STREAM_RING_BYTES-1]==2&&ring[0]==3&&ring[1]==4);
  atomic_store(&g_stream_tail,4);size_t head=atomic_load(&g_stream_head);
  unsigned received=atomic_load(&g_stream_received_bytes);
  assert(pet_p4_audio_stream_push_session("replacement",fresh,sizeof(fresh))==ESP_ERR_NO_MEM);
  assert(atomic_load(&g_stream_head)==head&&atomic_load(&g_stream_received_bytes)==received);
  assert(pet_p4_audio_stream_push_session("replacement",fresh,SIZE_MAX)==ESP_ERR_NO_MEM);
  pet_p4_audio_stream_flush();assert(!reservations());
}

static void actual_ingress_flush_boundary(void) {
  assert(pet_p4_audio_stream_begin("admitted")==ESP_OK);
  flush_waiting=flush_done=false;
  // An already admitted producer owns ingress while stop obtains stream.
  // Stop must not reset the ring until that bounded copy has finished.
  xSemaphoreTake(g_stream_ingress_lock,0);
  pthread_t stopper;assert(!pthread_create(&stopper,NULL,flush_stream,NULL));
  assert(!pthread_mutex_lock(&write_lock));
  while(!flush_waiting)assert(!pthread_cond_wait(&write_changed,&write_lock));
  assert(!flush_done);assert(!pthread_mutex_unlock(&write_lock));
  uint8_t pcm[]={9,8,7,6};
  assert(audio_stream_push_locked(pcm,sizeof(pcm))==ESP_OK);
  xSemaphoreGive(g_stream_ingress_lock);
  assert(!pthread_join(stopper,NULL));
  assert(flush_done&&!g_stream_active&&!stream_buffered()&&!reservations());
  assert(pet_p4_audio_stream_begin("after-stop")==ESP_OK);
  assert(pet_p4_audio_stream_push_session("admitted",pcm,sizeof(pcm))==ESP_OK);
  assert(!stream_buffered()&&!g_stream_received_bytes);
  assert(pet_p4_audio_stream_push_session("after-stop",pcm,sizeof(pcm))==ESP_OK);
  assert(stream_buffered()==sizeof(pcm)&&!memcmp(ring,pcm,sizeof(pcm)));
  pet_p4_audio_stream_flush();assert(!reservations());
}

static bool drain_speech(void) {
  uint8_t pcm[PET_P4_AUDIO_FRAME_BYTES];
  xSemaphoreTakeRecursive(g_stream_lock, portMAX_DELAY);
  bool wrote = wasm_speech_drain_locked(pcm, wasm_speech_blocked());
  xSemaphoreGiveRecursive(g_stream_lock);
  return wrote;
}

static void *physical_speech_write(void *unused) {
  (void)unused;
  assert(drain_speech());
  return NULL;
}

static void actual_wasm_speech(void) {
  uint8_t first[2 * PET_P4_WASM_SPEECH_BLOCK_BYTES] = {0xe8, 3, 0, 0, 64, 1};
  uint8_t second[PET_P4_WASM_SPEECH_BLOCK_BYTES] = {0xd0, 7, 0, 0, 64, 1};
  memcpy(first + PET_P4_WASM_SPEECH_BLOCK_BYTES, first, sizeof(second));
  expect_speech_write = true;

  assert(!pet_p4_audio_wasm_speech_play(1, first, sizeof(first), 100));
  pet_p4_audio_wasm_speech_session(1);
  assert(!pet_p4_audio_wasm_speech_play(1, first, sizeof(first) - 1, 100));
  assert(!pet_p4_audio_wasm_speech_play(1, first, PET_P4_WASM_SPEECH_MAX_BYTES + 1, 100));
  first[sizeof(first) - 1] = 0xf0;
  assert(!pet_p4_audio_wasm_speech_play(1, first, sizeof(first), 100));
  first[sizeof(first) - 1] = 0;

  assert(pet_p4_audio_wasm_speech_play(1, first, sizeof(first), 50));
  first[0] = 0;
  assert(drain_speech() && speech_first_sample == 500 && speech_writes == 1);
  first[0] = 0xe8;
  assert(pet_p4_audio_wasm_speech_play(1, second, sizeof(second), 100));
  pet_p4_audio_wasm_speech_stop(2);
  assert(drain_speech() && speech_first_sample == 2000 && speech_writes == 2);
  assert(!drain_speech() && !atomic_load(&g_wasm_speech_active));

  assert(pet_p4_audio_wasm_speech_play(1, first, sizeof(first), 100));
  pet_p4_audio_wasm_speech_session(2);
  assert(!drain_speech() && !pet_p4_audio_wasm_speech_play(1, first, sizeof(first), 100));
  assert(pet_p4_audio_wasm_speech_play(2, first, sizeof(first), 100));
  assert(pet_p4_audio_capture_start(true) == ESP_OK);
  assert(!pet_p4_audio_wasm_speech_play(2, second, sizeof(second), 100));
  assert(pet_p4_audio_capture_stop() == ESP_OK && !drain_speech());

  assert(pet_p4_audio_wasm_speech_play(2, first, sizeof(first), 100));
  assert(pet_p4_audio_stream_begin("speech-preempt") == ESP_OK);
  pet_p4_audio_stream_flush();
  assert(!drain_speech());

  assert(pet_p4_audio_wasm_speech_play(2, first, sizeof(first), 100));
  write_blocked = true; write_entered = false; write_released = false;
  pthread_t writer;
  assert(!pthread_create(&writer, NULL, physical_speech_write, NULL));
  assert(!pthread_mutex_lock(&write_lock));
  while (!write_entered) assert(!pthread_cond_wait(&write_changed, &write_lock));
  pet_p4_audio_wasm_speech_stop(2);
  assert(atomic_load(&g_wasm_speech_active) && reservations() == 1);
  storage_denied();
  write_released = true;
  assert(!pthread_cond_broadcast(&write_changed));
  assert(!pthread_mutex_unlock(&write_lock));
  assert(!pthread_join(writer, NULL));
  assert(!drain_speech() && speech_writes == 3 && !reservations());
  write_blocked = false;

  assert(pet_p4_audio_wasm_speech_play(2, second, sizeof(second), 100));
  codec_write_fail = true;
  assert(drain_speech() && !drain_speech() && !reservations());
  codec_write_fail = false;
  assert(pet_p4_audio_wasm_channel_play(2, first, sizeof(first), 30, true));
  assert(drain_speech() && speech_first_sample == 300);
  assert(pet_p4_audio_wasm_channel_play(2, second, sizeof(second), 70, false));
  assert(drain_speech() && speech_first_sample == 1700);
  assert(drain_speech() && speech_first_sample == 300);
  pet_p4_audio_wasm_channel_stop(2, true);
  assert(!drain_speech());
  free(g_wasm_music.encoded);
  g_wasm_music.encoded = NULL;
  pet_p4_audio_wasm_speech_session(0);
  assert(!pet_p4_audio_wasm_speech_play(2, second, sizeof(second), 100));
  free(g_wasm_speech.encoded);
  g_wasm_speech.encoded = NULL;
  expect_speech_write = false;
}
/* Execute the actual physical capture loop. Hooks represent stop/start arriving
 * during a codec call, during a timeout check, or during a chunk enqueue; no
 * replacement state machine is used. The second request must get its own begin,
 * queue policy and end instead of continuing inside the old session. */
static atomic_uint g_session_sequence,g_capture_tx_last_us,g_capture_tx_max_us;
static unsigned capture_fault,capture_begins,capture_ends,new_chunks,old_chunk_attempts;
static bool replaced;
static char capture_sessions[2][64];
static void replace_capture_request(void){
  assert(!replaced&&capture_begins==1);replaced=true;
  assert(pet_p4_audio_capture_stop()==ESP_OK);
  assert(pet_p4_audio_capture_start(false)==ESP_OK);
  assert(reservations()==2); /* Old physical read plus the new pending request. */
}
static int64_t capture_test_time(void){
  if(capture_fault==4&&!replaced&&capture_begins==0){
    /* Deterministic take -> stop/new request -> actual begin serialization.
     * The new request clears the global owner before the old begin is sent. */
    assert(atomic_load(&g_capture_active)&&!strcmp(g_capture_owner,"old-owner"));
    assert(pet_p4_audio_capture_stop_owned("old-owner")==ESP_OK);
    assert(pet_p4_audio_capture_start(false)==ESP_OK);
    assert(!g_capture_owner[0]&&reservations()==2);replaced=true;
  }
  if(capture_fault==2&&capture_begins==1&&!replaced)replace_capture_request();
  return capture_fault==2&&replaced?31000000:1000000;
}
static unsigned ulTaskNotifyTake(int clear,int wait){
  (void)clear;(void)wait;
  if(capture_ends==2){assert(!reservations());pthread_exit(NULL);}
  assert(g_capture_pending.serial);return 1;
}
static bool pet_p4_afe_preparing(void){return false;}
static bool pet_p4_afe_prepare(void){return true;}
static bool pet_p4_afe_ready(void){return false;}
static bool pet_p4_afe_feed(const int16_t *pcm){(void)pcm;return false;}
static bool pet_p4_afe_take(int16_t *pcm,bool *speech){(void)pcm;(void)speech;return false;}
static bool playback_gates_capture(uint64_t now){(void)now;return false;}
static int esp_codec_dev_read(int device,void *pcm,int bytes){
  (void)device;assert(bytes==PET_P4_AUDIO_FRAME_BYTES&&reservations()>0);memset(pcm,5,(size_t)bytes);
  if(capture_begins==1&&capture_fault<2){replace_capture_request();return capture_fault==1?ESP_FAIL:ESP_OK;}
  return ESP_OK;
}
static uint64_t fnv1a64_update(uint64_t hash,const uint8_t *bytes,size_t length){
  for(size_t i=0;i<length;i++){hash^=bytes[i];hash*=UINT64_C(0x00000100000001b3);}return hash;
}
static const char *g_board_device_id="test-board";
static void add_conversation_metadata(cJSON *payload){(void)payload;}
static void send_topic(const char *topic,cJSON *payload){
  assert(!strcmp(topic,"audio/begin"));
  assert(capture_begins<2&&capture_ends==capture_begins);
  const cJSON *session=cJSON_GetObjectItemCaseSensitive(payload,"sessionId");
  const cJSON *owner=cJSON_GetObjectItemCaseSensitive(payload,"captureOwner");
  assert(cJSON_IsString(session)&&cJSON_IsString(owner));
  assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(payload,"sessionQueueEmpty"))==(capture_begins==0));
  assert(!strcmp(owner->valuestring,capture_fault==4&&capture_begins==0?"old-owner":""));
  if(capture_fault==4&&capture_begins==0){
    assert(replaced&&!g_capture_owner[0]); /* Not read from the replaced global. */
  }
  snprintf(capture_sessions[capture_begins++],64,"%s",session->valuestring);
  if(capture_begins==2)assert(strcmp(capture_sessions[0],capture_sessions[1]));
  cJSON_Delete(payload);
}
#include "production_audio_begin.inc"
static bool send_audio_chunk(const char *session,uint32_t index,const uint8_t *pcm,
  size_t length,bool processed,bool speech,const char *conversation){
  (void)pcm;(void)processed;(void)speech;(void)conversation;
  assert(index==0&&length==PET_P4_AUDIO_FRAME_BYTES);
  if(capture_begins==1){
    assert(capture_fault==3&&!strcmp(session,capture_sessions[0]));
    old_chunk_attempts++;replace_capture_request();return false;
  }
  assert(capture_begins==2&&!strcmp(session,capture_sessions[1]));
  new_chunks++;assert(pet_p4_audio_capture_stop()==ESP_OK);return true;
}
static void send_audio_end(const char *session,const char *reason,uint32_t chunks,
  uint64_t bytes,uint64_t hash){
  (void)hash;assert(capture_ends<2&&!strcmp(session,capture_sessions[capture_ends]));
  if(capture_ends==0){
    assert(chunks==0&&bytes==0&&atomic_load(&g_capture_requested)&&g_capture_pending.serial);
    assert(!strcmp(reason,capture_fault==2?"timeout":capture_fault==3?"capture_tx_failed":"released"));
  }else assert(chunks==1&&bytes==PET_P4_AUDIO_FRAME_BYTES&&!atomic_load(&g_capture_requested));
  capture_ends++;
}
static atomic_uint g_usb_capture_tx_pending;
#include "production_capture_task.inc"
static void *run_actual_capture(void *unused){audio_task(unused);return NULL;}
static void actual_capture_stop_start_sessions(void){
  for(capture_fault=0;capture_fault<5;capture_fault++){
    capture_begins=capture_ends=new_chunks=old_chunk_attempts=0;replaced=false;
    capture_loop_testing=true;
    assert(pet_p4_audio_capture_start_owned(true,capture_fault==4?"old-owner":"")==ESP_OK);
    pthread_t thread;assert(!pthread_create(&thread,NULL,run_actual_capture,NULL));
    assert(!pthread_join(thread,NULL));
    capture_loop_testing=false;
    assert(capture_begins==2&&capture_ends==2&&new_chunks==1&&!reservations());
    assert(old_chunk_attempts==(capture_fault==3?1U:0U));
    assert(!atomic_load(&g_capture_requested)&&!atomic_load(&g_capture_active)&&!g_capture_pending.serial);
  }
}
int main(void) {
  pthread_mutexattr_t attr; assert(!pthread_mutexattr_init(&attr));
  assert(!pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE));
  assert(!pthread_mutex_init(&stream_lock, &attr)); assert(!pthread_mutexattr_destroy(&attr));
  capture_aba(); overlapping_sessions(); actual_write_completion();actual_capture_stop_start_sessions();
  actual_ingress_flush_boundary();actual_wasm_speech();
  puts("PASS production capture, overlapping audio sessions, paused music, and WASM speech ownership and physical write cancellation");
  return 0;
}
