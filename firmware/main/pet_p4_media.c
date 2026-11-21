#include "pet_p4_media.h"
#include "cJSON.h"
#include "pet_p4_media_codec.h"
#include "pet_p4_audio.h"
#include "pet_p4_miniapp.h"
#if PET_P4_SYSTEM_WIFI
#include "pet_p4_device_wifi.h"
#include "pet_p4_radio.h"
#include "pet_p4_radio_audio.h"
#endif
#include "pet_p4_service_bridge.h"
#if PET_SERVICE_BRIDGE_ENABLED
#include "pet_p4_app_host.h"
#endif
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mbedtls/base64.h"
#include <string.h>
#include <stdio.h>
#include <math.h>
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#else
#define EXT_RAM_BSS_ATTR
#endif
static SemaphoreHandle_t lock;
/* Task-only catalog text is not DMA/ISR state. Keep its capacity without
 * taking the internal RAM needed by codecs, USB and task stacks. */
static EXT_RAM_BSS_ATTR pet_p4_media_view_t view;
static char session[64];
static uint32_t next_seq,offset_ms;
static uint64_t last_request;
static uint8_t *cover;
static size_t cover_bytes;
static EXT_RAM_BSS_ATTR char queue_keys[PET_P4_MEDIA_QUEUE_MAX][180];
typedef struct {uint32_t at_ms;char text[PET_P4_MEDIA_LYRIC_BYTES];} lyric_line_t;
static lyric_line_t *lyrics;
static uint32_t lyric_received,lyric_total;
static pet_p4_send_line_fn send_line;
static void *send_ctx;
void pet_p4_media_transport(pet_p4_send_line_fn send,void *ctx){
  if(!lock)return;xSemaphoreTake(lock,portMAX_DELAY);send_line=send;send_ctx=ctx;xSemaphoreGive(lock);
}
static const char *str(const cJSON *v,const char *k) {const cJSON *s=cJSON_GetObjectItemCaseSensitive(v,k);return cJSON_IsString(s)?s->valuestring:"";}
static bool number(const cJSON *v,const char *k,uint32_t max,uint32_t *out) {
  const cJSON *n=cJSON_GetObjectItemCaseSensitive(v,k);
  if (!cJSON_IsNumber(n)||!isfinite(n->valuedouble)||n->valuedouble<0||n->valuedouble>max||floor(n->valuedouble)!=n->valuedouble) return false;
  *out=(uint32_t)n->valuedouble;return true;
}
static void copy(char *out,size_t size,const char *s) {
  size_t n=strlen(s);if(n>=size)n=size-1;
  while(n && ((unsigned char)s[n]&0xc0)==0x80)n--;
  memcpy(out,s,n);out[n]=0;
}
static void emit(const char *topic,cJSON *payload) {
  cJSON *root=cJSON_CreateObject();cJSON_AddStringToObject(root,"topic",topic);cJSON_AddItemToObject(root,"payload",payload);
  char *line=cJSON_PrintUnformatted(root);if(line&&send_line)send_line(line,send_ctx);cJSON_free(line);cJSON_Delete(root);
}
static bool library_valid(const cJSON *p) {
  const cJSON *q=cJSON_GetObjectItemCaseSensitive(p,"queue");
  const cJSON *keys=cJSON_GetObjectItemCaseSensitive(p,"keys");
  if(!cJSON_IsArray(q)||cJSON_GetArraySize(q)>PET_P4_MEDIA_QUEUE_MAX||!cJSON_IsArray(keys)||cJSON_GetArraySize(keys)!=cJSON_GetArraySize(q))return false;
  for(int i=0;i<cJSON_GetArraySize(q);i++) {
    const cJSON *title=cJSON_GetArrayItem(q,i),*key=cJSON_GetArrayItem(keys,i);
    if(!cJSON_IsString(title)||!cJSON_IsString(key)||!key->valuestring[0]||strlen(key->valuestring)>=180)return false;
    if(cJSON_GetArraySize(q)>20 && (strlen(key->valuestring)>80||strlen(title->valuestring)>90))return false;
  }
  return true;
}
static uint32_t library_generation;
static void apply_library(const cJSON *p) {
  char selected_key[180]="";
  if(view.selected<view.count)copy(selected_key,sizeof(selected_key),queue_keys[view.selected]);
  view.queue_revision=++library_generation;
  const cJSON *q=cJSON_GetObjectItemCaseSensitive(p,"queue"),*keys=cJSON_GetObjectItemCaseSensitive(p,"keys");
  view.count=cJSON_GetArraySize(q);
  for(int i=0;i<view.count;i++) {copy(view.queue[i],91,cJSON_GetArrayItem(q,i)->valuestring);copy(queue_keys[i],180,cJSON_GetArrayItem(keys,i)->valuestring);}
  if(view.selected>=view.count)view.selected=0;
  for(int i=0;i<view.count;i++)if(!strcmp(selected_key,queue_keys[i])){view.selected=i;break;}
}
#if PET_P4_SYSTEM_WIFI
typedef struct { const char *id, *title; } live_item_t;
typedef struct {
  const char *widget_id, *source;
  unsigned preset_count;
  bool (*preset)(unsigned, live_item_t *);
  bool (*lookup)(const char *, live_item_t *);
  bool (*submit)(const char *, uint32_t);
} live_provider_t;

static bool qingting_item(const pet_radio_station_t *station, live_item_t *item) {
  if (!station) return false;
  *item = (live_item_t){station->id, station->title};
  return true;
}
static bool qingting_preset(unsigned index, live_item_t *item) {
  return qingting_item(pet_radio_station_at(index), item);
}
static bool qingting_lookup(const char *id, live_item_t *item) {
  return qingting_item(pet_radio_station_find(id), item);
}
static bool qingting_submit(const char *id, uint32_t generation) {
  const pet_radio_station_t *station = pet_radio_station_find(id);
  return station && pet_radio_submit(station, generation);
}

typedef struct {
  char id[3], title[91];
  pet_wifi_endpoint_t endpoint;
  bool live;
} wifi_audio_item_t;

static EXT_RAM_BSS_ATTR wifi_audio_item_t wifi_audio_items[PET_P4_MEDIA_WIFI_PLAYLIST_MAX];
static unsigned wifi_audio_count;
static uint32_t wifi_audio_revision;
static char wifi_audio_widget[64];

static bool wifi_audio_read_item(const cJSON *row, unsigned index, wifi_audio_item_t *out) {
  if (!cJSON_IsObject(row)) return false;
  const cJSON *title_value = NULL, *url = NULL, *live = NULL;
  for (const cJSON *field = row->child; field; field = field->next) {
    if (!field->string) return false;
    if (!strcmp(field->string, "title") && !title_value) title_value = field;
    else if (!strcmp(field->string, "url") && !url) url = field;
    else if (!strcmp(field->string, "live") && !live) live = field;
    else return false;
  }
  if (!cJSON_IsString(title_value) || !cJSON_IsString(url)) return false;
  const char *title = title_value->valuestring;
  size_t length = strlen(title);
  if (!length || length >= sizeof(out->title) || (live && !cJSON_IsBool(live))) return false;
  for (size_t i = 0; i < length; ++i)
    if ((unsigned char)title[i] < 32 || title[i] == 127) return false;
  *out = (wifi_audio_item_t){0};
  if (!pet_wifi_endpoint_parse(url->valuestring, &out->endpoint)) return false;
  out->id[0] = 'w';
  out->id[1] = 'a' + index;
  memcpy(out->title, title, length + 1);
  out->live = cJSON_IsTrue(live);
  return true;
}

bool pet_p4_media_wifi_playlist_valid(const cJSON *playlist) {
  int count = cJSON_IsArray(playlist) ? cJSON_GetArraySize(playlist) : 0;
  if (count < 1 || count > PET_P4_MEDIA_WIFI_PLAYLIST_MAX) return false;
  wifi_audio_item_t item;
  const cJSON *row = playlist->child;
  for (int i = 0; i < count; ++i, row = row->next)
    if (!wifi_audio_read_item(row, i, &item)) return false;
  return true;
}

static bool wifi_audio_preset(unsigned index, live_item_t *item) {
  if (index >= wifi_audio_count) return false;
  *item = (live_item_t){wifi_audio_items[index].id, wifi_audio_items[index].title};
  return true;
}

static bool wifi_audio_lookup(const char *id, live_item_t *item) {
  return id[0] == 'w' && id[1] >= 'a' && id[1] < 'a' + PET_P4_MEDIA_WIFI_PLAYLIST_MAX &&
    !id[2] && wifi_audio_preset(id[1] - 'a', item);
}

static bool wifi_audio_submit(const char *id, uint32_t generation) {
  live_item_t item;
  if (!wifi_audio_lookup(id, &item)) return false;
  const wifi_audio_item_t *track = &wifi_audio_items[id[1] - 'a'];
  return pet_network_audio_submit_public(&track->endpoint, track->live, generation);
}

static const live_provider_t wifi_audio_provider = {
  NULL, PET_P4_MEDIA_WIFI_SOURCE, PET_P4_MEDIA_WIFI_PLAYLIST_MAX,
  wifi_audio_preset, wifi_audio_lookup, wifi_audio_submit,
};

/* Device network providers are registered by firmware. The runtime validates
 * that a component's live source matches its identity before requesting one. */
static const live_provider_t live_providers[] = {
  {"city-radio", PET_RADIO_SOURCE, PET_RADIO_OFFLINE_COUNT,
    qingting_preset, qingting_lookup, qingting_submit},
};
static const live_provider_t *live_provider;
static bool radio_direct, radio_remote, radio_format_rejected, radio_pc_connected;
static bool radio_draining;
static bool radio_reconnecting;
static bool radio_autoplay_pending;
/* Only this fresh entry, before any PCM ownership, may hand off a known format failure. */
static bool radio_auto_entry, radio_fallback_pending;
static char radio_fallback_key[180];
static uint32_t radio_serial, radio_generation;
static char radio_owner[64];
static live_item_t local_item;

static bool live_source(const char *source) {
  return live_provider && !strcmp(source, live_provider->source);
}

static void radio_reset_locked(void) {
  radio_generation = 0;
  radio_draining = radio_reconnecting = false;
  radio_owner[0] = 0;
}

static void radio_cancel_locked(const char *status, const char *message) {
  radio_auto_entry = radio_fallback_pending = false;
  radio_fallback_key[0] = 0;
  if (radio_owner[0]) pet_p4_audio_music_flush_if_owner(radio_owner);
  radio_reset_locked();
  copy(view.status, sizeof(view.status), status);
  copy(view.message, sizeof(view.message), message);
  view.position_ms = 0;
  view.level = 0;
}

static void radio_add_local_stations(void) {
  for (unsigned i = 0; i < live_provider->preset_count && view.count < PET_P4_MEDIA_QUEUE_MAX; ++i) {
    live_item_t station;
    if (!live_provider->preset(i, &station)) break;
    unsigned row = 0;
    while (row < view.count && strcmp(queue_keys[row], station.id)) ++row;
    if (row < view.count) continue;
    copy(queue_keys[row], sizeof(queue_keys[row]), station.id);
    copy(view.queue[row], sizeof(view.queue[row]), station.title);
    ++view.count;
  }
  view.queue_revision = ++library_generation;
}

static bool radio_owns_controls(const char *source) {
  return live_source(source) && radio_direct && !radio_remote;
}

static void radio_start_locked(live_item_t station) {
  radio_cancel_locked("buffering", live_provider == &wifi_audio_provider
    ? "正在连接 Wi-Fi 音频" : "正在连接 Wi-Fi 电台");
  local_item = station;
  radio_format_rejected = false;
  session[0] = 0;
  view.cover_ready = view.lyrics_ready = false;
  view.artist[0] = 0;
  view.duration_ms = offset_ms = 0;
  copy(view.title, sizeof(view.title), station.title);
  if (radio_serial != UINT32_MAX && live_provider->submit(station.id, ++radio_serial)) {
    radio_generation = radio_serial;
  } else {
    radio_cancel_locked("error", "无法启动设备音频");
  }
}

static bool radio_current_locked(uint32_t generation) {
  return generation && generation == radio_generation && radio_owns_controls(view.source);
}

bool pet_p4_media_radio_current(uint32_t generation) {
  xSemaphoreTake(lock, portMAX_DELAY);
  bool current = radio_current_locked(generation);
  xSemaphoreGive(lock);
  return current;
}

bool pet_p4_media_radio_reconnecting(uint32_t generation) {
  xSemaphoreTake(lock, portMAX_DELAY);
  bool current = radio_current_locked(generation);
  if (current && radio_owner[0] && !pet_p4_audio_music_flush_if_owner(radio_owner)) {
    radio_cancel_locked("interrupted", "音频已被其他会话接管");
    current = false;
  }
  if (current) {
    radio_owner[0] = 0;
    radio_reconnecting = true;
    view.level = view.position_ms = 0;
    copy(view.status, sizeof(view.status), "buffering");
    copy(view.message, sizeof(view.message), "正在重新连接 Wi-Fi 音频");
  }
  xSemaphoreGive(lock);
  return current;
}

pet_radio_write_t pet_p4_media_radio_write(uint32_t generation, const int16_t *pcm, size_t samples) {
  xSemaphoreTake(lock, portMAX_DELAY);
  pet_radio_write_t result = PET_RADIO_CANCELLED;
  if (radio_current_locked(generation)) {
    radio_auto_entry = false; /* Once PCM is delivered, a later stop is never automatic fallback. */
    if (!radio_owner[0]) {
      snprintf(radio_owner, sizeof(radio_owner), "music-radio-device-%lu", (unsigned long)generation);
      if (pet_p4_audio_music_active() || pet_p4_audio_music_begin(radio_owner) != ESP_OK) {
        radio_cancel_locked("interrupted", "音频正在使用，请稍后继续收听");
        goto done;
      }
      pet_p4_audio_music_volume(view.volume);
      radio_reconnecting = false;
      view.message[0] = 0;
    }
    if (!pet_p4_audio_music_active()) {
      radio_cancel_locked("interrupted", "语音优先，广播已停止");
    /* Music playback starts at 400 ms in the shared audio ring. */
    } else if (pet_p4_audio_music_buffered_ms() >= 600) {
      result = PET_RADIO_BACKPRESSURE;
    } else {
      esp_err_t pushed = pet_p4_audio_music_push_if_owner(radio_owner, (const uint8_t *)pcm,
                                                        samples * sizeof(*pcm));
      if (pushed == ESP_OK) {
        unsigned peak = 0;
        for (size_t i = 0; i < samples; ++i) {
          unsigned magnitude = pcm[i] < 0 ? -pcm[i] : pcm[i];
          if (magnitude > peak) peak = magnitude;
        }
        view.level = peak * 100 / 32768;
        result = PET_RADIO_WRITTEN;
      } else if (pushed == ESP_ERR_NO_MEM) {
        result = PET_RADIO_BACKPRESSURE;
      } else {
        radio_cancel_locked("interrupted", "语音优先，广播已停止");
      }
    }
  }
done:
  xSemaphoreGive(lock);
  return result;
}

static bool radio_prepare_pc_fallback_locked(const char *key) {
  if (live_provider == &wifi_audio_provider || !radio_pc_connected || !send_line ||
      !key || !key[0] || pet_p4_audio_music_active() || pet_p4_audio_active() ||
      pet_p4_audio_conversation_active()) return false;
  copy(radio_fallback_key, sizeof(radio_fallback_key), key);
  radio_fallback_pending = true;
  return true;
}

void pet_p4_media_radio_finished(uint32_t generation, pet_radio_error_t error) {
  xSemaphoreTake(lock, portMAX_DELAY);
  if (radio_current_locked(generation)) {
    if (error == PET_RADIO_COMPLETE && pet_p4_audio_music_end_if_owner(radio_owner)) {
      radio_draining = true;
      xSemaphoreGive(lock);
      return;
    }
    const char *message = error == PET_RADIO_FORMAT_UNSUPPORTED ? "设备不支持该音频流，请在 PC 播放"
      : error == PET_RADIO_OUT_OF_MEMORY ? "设备内存不足，无法播放电台"
      : error == PET_RADIO_CLOCK_UNAVAILABLE ? "设备正在校时，请稍后继续收听"
      : error == PET_RADIO_CONNECT_FAILED ? "无法建立电台安全连接，请重试"
      : error == PET_RADIO_REQUEST_FAILED ? "电台请求未发出，请重试"
      : error == PET_RADIO_RESPONSE_FAILED ? "电台未返回可用响应，请换台或重试"
      : "电台音频流中断，请换台或重试";
    if (error == PET_RADIO_WIFI_DISCONNECTED) {
      message = "Wi-Fi 已断开，请重新播放";
    } else if (live_provider == &wifi_audio_provider) {
      message = error == PET_RADIO_FORMAT_UNSUPPORTED ? "仅支持 MP3 和 16 位 PCM WAV"
        : error == PET_RADIO_OUT_OF_MEMORY ? "设备内存不足，无法播放音频"
        : error == PET_RADIO_CLOCK_UNAVAILABLE ? "设备正在校时，请稍后重试"
        : error == PET_RADIO_CONNECT_FAILED ? "无法建立音频安全连接，请重试"
        : error == PET_RADIO_REQUEST_FAILED ? "音频请求未发出，请重试"
        : error == PET_RADIO_RESPONSE_FAILED ? "音频服务未返回可用响应"
        : "音频播放中断，请重试";
    }
    bool fallback = radio_auto_entry && error == PET_RADIO_FORMAT_UNSUPPORTED &&
      !radio_owner[0] && !pet_p4_audio_music_active();
    radio_cancel_locked("error", message);
    radio_format_rejected = error == PET_RADIO_FORMAT_UNSUPPORTED;
    if (fallback) radio_prepare_pc_fallback_locked(local_item.id);
  }
  xSemaphoreGive(lock);
}

static void radio_offer_local_locked(bool entering) {
  radio_direct = true;
  if (!live_source(view.source)) {
    uint8_t volume = view.volume;
    memset(&view, 0, sizeof(view));
    view.volume = volume;
    view.lyric_index = -1;
    copy(view.source, sizeof(view.source), live_provider->source);
  }
  radio_add_local_stations();
  local_item = (live_item_t){0};
  if (view.selected < view.count) live_provider->lookup(queue_keys[view.selected], &local_item);
  if (entering && !local_item.id && view.selected < view.count &&
      radio_prepare_pc_fallback_locked(queue_keys[view.selected])) {
    copy(view.status, sizeof(view.status), "error");
    copy(view.message, sizeof(view.message), "此电台需要 PC 播放");
    view.page = 0;
    return;
  }
  if (entering && !local_item.id) live_provider->preset(0, &local_item);
  for (unsigned i = 0; local_item.id && i < view.count; ++i)
    if (!strcmp(queue_keys[i], local_item.id)) { view.selected = i; break; }
  view.page = 0;
  if (entering && local_item.id) {
    radio_start_locked(local_item);
    radio_auto_entry = radio_generation && live_provider != &wifi_audio_provider;
  }
  else if (live_provider == &wifi_audio_provider) {
    if (local_item.id) copy(view.title, sizeof(view.title), local_item.title);
    radio_cancel_locked("idle", pet_device_wifi_online() ? "请选择音频播放" : "请先连接 Wi-Fi");
  } else radio_cancel_locked("idle", local_item.id ? "可在设备上继续收听" : "请选择支持设备播放的电台");
}

void pet_p4_media_authorize_live_playlist_revision(const char *widget_id, const char *source,
                                                   bool visible, bool pc_connected, uint32_t revision,
                                                   const cJSON *playlist) {
  if (!lock) return;
  const live_provider_t *provider = NULL;
  bool same_playlist = false;
  xSemaphoreTake(lock, portMAX_DELAY);
  if (visible && widget_id && widget_id[0] && source) {
    if (!strcmp(source, PET_P4_MEDIA_WIFI_SOURCE) && strlen(widget_id) < sizeof(wifi_audio_widget)) {
      same_playlist = live_provider == &wifi_audio_provider && wifi_audio_revision == revision &&
        !strcmp(wifi_audio_widget, widget_id);
      if (same_playlist || pet_p4_media_wifi_playlist_valid(playlist)) provider = &wifi_audio_provider;
    }
    for (unsigned i = 0; i < sizeof(live_providers)/sizeof(live_providers[0]); ++i)
      if (!strcmp(widget_id, live_providers[i].widget_id) && !strcmp(source, live_providers[i].source)) {
        provider = &live_providers[i];
        break;
      }
  }
  const bool wifi_audio = provider == &wifi_audio_provider;
  const bool changed = provider != live_provider || (wifi_audio && !same_playlist);
  const bool entering = provider && changed;
  radio_pc_connected = pc_connected;
  if (!pc_connected) {
    radio_fallback_pending = false;
    radio_fallback_key[0] = 0;
  }
  if (changed) {
    if (radio_generation || (radio_direct && !radio_remote && live_source(view.source)))
      radio_cancel_locked("idle", "已停止播放");
    radio_direct = radio_remote = radio_format_rejected = false;
    radio_auto_entry = radio_fallback_pending = false;
    radio_fallback_key[0] = 0;
    local_item = (live_item_t){0};
    live_provider = provider;
    if (!strcmp(view.source, PET_P4_MEDIA_WIFI_SOURCE)) view.source[0] = 0;
    radio_autoplay_pending = entering && !wifi_audio;
    if (wifi_audio) {
      copy(wifi_audio_widget, sizeof(wifi_audio_widget), widget_id);
      wifi_audio_revision = revision;
      wifi_audio_count = cJSON_GetArraySize(playlist);
      const cJSON *row = playlist->child;
      for (unsigned i = 0; i < wifi_audio_count; ++i, row = row->next)
        wifi_audio_read_item(row, i, &wifi_audio_items[i]);
    } else {
      wifi_audio_widget[0] = 0;
      wifi_audio_count = 0;
    }
  }
  if (provider) {
    if (!pc_connected && radio_remote) {
      if (live_source(view.source) && session[0]) {
        pet_p4_audio_music_flush_if_owner(session);
        session[0] = 0;
        /* PC begin has a title but no selected key. Keep the selection only
         * when the active title maps to one exact row. */
        unsigned row = view.count;
        for (unsigned i = 0; view.title[0] && i < view.count; ++i) {
          if (strcmp(view.title, view.queue[i])) continue;
          if (row != view.count) { row = view.count; break; }
          row = i;
        }
        if (row < view.count) view.selected = row;
      }
      radio_remote = radio_direct = radio_format_rejected = false;
      local_item = (live_item_t){0};
    }
    if (!radio_direct && !radio_remote && (wifi_audio || pet_device_wifi_online()) && !pet_p4_audio_music_active()) {
      radio_offer_local_locked(radio_autoplay_pending);
      radio_autoplay_pending = false;
    }
  }
  xSemaphoreGive(lock);
}

void pet_p4_media_authorize_live(const char *widget_id, const char *source, bool visible,
                                 bool pc_connected) {
  pet_p4_media_authorize_live_playlist_revision(widget_id, source, visible, pc_connected, 0, NULL);
}

/* Returns false when the caller should forward the operation to the PC. */
static bool radio_action_locked(const char *source, const char *operation, int volume) {
  if (!radio_owns_controls(source)) return false;
  if (!strcmp(operation, "sync")) return true;
  if (!strcmp(operation, "volume") || !strcmp(operation, "volume_up") || !strcmp(operation, "volume_down")) {
    if (volume < 0) volume = view.volume + (!strcmp(operation, "volume_up") ? 5 : -5);
    view.volume = volume < 0 ? 0 : volume > 100 ? 100 : volume;
    if (radio_owner[0]) pet_p4_audio_music_volume(view.volume);
    return true;
  }
  if (!strcmp(operation, "toggle") || !strcmp(operation, "select") ||
      !strcmp(operation, "next") || !strcmp(operation, "previous")) {
    radio_auto_entry = radio_fallback_pending = radio_autoplay_pending = false;
    radio_fallback_key[0] = 0;
  }
  if (!strcmp(operation, "toggle")) {
    if (radio_generation) radio_cancel_locked("paused", "已暂停");
    else if (local_item.id) {
      if (live_provider != &wifi_audio_provider && radio_pc_connected && send_line &&
          (radio_format_rejected || !pet_device_wifi_online())) {
        for (unsigned i = 0; i < view.count; ++i) {
          if (!strcmp(queue_keys[i], local_item.id)) {
            view.selected = i;
            radio_remote = true;
            return false;
          }
        }
      }
      if (pet_device_wifi_online()) radio_start_locked(local_item);
      else radio_cancel_locked("error", "Wi-Fi 未连接，请连接后继续收听");
    } else {
      /* A selected HLS/PC-only row has no local_item. A later explicit
       * play must still reach the PC after it reconnects. Do not hand off
       * merely on reconnect, or submit this row to the local MP3 decoder. */
      if (live_provider != &wifi_audio_provider && radio_pc_connected && send_line &&
          view.selected < view.count) {
        radio_remote = true;
        return false;
      }
      radio_cancel_locked("idle", "请选择支持设备播放的电台");
    }
    return true;
  }
  if (!view.count) return true;
  unsigned row = view.selected;
  if (strcmp(operation, "select")) {
    if (local_item.id) {
      for (unsigned i = 0; i < view.count; ++i)
        if (!strcmp(queue_keys[i], local_item.id)) { row = i; break; }
    }
    row = (row + view.count + (!strcmp(operation, "previous") ? -1 : 1)) % view.count;
  }
  view.selected = row;
  live_item_t station = {0};
  bool supported = live_provider->lookup(queue_keys[row], &station);
  if (supported && pet_device_wifi_online()) {
    view.page = 0;
    radio_start_locked(station);
    return true;
  }
  local_item = station;
  radio_cancel_locked("idle", supported ? "Wi-Fi 未连接，请连接后继续收听" : "此电台需要 PC 播放");
  if (live_provider == &wifi_audio_provider || !radio_pc_connected || !send_line) return true;
  radio_remote = true;
  return false;
}
#endif

void pet_p4_media_visibility(const char *source,bool visible,uint64_t now_ms) {
  static char prior[48];static bool was_visible;static uint32_t epoch,boot;static uint64_t last;
  if(!lock)return;
  xSemaphoreTake(lock,portMAX_DELAY);
  if(!boot)boot=esp_random()|1;
  if(was_visible&&(!visible||strcmp(prior,source))){
    cJSON *p=cJSON_CreateObject();cJSON_AddStringToObject(p,"source",prior);cJSON_AddStringToObject(p,"operation","visibility");
    cJSON_AddNumberToObject(p,"boot",boot);cJSON_AddNumberToObject(p,"epoch",++epoch);cJSON_AddBoolToObject(p,"visible",false);emit("media/event",p);was_visible=false;
  }
#if PET_P4_SYSTEM_WIFI
  if (!strcmp(source, PET_P4_MEDIA_WIFI_SOURCE)) visible = false;
#endif
  if(visible&&source[0]){
    if(!was_visible||strcmp(prior,source)){copy(prior,sizeof(prior),source);++epoch;last=0;}
    was_visible=true;
#if PET_P4_SYSTEM_WIFI
    if (radio_fallback_pending && live_source(source)) {
      radio_fallback_pending = false;
      if (radio_pc_connected && send_line && !pet_p4_audio_music_active() &&
          !pet_p4_audio_active() && !pet_p4_audio_conversation_active()) {
        /* A distinct visibility epoch makes this one handoff an entry, even
         * if the PC already saw deviceDirect in the current component. */
        /* Keep deviceDirect until PC begin succeeds. Legacy PCs ignore the
         * fallback metadata and must not autoplay their stale current row. */
        ++epoch;last=0;
      } else radio_fallback_key[0] = 0;
    }
#endif
    if(send_line&&(!last||now_ms-last>=2000)){
      cJSON *p=cJSON_CreateObject();cJSON_AddStringToObject(p,"source",source);cJSON_AddStringToObject(p,"operation","visibility");
      cJSON_AddNumberToObject(p,"boot",boot);cJSON_AddNumberToObject(p,"epoch",epoch);cJSON_AddBoolToObject(p,"visible",true);
#if PET_P4_SYSTEM_WIFI
      if(radio_owns_controls(source))cJSON_AddBoolToObject(p,"deviceDirect",true);
      if(live_source(source) && radio_fallback_key[0]) {
        cJSON_AddStringToObject(p,"fallback","format_unsupported");
        cJSON_AddStringToObject(p,"key",radio_fallback_key);
      }
#endif
      emit("media/event",p);last=now_ms;
    }
  }
  xSemaphoreGive(lock);
}
void pet_p4_media_init(void) {
  if(lock)return;lock=xSemaphoreCreateMutex();view.volume=65;
  view.lyric_index=-1;
  copy(view.status,sizeof(view.status),"idle");copy(view.message,sizeof(view.message),"在 PC 搜索歌曲或用实时对话点歌");
}
static void refresh(void) {
#if PET_P4_SYSTEM_WIFI
  if(radio_owner[0]) {
    if(radio_draining && pet_p4_audio_music_finish_if_owner(radio_owner, &view.position_ms)) {
      view.duration_ms = view.position_ms;
      radio_reset_locked();
      copy(view.status, sizeof(view.status), "ended");
    }else if(!pet_p4_audio_music_active()||!pet_p4_audio_stream_matches(radio_owner)){
      radio_cancel_locked("interrupted","语音优先，广播已停止");
    }else{
      view.position_ms=pet_p4_audio_stream_played_bytes()/96;
      copy(view.status,sizeof(view.status),pet_p4_audio_stream_playing()?"playing":"buffering");
    }
    return;
  }
#endif
  if(!session[0])return;
  if(!pet_p4_audio_music_active()) {
    if(strcmp(view.status,"error")) {copy(view.status,sizeof(view.status),"interrupted");copy(view.message,sizeof(view.message),"语音优先，音乐已暂停");}
    return;
  }
  view.position_ms=offset_ms+pet_p4_audio_stream_played_bytes()/96;
  const char *s=pet_p4_audio_music_paused()?"paused":pet_p4_audio_music_ended()?"ended":pet_p4_audio_stream_playing()?"playing":"buffering";
  copy(view.status,sizeof(view.status),s);
  if(!strcmp(s,"ended"))view.duration_ms=view.position_ms;
}
bool pet_p4_media_handle(const char *topic,const cJSON *p,pet_p4_send_line_fn send,void *ctx) {
  return pet_p4_media_handle_with_reply(topic,p,send,ctx,NULL);
}
bool pet_p4_media_handle_with_reply(const char *topic,const cJSON *p,
    pet_p4_send_line_fn send,void *ctx,pet_p4_query_reply_t *deferred) {
  /* Reject a used slot before any state mutation; never overwrite/replay it. */
  if(deferred && deferred->line)return false;
  if(strncmp(topic,"media/",6))return false;
  pet_p4_media_init();if(!lock)return false;
  xSemaphoreTake(lock,portMAX_DELAY);send_line=send;send_ctx=ctx;
  bool ok=true;const char *message="";const char *id=str(p,"sessionId");
  uint32_t n=0;bool exact=id[0]&&!strcmp(id,session);
#if PET_SERVICE_BRIDGE_ENABLED
  const char*host_instance=str(p,"hostInstance"),*scoped_source=str(p,"source");
  bool scoped=host_instance[0];
  bool blocked=scoped&&(strncmp(scoped_source,"app.",4)||!pet_app_host_instance_matches(scoped_source+4,host_instance)
    ||(strcmp(topic,"media/begin")&&strcmp(topic,"media/library")&&strcmp(view.source,scoped_source)));
  if(blocked){ok=false;message="media view instance expired";}else
#endif
  if(!strcmp(topic,"media/begin")) {
    uint32_t rate,duration,offset,volume;
    if(!id[0]||strlen(id)>=sizeof(session)||strncmp(id,"music-",6)||strcmp(str(p,"format"),"ima-block-v1")
        ||!number(p,"sampleRate",48000,&rate)||rate!=48000
        ||!number(p,"durationMs",3600000,&duration)||!number(p,"offsetMs",3600000,&offset)
        ||!number(p,"volume",100,&volume)||!library_valid(p)) {ok=false;message="音乐格式或参数无效";}
#if PET_P4_SYSTEM_WIFI
    else if(!strcmp(str(p,"source"), PET_P4_MEDIA_WIFI_SOURCE)) {ok=false;message="Wi-Fi 音频由设备播放";}
#endif
    else if(!strncmp(str(p,"source"),"app.",4)&&!pet_p4_miniapp_host_source(str(p,"source"))) {ok=false;message="请先打开对应组件";}
    else if(pet_p4_audio_music_begin(id)!=ESP_OK) {ok=false;message="麦克风或语音正在使用，音乐未启动";}
    else {
#if PET_P4_SYSTEM_WIFI
      radio_reset_locked();  /* The successful PC begin has replaced audio ownership. */
      if (live_provider == &wifi_audio_provider) radio_direct = false;
      if(live_source(str(p,"source"))) {
        radio_remote=true;
        radio_autoplay_pending=false;
        radio_auto_entry=radio_fallback_pending=false;
        radio_fallback_key[0]=0;
      }
#endif
      const uint8_t page=view.page==2?2:0;
      memset(&view,0,sizeof(view));view.page=page;view.lyric_index=-1;lyric_received=lyric_total=0;
      copy(view.source,sizeof(view.source),str(p,"source"));if(!strcmp(str(p,"mode"),"live"))view.page=0;
      view.volume=volume;view.duration_ms=duration;offset_ms=offset;next_seq=0;cover_bytes=0;
      copy(session,sizeof(session),id);copy(view.title,sizeof(view.title),str(p,"title"));copy(view.artist,sizeof(view.artist),str(p,"artist"));
      apply_library(p);
      pet_p4_audio_music_volume(volume);copy(view.status,sizeof(view.status),"buffering");
    }
  } else if(!strcmp(topic,"media/chunk")) {
    static uint8_t encoded[1926],pcm[7680];size_t bytes=0,written=0;const char *data=str(p,"data");
    if(!exact || !pet_p4_audio_music_active()) {ok=false;message="音乐会话已结束";}
    else if(!number(p,"seq",UINT32_MAX,&n)||n!=next_seq||strlen(data)>2568
      ||mbedtls_base64_decode(encoded,sizeof(encoded),&bytes,(const unsigned char*)data,strlen(data))
      ||!pet_p4_media_decode(encoded,bytes,pcm,sizeof(pcm),&written)) {ok=false;message="音乐数据顺序或格式无效";}
    else if(pet_p4_audio_stream_push(pcm,written)!=ESP_OK) {ok=false;message="音乐缓冲区已满";}
    else {next_seq++;unsigned peak=0;for(size_t i=0;i+1<written;i+=2){int sample=(int16_t)(pcm[i]|pcm[i+1]<<8);unsigned a=sample<0?-sample:sample;if(a>peak)peak=a;}view.level=(uint8_t)(peak*100/32768);}
  } else if(!strcmp(topic,"media/end")) {
    if(!exact) {ok=false;message="音乐会话不匹配";} else pet_p4_audio_stream_end();
  } else if(!strcmp(topic,"media/stop")) {
#if PET_P4_SYSTEM_WIFI
    if(!id[0]||exact) {
      radio_auto_entry = radio_fallback_pending = radio_autoplay_pending = false;
      radio_fallback_key[0] = 0;
    }
    if(!id[0]&&radio_generation)radio_cancel_locked("idle","已停止播放");
#endif
    if(!id[0]||exact) {if(pet_p4_audio_music_active())pet_p4_audio_stream_flush();session[0]=0;copy(view.status,sizeof(view.status),!strcmp(str(p,"state"),"error")?"error":"idle");copy(view.message,sizeof(view.message),str(p,"message"));view.position_ms=0;view.level=0;}
  } else if(!strcmp(topic,"media/control")) {
    const char *op=str(p,"operation");
    if(!strcmp(op,"volume")&&number(p,"volume",100,&n)) {view.volume=n;pet_p4_audio_music_volume(n);}
#if PET_P4_SYSTEM_WIFI
    else if(radio_owns_controls(view.source)&&local_item.id&&
        (!strcmp(op,"pause")||!strcmp(op,"resume")||!strcmp(op,"toggle"))) {
      bool pause=!strcmp(op,"pause")||(!strcmp(op,"toggle")&&radio_generation);
      if(pause)radio_cancel_locked("paused","已暂停");
      else if(!radio_generation&&pet_device_wifi_online())radio_start_locked(local_item);
      else if(!radio_generation){ok=false;message="Wi-Fi 未连接";}
    }
#endif
    else if(!pet_p4_audio_music_active()) {ok=false;message="当前没有播放中的音乐";}
    else if(!strcmp(op,"pause"))pet_p4_audio_music_pause(true);
    else if(!strcmp(op,"resume"))pet_p4_audio_music_pause(false);
    else if(!strcmp(op,"toggle"))pet_p4_audio_music_pause(!pet_p4_audio_music_paused());
    else {ok=false;message="播放器操作无效";}
  } else if(!strcmp(topic,"media/cover")) {
    size_t bytes=0;static uint8_t decoded[2048];const char *data=str(p,"data");
    if(!exact || !number(p,"offset",192*192*2,&n)||n!=cover_bytes||strlen(data)>2732
       ||mbedtls_base64_decode(decoded,sizeof(decoded),&bytes,(const unsigned char*)data,strlen(data))
       ||!bytes||cover_bytes+bytes>192*192*2) {ok=false;message="封面数据无效";}
    else {if(!cover)cover=heap_caps_malloc(192*192*2,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
      if(!cover){ok=false;message="封面内存不足";}else{memcpy(cover+cover_bytes,decoded,bytes);cover_bytes+=bytes;view.cover_ready=cover_bytes==192*192*2;}}
  } else if(!strcmp(topic,"media/lyrics")) {
    uint32_t start,total,at=0;
    const cJSON *lines=cJSON_GetObjectItemCaseSensitive(p,"lines");
    int count=cJSON_IsArray(lines)?cJSON_GetArraySize(lines):-1;
    if(!exact||!number(p,"start",PET_P4_MEDIA_LYRIC_MAX,&start)
        ||!number(p,"total",PET_P4_MEDIA_LYRIC_MAX,&total)||count<0||count>4
        ||start!=lyric_received||start+(uint32_t)count>total
        ||(start && total!=lyric_total)||(!count && total)) {ok=false;message="歌词会话或顺序无效";}
    else {
      uint32_t previous=start?lyrics[start-1].at_ms:0;
      for(int i=0;i<count;i++) {
        const cJSON *line=cJSON_GetArrayItem(lines,i),*text=cJSON_GetObjectItemCaseSensitive(line,"text");
        if(!number(line,"atMs",3600000,&at)||at<previous||!cJSON_IsString(text)
            ||strlen(text->valuestring)>=PET_P4_MEDIA_LYRIC_BYTES) {ok=false;message="歌词格式无效";break;}
        previous=at;
      }
      if(ok && count && !lyrics)lyrics=heap_caps_malloc(sizeof(*lyrics)*PET_P4_MEDIA_LYRIC_MAX,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
      if(ok && count && !lyrics) {ok=false;message="歌词内存不足";}
      if(ok) {
        if(!start){view.lyrics_ready=false;view.lyric_index=-1;lyric_total=total;}
        for(int i=0;i<count;i++) {
          const cJSON *line=cJSON_GetArrayItem(lines,i);number(line,"atMs",3600000,&at);
          lyrics[start+i].at_ms=at;copy(lyrics[start+i].text,sizeof(lyrics[0].text),str(line,"text"));
        }
        lyric_received+=count;view.lyrics_ready=lyric_received==lyric_total;
      }
    }
  } else if(!strcmp(topic,"media/library")) {
#if PET_P4_SYSTEM_WIFI
    if(!strcmp(str(p,"source"), PET_P4_MEDIA_WIFI_SOURCE) ||
       (live_provider == &wifi_audio_provider && radio_direct)) {ok=false;message="Wi-Fi 音频由设备播放";}
    else
#endif
    if(pet_p4_audio_music_active()&&strcmp(str(p,"source"),view.source)) {ok=false;message="其他组件正在播放";}
    else if(!library_valid(p)) {ok=false;message="播放列表格式无效";}
#if PET_P4_SYSTEM_WIFI
    else if(radio_generation&&!live_source(str(p,"source"))){ok=false;message="其他组件正在播放";}
#endif
    else {apply_library(p);copy(view.source,sizeof(view.source),str(p,"source"));
#if PET_P4_SYSTEM_WIFI
      if(radio_owns_controls(view.source))radio_add_local_stations();
#endif
    }
    if(ok&&number(p,"volume",100,&n)) {view.volume=n;pet_p4_audio_music_volume(n);}
  } else if(!strcmp(topic,"media/notice")) {
    const char *source=str(p,"source"),*state=str(p,"state");
    if(
#if PET_P4_SYSTEM_WIFI
       !strcmp(source, PET_P4_MEDIA_WIFI_SOURCE) ||
#endif
       !source[0]||strcmp(source,view.source)||strlen(str(p,"message"))>240
       ||(strcmp(state,"error")&&strcmp(state,"idle")&&strcmp(state,"buffering"))) {ok=false;message="组件提示已过期或无效";}
    else if(!pet_p4_audio_music_active()
#if PET_P4_SYSTEM_WIFI
      && !radio_generation && !radio_owns_controls(view.source)
#endif
    ) {
      session[0]=0;copy(view.status,sizeof(view.status),state);copy(view.message,sizeof(view.message),str(p,"message"));
    }
  } else if(!strcmp(topic,"media/metadata")) {
    if(!exact || !number(p,"durationMs",3600000,&n)) {ok=false;message="曲目信息已过期";}else view.duration_ms=n;
  } else if(!strcmp(topic,"media/query")) {
    if(id[0]&&!exact) {ok=false;message="音乐会话不匹配";}
  } else {ok=false;message="未知媒体指令";}
  if(ok && strcmp(topic,"media/library") && strcmp(topic,"media/notice"))last_request=esp_timer_get_time()/1000ULL;
  refresh();
  cJSON *reply=cJSON_CreateObject();
  cJSON_AddBoolToObject(reply,"ok",ok);cJSON_AddStringToObject(reply,"requestId",str(p,"requestId"));
  cJSON_AddStringToObject(reply,"sessionId",session);cJSON_AddStringToObject(reply,"state",view.status);
  cJSON_AddStringToObject(reply,"message",ok?view.message:message);
  cJSON_AddNumberToObject(reply,"playedBytes",pet_p4_audio_music_active()?pet_p4_audio_stream_played_bytes():0);
  cJSON_AddNumberToObject(reply,"positionMs",view.position_ms);
  cJSON_AddNumberToObject(reply,"bufferedMs",pet_p4_audio_music_buffered_ms());
  cJSON_AddNumberToObject(reply,"volume",view.volume);
  if(deferred && send && !strcmp(topic,"media/chunk")) {
    /* Snapshot the complete status under media lock, without registering a
     * temporary callback as the persistent event transport. The worker owns
     * this single serialization until TX on the request route after unlock. */
    cJSON *root=cJSON_CreateObject();
    if(root && reply) {
      cJSON_AddStringToObject(root,"topic","media/status");
      cJSON_AddItemToObject(root,"payload",reply);
      deferred->line=cJSON_PrintUnformatted(root);
    } else cJSON_Delete(reply);
    cJSON_Delete(root);
    xSemaphoreGive(lock);return true;
  }
  emit("media/status",reply);xSemaphoreGive(lock);return true;
}
void pet_p4_media_tick(uint64_t now) {
  if(!lock)return;xSemaphoreTake(lock,portMAX_DELAY);
#if PET_P4_SYSTEM_WIFI
  if ((radio_auto_entry || radio_fallback_pending) &&
      (pet_p4_audio_active() || pet_p4_audio_conversation_active())) {
    radio_auto_entry = radio_fallback_pending = false;
    radio_fallback_key[0] = 0;
  }
  if (radio_reconnecting && (pet_p4_audio_active() || pet_p4_audio_conversation_active()))
    radio_cancel_locked("interrupted", "语音优先，音频已停止");
#endif
  if(session[0]&&pet_p4_audio_music_active()&&now>last_request+5000) {
    view.position_ms=offset_ms+pet_p4_audio_stream_played_bytes()/96;
    pet_p4_audio_stream_flush();copy(view.status,sizeof(view.status),"error");copy(view.message,sizeof(view.message),"PC 连接已中断，请重新播放");
  }
  xSemaphoreGive(lock);
}
void pet_p4_media_view(pet_p4_media_view_t *out) {
  if(!out)return;memset(out,0,sizeof(*out));if(!lock)return;
  xSemaphoreTake(lock,portMAX_DELAY);refresh();
  int current=-1;
  if(view.lyrics_ready)for(uint32_t i=0;i<lyric_total;i++) {if(lyrics[i].at_ms>view.position_ms)break;current=(int)i;}
  if(current!=view.lyric_index) {view.lyric_index=current;view.lyric_changed_ms=esp_timer_get_time()/1000ULL;}
  *out=view;
  for(int row=0;row<5;row++) {
    int index=current+row-2;
    if(view.lyrics_ready && index>=0 && index<(int)lyric_total)copy(out->lyric_lines[row],sizeof(out->lyric_lines[row]),lyrics[index].text);
    else out->lyric_lines[row][0]=0;
  }
  xSemaphoreGive(lock);
}
void pet_p4_media_blit(uint16_t *frame,int stride,int x,int y) {
  if(!lock||!frame||x<0||y<0||x+192>stride||y+192>480)return;
  xSemaphoreTake(lock,portMAX_DELAY);
  if(view.cover_ready&&cover)for(int row=0;row<192;row++)for(int col=0;col<192;col++) {
    int i=(row*192+col)*2;frame[(y+row)*stride+x+col]=(uint16_t)cover[i]|((uint16_t)cover[i+1]<<8);
  }
  xSemaphoreGive(lock);
}
void pet_p4_media_stop_background(pet_p4_send_line_fn send, void *ctx) {
  pet_p4_media_init();if(!lock)return;
  xSemaphoreTake(lock,portMAX_DELAY);
#if PET_P4_SYSTEM_WIFI
  radio_auto_entry = radio_fallback_pending = radio_autoplay_pending = false;
  radio_fallback_key[0] = 0;
#endif
#if PET_P4_SYSTEM_WIFI
  if(radio_generation)radio_cancel_locked("idle","已停止播放");
#endif
  // Only flush the music owner: voice and notification audio are unrelated.
  if(pet_p4_audio_music_active())pet_p4_audio_stream_flush();
  session[0]=0;offset_ms=0;view.position_ms=0;view.seeking=false;
  copy(view.status,sizeof(view.status),"idle");
  copy(view.message,sizeof(view.message),"已停止播放");
  send_line=send;send_ctx=ctx;
  // Always notify PC, including when a song is still being prepared there.
  cJSON *payload=cJSON_CreateObject();
  cJSON_AddStringToObject(payload,"operation","stop");
  cJSON_AddBoolToObject(payload,"global",true);
  cJSON_AddStringToObject(payload,"source",view.source[0]?view.source:"audio.player");
  emit("media/event",payload);
  xSemaphoreGive(lock);
}
bool pet_p4_media_action(const char *action) {
  if(!lock || !action || strncmp(action,"media.",6))return false;
  xSemaphoreTake(lock,portMAX_DELAY);const char *op=action+6;const char *remote=NULL;
  if(!strcmp(op,"lyrics")) {view.page=view.page==2?0:2;view.seeking=false;}
  else if(!strcmp(op,"queue")) {view.page=view.page==1?0:1;view.seeking=false;}
  else if(!strcmp(op,"select")) {if(view.page==1)remote="select";else if(view.page==0)view.seeking=!view.seeking;}
  else if(!strcmp(op,"up")||!strcmp(op,"down")) {
    if(view.page==1 && view.count) view.selected=(view.selected+view.count+(!strcmp(op,"up")?-1:1))%view.count;
    else remote=!strcmp(op,"up")?"volume_up":"volume_down";
  } else if(!strcmp(op,"previous"))remote=view.seeking?"seek_back":"previous";
  else if(!strcmp(op,"next"))remote=view.seeking?"seek_forward":"next";
  else if(!strcmp(op,"toggle"))remote="toggle";
  else {xSemaphoreGive(lock);return false;}
  if(remote) {cJSON *payload=cJSON_CreateObject();cJSON_AddStringToObject(payload,"operation",remote);if(view.selected<view.count)cJSON_AddStringToObject(payload,"key",queue_keys[view.selected]);emit("media/event",payload);}
  xSemaphoreGive(lock);return true;
}
bool pet_p4_media_live_action(const char *source,const char *action) {
  pet_p4_media_init();if(!lock||!action||strncmp(action,"media.",6))return false;
  xSemaphoreTake(lock,portMAX_DELAY);
#if PET_P4_SYSTEM_WIFI
  if (!strcmp(source, PET_P4_MEDIA_WIFI_SOURCE) && !radio_owns_controls(source)) {
    xSemaphoreGive(lock);
    return false;
  }
#endif
  if(strcmp(view.source,source)){
#if PET_P4_SYSTEM_WIFI
    if (live_provider == &wifi_audio_provider && radio_direct) {
      radio_cancel_locked("idle", "已停止播放");
      radio_direct = false;
    }
#endif
    memset(&view,0,sizeof(view));copy(view.source,sizeof(view.source),source);
  }
  view.seeking=false;
  const char *op=action+6,*remote=NULL;
  if(!strcmp(op,"queue")){view.page=view.page==1?0:1;if(view.page==1)remote="sync";}
  else if(!strcmp(op,"select")){if(view.page==1)remote="select";else {view.page=1;remote="sync";}}
  else if(!strcmp(op,"up")||!strcmp(op,"down")){
    if(view.page==1&&view.count)view.selected=(view.selected+view.count+(!strcmp(op,"up")?-1:1))%view.count;
    else remote=!strcmp(op,"up")?"volume_up":"volume_down";
  }else if(!strcmp(op,"toggle"))remote=view.page==1?"select":"toggle";
  else if(!strcmp(op,"previous")||!strcmp(op,"next"))remote=op;
  else{xSemaphoreGive(lock);return false;}
#if PET_P4_SYSTEM_WIFI
  if(remote&&radio_owns_controls(source)){
    if(radio_action_locked(source,remote,-1)){xSemaphoreGive(lock);return true;}
    /* The selected row is the exact fallback target even for next/previous. */
    remote="select";
  }
#endif
  if(remote){cJSON *p=cJSON_CreateObject();cJSON_AddStringToObject(p,"source",source);cJSON_AddStringToObject(p,"operation",remote);
    if(view.selected<view.count)cJSON_AddStringToObject(p,"key",queue_keys[view.selected]);emit("media/event",p);}
  xSemaphoreGive(lock);return true;
}
bool pet_p4_media_touch(const char *source,int index,int volume,uint32_t revision,uint64_t now_ms){
  static uint64_t last_select;
  if (!lock || !source) return false;
  if (strncmp(source, "pc.", 3)
#if PET_P4_SYSTEM_WIFI
      && strcmp(source, PET_P4_MEDIA_WIFI_SOURCE)
#endif
  ) return false;
  xSemaphoreTake(lock,portMAX_DELAY);
#if PET_P4_SYSTEM_WIFI
  if (!strcmp(source, PET_P4_MEDIA_WIFI_SOURCE) && !radio_owns_controls(source)) {
    xSemaphoreGive(lock);
    return false;
  }
#endif
  bool select=index>=0;
  if(strcmp(source,view.source)||revision!=view.queue_revision||
     (select&&(view.page!=1||index>=view.count||(last_select&&now_ms-last_select<300)))||
     (!select&&(volume<0||volume>100||view.page!=0))){
    xSemaphoreGive(lock);return false;
  }
#if PET_P4_SYSTEM_WIFI
  if(radio_owns_controls(source)){
    if(select){view.selected=index;view.page=0;last_select=now_ms;}
    if(radio_action_locked(source,select?"select":"volume",volume)){xSemaphoreGive(lock);return true;}
  }
#endif
  cJSON *p=cJSON_CreateObject();
  if(!p){xSemaphoreGive(lock);return false;}
  cJSON_AddStringToObject(p,"source",source);
  if(select){
    view.selected=index;view.page=0;last_select=now_ms;
    cJSON_AddStringToObject(p,"operation","select");cJSON_AddStringToObject(p,"key",queue_keys[index]);
  }else{
    cJSON_AddStringToObject(p,"operation","volume");cJSON_AddNumberToObject(p,"volume",volume);
    /* Show requested position; authoritative host volume will confirm it. */
    view.volume=volume;
  }
  emit("media/event",p);xSemaphoreGive(lock);return true;
}
