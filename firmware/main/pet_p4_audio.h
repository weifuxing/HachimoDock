#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "pet_p4_protocol.h"
#include "pet_p4_instrument_core.h"
#include "pet_p4_sfx_core.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PET_P4_AUDIO_SAMPLE_RATE 16000
#define PET_P4_AUDIO_CHANNELS 1
#define PET_P4_AUDIO_BITS_PER_SAMPLE 16
#define PET_P4_AUDIO_FRAME_MS 20
void pet_p4_audio_instrument_set(bool active, unsigned effect, unsigned ambience);
void pet_p4_audio_fruit_sound(unsigned events,unsigned volume,bool active);
/* p4-sfx-v1: bounded component cues. Same gating as the fruit/instrument paths:
 * capture, realtime conversation, streams and music always win; never queued. */
void pet_p4_audio_sfx_set(bool active,unsigned volume);
bool pet_p4_audio_sfx_trigger(unsigned preset);
/* A WASM speech clip contains up to 8 seconds of independent 20 ms IMA blocks
 * (320 samples, mono at 16 kHz). Playback copies the caller's bytes before it
 * returns. A new clip replaces the old one, and realtime audio takes priority.
 * Changing the current session or stopping that exact session cancels speech
 * at the next codec block boundary. Session zero has no playback authority. */
#define PET_P4_WASM_SPEECH_BLOCK_BYTES 166U
#define PET_P4_WASM_SPEECH_MAX_BYTES (400U * PET_P4_WASM_SPEECH_BLOCK_BYTES)
void pet_p4_audio_wasm_speech_session(unsigned session);
void pet_p4_audio_wasm_speech_stop(unsigned session);
void pet_p4_audio_wasm_channel_stop(unsigned session, bool music);
bool pet_p4_audio_wasm_channel_play(unsigned session, const uint8_t *encoded,
                                    size_t bytes, unsigned volume, bool music);
static inline bool pet_p4_audio_wasm_speech_play(unsigned session, const uint8_t *encoded,
                                                 size_t bytes, unsigned volume) {
  return pet_p4_audio_wasm_channel_play(session, encoded, bytes, volume, false);
}
/* Track C p4-sequence-v1 / p4-samples-v1: mixed inside the sfx drain step with the same
 * blockers (capture, conversation, stream, music); never queued while blocked. */
void pet_p4_audio_cext_set(bool active,const pet_seq_program_t *program,bool sequence_playing,unsigned sequence_volume,unsigned sample_volume);
bool pet_p4_audio_cext_sample(const int16_t *pcm,uint32_t frames);
bool pet_p4_audio_instrument_strike(uint64_t now_ms);
uint32_t pet_p4_audio_instrument_age(uint64_t now_ms);
pet_instrument_motion_t pet_p4_audio_instrument_motion(uint64_t now_ms);

esp_err_t pet_p4_audio_init(
  const char *board_device_id,
  pet_p4_send_line_fn send_line,
  void *ctx
);
bool pet_p4_audio_ready(void);
/* Route capture to the client that enabled it, not every enumerated USB link. */
void pet_p4_audio_set_transport(pet_p4_send_line_fn send_line, void *ctx);
bool pet_p4_audio_enabled(void);
bool pet_p4_audio_active(void);
const char *pet_p4_audio_codec_name(void);
/* Protocol owner publishes before appearance storage/claim admission; only
 * confirmed quiescence releases status cues. Does not gate voice/AEC/streams. */
void pet_p4_audio_status_cues_defer(bool busy);
bool pet_p4_audio_playback_ready(void);
bool pet_p4_audio_playback_active(void);
esp_err_t pet_p4_audio_set_enabled(bool enabled);
/* Capture/stream/conversation starts reserve realtime priority in the shared
 * broker before publishing requested/active state. Stop requests do not free
 * a worker's reservation until its physical codec work has returned. This
 * prevents new admitted background FS work, not hard preemption of in-flight
 * flash calls, and does not alter audio format/DSP scheduling parameters. */
esp_err_t pet_p4_audio_capture_start(bool session_queue_empty);
esp_err_t pet_p4_audio_capture_stop(void);
esp_err_t pet_p4_audio_capture_start_owned(bool session_queue_empty,const char *owner);
esp_err_t pet_p4_audio_capture_stop_owned(const char *owner);

/* 实时对话：PC 经 audio/conversation 打开后麦克风持续采集上行（不受
 * 30 s 上限）；AEC/VAD 可用时支持双向语音，旧客户端半双工播放期间不上行。
 * PC 经 audio/play_begin/chunk/end/flush
 * 流式下发 16 kHz PCM，设备缓冲约 200 ms 后起播。 */
#define PET_P4_AUDIO_STREAM_CHUNK_MAX 4096
#define PET_P4_AUDIO_STREAM_CHUNK_B64_MAX (((PET_P4_AUDIO_STREAM_CHUNK_MAX + 2) / 3) * 4 + 4)
esp_err_t pet_p4_audio_conversation_set(bool enabled, bool half_duplex, const char *session_id);
#if PET_P4_SYSTEM_WIFI
#define PET_P4_AUDIO_WIFI_FRAME_BYTES 640U
typedef uint32_t pet_p4_audio_wifi_session_t;
typedef void (*pet_p4_audio_wifi_capture_fn)(void *ctx, pet_p4_audio_wifi_session_t session,
    uint64_t now_ms, const uint8_t pcm[PET_P4_AUDIO_WIFI_FRAME_BYTES], bool speech);
typedef enum {
  PET_P4_AUDIO_WIFI_PLAY_BEGIN,
  PET_P4_AUDIO_WIFI_PLAY_FRAME,
  PET_P4_AUDIO_WIFI_PLAY_END,
  PET_P4_AUDIO_WIFI_PLAY_FLUSH,
} pet_p4_audio_wifi_play_t;

/* Trusted Start admission belongs to the LAN caller. The tap may only try
 * bounded queue admission: no waiting, I/O or audio reentry. PCM is valid
 * only for the duration of the call. Stop synchronously detaches the tap.
 * Poll live from the network owner and stop on fault or transport loss;
 * do not retain a lock required by the tap when calling stop. */
esp_err_t pet_p4_audio_wifi_start(pet_p4_audio_wifi_capture_fn tap, void *ctx,
                                 pet_p4_audio_wifi_session_t *session);
esp_err_t pet_p4_audio_wifi_ptt_start(pet_p4_audio_wifi_capture_fn tap, bool queue_empty,
                                     pet_p4_audio_wifi_session_t *session);
esp_err_t pet_p4_audio_wifi_ptt_enable(bool enabled);
bool pet_p4_audio_wifi_live(pet_p4_audio_wifi_session_t session);
esp_err_t pet_p4_audio_wifi_stop(pet_p4_audio_wifi_session_t session);
/* Consumes the latest exact playback turn drained by the speaker worker.
 * Zero means no receipt. Empty, flushed and failed turns do not qualify. */
uint32_t pet_p4_audio_wifi_take_drained_turn(pet_p4_audio_wifi_session_t session);
esp_err_t pet_p4_audio_wifi_play(pet_p4_audio_wifi_session_t session, uint32_t turn,
                                pet_p4_audio_wifi_play_t op, const uint8_t *pcm, size_t bytes);
#endif
bool pet_p4_audio_conversation_active(void);
/* Coordination observation only; does not alter the AEC/capture pipeline. */
bool pet_p4_audio_conversation_video_ready(void);
bool pet_p4_audio_ptt_video_ready(void);
esp_err_t pet_p4_audio_stream_begin(const char *session_id);
bool pet_p4_audio_stream_matches(const char *session_id);
esp_err_t pet_p4_audio_stream_push(const uint8_t *pcm, size_t length);
/* Wired protocol ingress: atomic session check + bounded copy, no codec wait.
 * A stale/cancelled session is ignored with ESP_OK, just like play_chunk. */
esp_err_t pet_p4_audio_stream_push_session(const char *session_id, const uint8_t *pcm, size_t length);
esp_err_t pet_p4_audio_stream_end(void);
void pet_p4_audio_stream_flush(void);
unsigned int pet_p4_audio_stream_played_bytes(void);
esp_err_t pet_p4_audio_music_begin(const char *session_id);
bool pet_p4_audio_music_active(void);
bool pet_p4_audio_music_paused(void);
bool pet_p4_audio_music_ended(void);
void pet_p4_audio_music_pause(bool paused);
void pet_p4_audio_music_volume(unsigned int volume);
unsigned int pet_p4_audio_music_buffered_ms(void);
bool pet_p4_audio_stream_playing(void);
/* Metadata-only device diagnostic events: no PCM or spoken text. */
void pet_p4_audio_diagnostic(const char *event, const char *session_id, size_t bytes, bool ok);
void pet_p4_audio_process(
  const pet_p4_runtime_state_t *state,
  unsigned long long now_ms
);
/* Play one embedded UI/system cue (permission request/approved/denied/expired).
 * Unlike lifecycle cues this is event-driven and may repeat for another request. */
esp_err_t pet_p4_audio_play_event_cue(const char *family);
void pet_p4_audio_send_status(void);
void pet_p4_audio_add_completion_counters(cJSON *payload);
/* Cold master-volume persistence gate. No hardware/flash work here. */
bool pet_p4_audio_master_idle(void);

#ifdef __cplusplus
}
#endif
