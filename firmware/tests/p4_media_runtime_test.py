"""Execute the real media protocol and navigation with simulated hardware, not real audio."""
from pathlib import Path
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]

@pytest.mark.parametrize("wifi", [0, 1])
def test_media_sessions_queue_controls_and_watchdog(tmp_path, wifi):
    cjson = ROOT / "tests/vendor/cjson"
    stubs = {
        "esp_err.h": "typedef int esp_err_t;\n#define ESP_OK 0\n#define ESP_ERR_NO_MEM 257\n#define ESP_ERR_INVALID_STATE 259\n",
        "esp_heap_caps.h": "#include <stdlib.h>\n#define MALLOC_CAP_SPIRAM 0\n#define MALLOC_CAP_8BIT 0\n#define heap_caps_malloc(n,c) malloc(n)\n",
        "esp_timer.h": "#include <stdint.h>\nint64_t esp_timer_get_time(void);\n",
        "esp_random.h": "#include <stdint.h>\nstatic inline uint32_t esp_random(void){return 42;}\n",
        "freertos/FreeRTOS.h": "#define portMAX_DELAY 0\n",
        "freertos/semphr.h": "typedef int SemaphoreHandle_t;\nvoid test_take(int);\nvoid test_give(int);\n#define xSemaphoreCreateMutex() 1\n#define xSemaphoreTake(a,b) test_take(a)\n#define xSemaphoreGive(a) test_give(a)\n",
        "mbedtls/base64.h": "#include <stddef.h>\nint mbedtls_base64_decode(unsigned char*,size_t,size_t*,const unsigned char*,size_t);\n",
    }
    for name, source in stubs.items():
        file = tmp_path / name; file.parent.mkdir(parents=True, exist_ok=True); file.write_text(source)
    # Use the actual worker's unlock/TX/free tail, not a substitute sender.
    main_source = (ROOT / "main/pet_p4_main.c").read_text()
    worker = main_source.split("static void protocol_worker_task(void *arg) {", 1)[1]
    tail = worker.split("      xSemaphoreGive(g_state_mutex);", 1)[1].split(
        "#if PET_P4_QUERY_REPLY_TRACE\n#if PET_P4_QUERY_OBSERVATION\n      uint64_t query_observed_end", 1)[0]
    (tmp_path / "worker_tx.inc").write_text("xSemaphoreGive(g_state_mutex);" + tail)
    harness = tmp_path / "runtime.c"
    harness.write_text(r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdatomic.h>
#include "cJSON.h"
#include "pet_p4_media.h"
#include "pet_p4_audio.h"
static int media_held, state_held;
static const int g_state_mutex=2;
void test_take(int m){int *held=m==1?&media_held:&state_held;assert(!*held);*held=1;}
void test_give(int m){int *held=m==1?&media_held:&state_held;assert(*held);*held=0;}
#include "freertos/semphr.h"
static unsigned sends, wrong_sends;
static bool expect_unlocked;
static void *expected_ctx;
static atomic_bool active;
static bool paused, ended, busy;
static unsigned int flushes;
static uint32_t played;
static unsigned buffered_ms=200;
static int64_t clock_us=1000000;
static cJSON *reply;
#if PET_P4_SYSTEM_WIFI
#include "pet_p4_radio.h"
static const pet_radio_station_t local[]={{"cr-a","Local A","10"},{"cr-b","Local B","20"}};
static bool online=true, voice;
static uint32_t submitted;
static bool submitted_live;
static pet_wifi_endpoint_t submitted_endpoint;
static char current_owner[64];
bool pet_device_wifi_online(void){return online;}
const pet_radio_station_t *pet_radio_station_at(unsigned i){return i<2?&local[i]:NULL;}
const pet_radio_station_t *pet_radio_station_find(const char *id){
  for(unsigned i=0;i<2;++i)if(!strcmp(id,local[i].id))return &local[i];
  return NULL;
}
bool pet_radio_submit(const pet_radio_station_t *station,uint32_t generation){
  assert(station==&local[0]||station==&local[1]);submitted=generation;return true;
}
bool pet_network_audio_submit_public(const pet_wifi_endpoint_t *endpoint,bool live,uint32_t generation){
  submitted_endpoint=*endpoint;submitted_live=live;submitted=generation;return true;
}
bool pet_p4_audio_active(void){return voice;}
bool pet_p4_audio_conversation_active(void){return false;}
bool pet_p4_audio_stream_matches(const char *id){return active&&!voice&&!strcmp(id,current_owner);}
static int g_stream_lock=1, stream_lock_depth;
#define g_music active
#define g_stream_active active
#define g_stream_session current_owner
#define portMAX_DELAY 0
#define xSemaphoreTakeRecursive(m,t) do { assert((m) && !(t) && !stream_lock_depth); ++stream_lock_depth; } while(0)
#define xSemaphoreGiveRecursive(m) do { assert((m) && stream_lock_depth==1); --stream_lock_depth; } while(0)
#include "pet_p4_radio_audio.inc"
#endif
bool pet_p4_miniapp_host_source(const char *source){return source&&!strcmp(source,"app.test");}
int64_t esp_timer_get_time(void){return clock_us;}
bool pet_p4_audio_music_active(void){return active;}
bool pet_p4_audio_music_paused(void){return paused;}
bool pet_p4_audio_music_ended(void){return ended;}
bool pet_p4_audio_stream_playing(void){return active&&!paused&&!ended;}
uint32_t pet_p4_audio_stream_played_bytes(void){return played;}
unsigned int pet_p4_audio_music_buffered_ms(void){return buffered_ms;}
esp_err_t pet_p4_audio_music_begin(const char *id){(void)id;if(busy)return -1;
#if PET_P4_SYSTEM_WIFI
if(voice)return -1;snprintf(current_owner,sizeof(current_owner),"%s",id);
#endif
active=true;paused=false;ended=false;played=0;return 0;}
esp_err_t pet_p4_audio_stream_push(const uint8_t *p,size_t n){(void)p;played+=(uint32_t)n;return 0;}
esp_err_t pet_p4_audio_stream_end(void){ended=true;return 0;}
void pet_p4_audio_stream_flush(void){active=false;flushes++;}
void pet_p4_audio_music_pause(bool value){paused=value;}
void pet_p4_audio_music_volume(unsigned int value){(void)value;}
int mbedtls_base64_decode(unsigned char *out,size_t cap,size_t *n,const unsigned char *in,size_t len){
  if(cap<6||len!=8||memcmp(in,"AAAoAAEA",8))return -1;
  const unsigned char data[]={0,0,40,0,1,0};memcpy(out,data,6);*n=6;return 0;
}
static void sent(const char *line,void *ctx){
  assert(ctx==expected_ctx);if(expect_unlocked)assert(!state_held&&!media_held);
  ++sends;cJSON_Delete(reply);reply=cJSON_Parse(line);assert(reply);
}
static void other_route(const char *line,void *ctx){(void)line;assert(ctx==(void*)456);++wrong_sends;}
static void worker_tx(pet_p4_query_reply_t query_reply,void *ctx){
  struct {pet_p4_send_line_fn send_line;void *reply_ctx;} request={sent,ctx};
#include "worker_tx.inc"
}
static cJSON *payload(void){return cJSON_GetObjectItemCaseSensitive(reply,"payload");}
static bool call(const char *topic,const char *json){
  cJSON *p=cJSON_Parse(json);assert(p);unsigned before=sends;
  bool chunk=!strcmp(topic,"media/chunk");
  expected_ctx=chunk?(void*)123:NULL;
  test_take(g_state_mutex);
  pet_p4_query_reply_t detached={0};
  assert(pet_p4_media_handle_with_reply(topic,p,sent,expected_ctx,TEST_DEFERRED?&detached:NULL));
  cJSON_Delete(p); // No request pointers may survive into TX.
  if(TEST_DEFERRED&&chunk){
    assert(sends==before&&detached.line&&!media_held&&state_held&&!detached.diagnostic_query);
    // A competing transport update must not redirect the held ACK.
    pet_p4_media_transport(other_route,(void*)456);
    uint32_t snapshot_played=played;unsigned snapshot_buffered=buffered_ms;
    played+=96000;buffered_ms=987; // TX must not resample mutable audio state.
    expect_unlocked=true;worker_tx(detached,expected_ctx);expect_unlocked=false;
    played=snapshot_played;buffered_ms=snapshot_buffered;
    assert(!wrong_sends);
    // Persistent event transport remains the real callback, never a stack capture.
    assert(pet_p4_media_action("media.next"));assert(wrong_sends==1);wrong_sends=0;
  }else{assert(!detached.line);test_give(g_state_mutex);}
  assert(sends==before+1&&!state_held&&!media_held);
  if(chunk){char *wire=cJSON_PrintUnformatted(reply);puts(wire);cJSON_free(wire);}
  expected_ctx=NULL;pet_p4_media_transport(sent,NULL);
  return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(payload(),"ok"));
}
static const char *field(const char *name){return cJSON_GetObjectItemCaseSensitive(payload(),name)->valuestring;}
int main(void){
  pet_p4_media_init();
  pet_p4_media_transport(sent,NULL);
  pet_p4_media_visibility("pc.test",true,1000);assert(!strcmp(field("operation"),"visibility"));
  assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(payload(),"visible")));
  int entry_epoch=cJSON_GetObjectItemCaseSensitive(payload(),"epoch")->valueint;
  pet_p4_media_visibility("pc.test",true,4000);assert(cJSON_GetObjectItemCaseSensitive(payload(),"epoch")->valueint==entry_epoch);
  pet_p4_media_visibility("",false,4100);assert(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(payload(),"visible")));
  assert(cJSON_GetObjectItemCaseSensitive(payload(),"epoch")->valueint>entry_epoch);
  {cJSON *big=cJSON_CreateObject(),*q=cJSON_AddArrayToObject(big,"queue"),*keys=cJSON_AddArrayToObject(big,"keys");
  for(int i=0;i<128;i++){char text[32];sprintf(text,"station%d",i);cJSON_AddItemToArray(q,cJSON_CreateString(text));cJSON_AddItemToArray(keys,cJSON_CreateString(text));}
  char *big_text=cJSON_PrintUnformatted(big);assert(call("media/library",big_text));cJSON_free(big_text);
  pet_p4_media_view_t big_view;pet_p4_media_view(&big_view);assert(big_view.count==128);
  cJSON_AddItemToArray(q,cJSON_CreateString("overflow"));cJSON_AddItemToArray(keys,cJSON_CreateString("overflow"));
  big_text=cJSON_PrintUnformatted(big);assert(!call("media/library",big_text));cJSON_free(big_text);cJSON_Delete(big);}
  assert(call("media/library","{\"queue\":[\"重复标题\",\"重复标题\"],\"keys\":[\"local:1\",\"local:2\"],\"volume\":25}"));
  assert(pet_p4_media_action("media.queue"));assert(pet_p4_media_action("media.down"));assert(pet_p4_media_action("media.select"));assert(!strcmp(field("key"),"local:2"));
  pet_p4_media_view_t view;pet_p4_media_view(&view);assert(view.selected==1&&view.page==1);
  assert(!call("media/library","{\"queue\":[\"bad\",4],\"keys\":[\"local:3\",\"local:4\"]}"));
  pet_p4_media_view(&view);assert(view.count==2&&view.selected==1);
  const char *begin="{\"sessionId\":\"music-new\",\"sampleRate\":48000,\"format\":\"ima-block-v1\",\"durationMs\":20000,\"offsetMs\":0,\"volume\":25,\"queue\":[\"歌曲\"],\"keys\":[\"local:1\"]}";
  busy=true;assert(!call("media/begin",begin));assert(!active);busy=false;assert(call("media/begin",begin));
  assert(!call("media/lyrics","{\"sessionId\":\"music-old\",\"start\":0,\"total\":0,\"lines\":[]}"));
  assert(!call("media/lyrics","{\"sessionId\":\"music-new\",\"start\":0,\"total\":2,\"lines\":[{\"atMs\":5000,\"text\":\"bad\"},{\"atMs\":2000,\"text\":\"bad\"}]}"));
  assert(call("media/lyrics","{\"sessionId\":\"music-new\",\"start\":0,\"total\":2,\"lines\":[{\"atMs\":1000,\"text\":\"测试第一句\"}]}"));
  pet_p4_media_view(&view);assert(!view.lyrics_ready);
  assert(!call("media/lyrics","{\"sessionId\":\"music-new\",\"start\":0,\"total\":2,\"lines\":[{\"atMs\":2000,\"text\":\"重复块\"}]}"));
  assert(call("media/lyrics","{\"sessionId\":\"music-new\",\"start\":1,\"total\":2,\"lines\":[{\"atMs\":2000,\"text\":\"测试第二句\"}]}"));
  assert(pet_p4_media_action("media.lyrics"));played=1500*96;pet_p4_media_view(&view);
  assert(view.page==2&&view.lyric_index==0&&!strcmp(view.lyric_lines[2],"测试第一句"));
  played=2500*96;pet_p4_media_view(&view);assert(view.lyric_index==1&&!strcmp(view.lyric_lines[2],"测试第二句"));
  paused=true;pet_p4_media_view(&view);assert(view.lyric_index==1);paused=false;
  played=500*96;pet_p4_media_view(&view);assert(view.lyric_index==-1);
  assert(pet_p4_media_action("media.lyrics"));pet_p4_media_view(&view);assert(view.page==0);
  assert(pet_p4_media_action("media.lyrics"));assert(call("media/begin",begin));pet_p4_media_view(&view);
  assert(view.page==2&&!view.lyrics_ready&&view.lyric_index==-1&&!view.lyric_lines[2][0]);
  assert(call("media/lyrics","{\"sessionId\":\"music-new\",\"start\":0,\"total\":0,\"lines\":[]}"));
  pet_p4_media_view(&view);assert(view.lyrics_ready&&!view.lyric_lines[2][0]);
  assert(!call("media/chunk","{\"sessionId\":\"music-old\",\"seq\":0,\"data\":\"AAAoAAEA\"}"));
  assert(call("media/chunk","{\"sessionId\":\"music-new\",\"seq\":0,\"data\":\"AAAoAAEA\"}"));
  assert(!call("media/chunk","{\"sessionId\":\"music-new\",\"seq\":0,\"data\":\"AAAoAAEA\"}"));assert(played==2);
  // An occupied reply slot cannot consume a chunk or replace its bytes.
  cJSON *occupied_request=cJSON_Parse("{\"sessionId\":\"music-new\",\"seq\":1,\"data\":\"AAAoAAEA\"}");
  pet_p4_query_reply_t occupied={.line=(char*)"existing"};unsigned prior_sends=sends;
  assert(!pet_p4_media_handle_with_reply("media/chunk",occupied_request,sent,NULL,&occupied));
  assert(played==2&&sends==prior_sends&&!strcmp(occupied.line,"existing"));cJSON_Delete(occupied_request);
  // Long request identity is kept in full, without a fixed ACK buffer.
  char long_chunk[8192],identity[4096];memset(identity,'x',sizeof(identity)-1);identity[sizeof(identity)-1]=0;
  snprintf(long_chunk,sizeof(long_chunk),"{\"requestId\":\"%s\",\"sessionId\":\"music-new\",\"seq\":1,\"data\":\"AAAoAAEA\"}",identity);
  assert(call("media/chunk",long_chunk));assert(!strcmp(field("requestId"),identity)&&played==4);
  assert(call("media/stop","{\"sessionId\":\"music-old\"}"));assert(active);
  assert(call("media/control","{\"operation\":\"pause\"}"));assert(paused&&!strcmp(field("state"),"paused"));
  assert(call("media/control","{\"operation\":\"resume\"}"));assert(!paused);
  assert(!call("media/end","{\"sessionId\":\"music-old\"}"));assert(!ended);
  assert(call("media/end","{\"sessionId\":\"music-new\"}"));assert(ended&&!strcmp(field("state"),"ended"));
  // Back on the pet home page stops locally, preserves the queue and notifies PC.
  assert(call("media/begin",begin));
  assert(pet_p4_media_action("media.lyrics"));assert(active);
  pet_p4_media_stop_background(sent,NULL);assert(!active);
  assert(!strcmp(field("operation"),"stop"));
  pet_p4_media_view(&view);assert(!strcmp(view.status,"idle")&&view.position_ms==0&&view.count==1);
  pet_p4_media_stop_background(sent,NULL);assert(!active);
  assert(!call("media/chunk","{\"sessionId\":\"music-new\",\"seq\":0,\"data\":\"AAAoAAEA\"}"));
  assert(!call("media/query","{\"sessionId\":\"music-new\"}"));
  // Pending preparation also emits a cancellation, without touching voice audio.
  unsigned int music_flushes=flushes;
  pet_p4_media_stop_background(sent,NULL);
  assert(flushes==music_flushes);
  assert(!strcmp(field("operation"),"stop"));
  assert(call("media/begin",begin));
  assert(call("media/control","{\"operation\":\"pause\"}"));
  pet_p4_media_stop_background(sent,NULL);assert(!active);
  assert(call("media/begin",begin));clock_us=9000000;
  assert(call("media/library","{\"queue\":[],\"keys\":[]}"));pet_p4_media_tick(9000);assert(!active);
  pet_p4_media_view(&view);assert(!strcmp(view.status,"error"));
  assert(!pet_p4_media_action("media.shell"));
  assert(pet_p4_media_live_action("pc.first","media.queue"));assert(!strcmp(field("source"),"pc.first"));assert(!strcmp(field("operation"),"sync"));
  assert(call("media/library","{\"source\":\"pc.first\",\"queue\":[\"One\",\"Two\"],\"keys\":[\"1\",\"2\"],\"volume\":20}"));
  assert(pet_p4_media_live_action("pc.first","media.down"));assert(pet_p4_media_live_action("pc.first","media.select"));assert(!strcmp(field("key"),"2"));
  assert(pet_p4_media_live_action("pc.first","media.toggle"));assert(!strcmp(field("operation"),"select"));assert(!strcmp(field("key"),"2"));
  assert(pet_p4_media_live_action("pc.first","media.down"));pet_p4_media_view(&view);assert(view.selected==0);
  assert(pet_p4_media_live_action("pc.first","media.up"));pet_p4_media_view(&view);assert(view.selected==1);
  assert(pet_p4_media_live_action("pc.first","media.queue"));pet_p4_media_view(&view);assert(view.page==0);
  assert(pet_p4_media_live_action("pc.first","media.toggle"));assert(!strcmp(field("operation"),"toggle"));
  assert(pet_p4_media_live_action("pc.first","media.next"));assert(!strcmp(field("operation"),"next"));
  assert(pet_p4_media_live_action("pc.first","media.previous"));assert(!strcmp(field("operation"),"previous"));
  assert(!pet_p4_media_live_action("pc.first","media.lyrics"));
  // Touch must use the exact queue generation and identity. Stale/upstream
  // replacement lists and out-of-range slider values cannot trigger commands.
  pet_p4_media_view(&view);uint32_t revision=view.queue_revision;
  assert(!pet_p4_media_touch("pc.foreign",-1,50,revision,10000));
  assert(!pet_p4_media_touch("pc.first",-1,101,revision,10000));
  assert(pet_p4_media_touch("pc.first",-1,55,revision,10000));
  assert(!strcmp(field("operation"),"volume"));
  assert(cJSON_GetObjectItemCaseSensitive(payload(),"volume")->valueint==55);
  assert(pet_p4_media_live_action("pc.first","media.queue"));
  assert(!pet_p4_media_touch("pc.first",2,-1,revision,11000));
  assert(pet_p4_media_touch("pc.first",0,-1,revision,11000));
  assert(!strcmp(field("key"),"1"));
  assert(pet_p4_media_live_action("pc.first","media.queue"));
  assert(!pet_p4_media_touch("pc.first",1,-1,revision,11100));
  assert(pet_p4_media_touch("pc.first",1,-1,revision,11400));
  assert(!strcmp(field("key"),"2"));
  assert(call("media/library","{\"source\":\"pc.first\",\"queue\":[\"Changed\"],\"keys\":[\"new\"]}"));
  assert(pet_p4_media_live_action("pc.first","media.queue"));
  assert(!pet_p4_media_touch("pc.first",0,-1,revision,12000));
  // All 128 real queue identities remain reachable, including wrap across pages.
  assert(pet_p4_media_live_action("pc.navigation","media.queue"));
  cJSON *catalog=cJSON_CreateObject(),*titles=cJSON_AddArrayToObject(catalog,"queue"),*keys=cJSON_AddArrayToObject(catalog,"keys");
  cJSON_AddStringToObject(catalog,"source","pc.navigation");
  for(int i=0;i<128;i++){char item[8];snprintf(item,sizeof(item),"%d",i);cJSON_AddItemToArray(titles,cJSON_CreateString(item));cJSON_AddItemToArray(keys,cJSON_CreateString(item));}
  char *catalog_json=cJSON_PrintUnformatted(catalog);assert(call("media/library",catalog_json));cJSON_free(catalog_json);cJSON_Delete(catalog);
  for(int i=0;i<128;i++){pet_p4_media_view(&view);assert(view.selected==i);assert(pet_p4_media_live_action("pc.navigation","media.select"));char key[8];snprintf(key,sizeof(key),"%d",i);assert(!strcmp(field("key"),key));assert(pet_p4_media_live_action("pc.navigation","media.down"));}
  pet_p4_media_view(&view);assert(view.selected==0);assert(pet_p4_media_live_action("pc.navigation","media.up"));pet_p4_media_view(&view);assert(view.selected==127);
  assert(pet_p4_media_live_action("pc.second","media.toggle"));assert(!strcmp(field("source"),"pc.second"));
  pet_p4_media_view(&view);assert(view.count==0); // no prior component's queue leaks
  assert(call("media/notice","{\"source\":\"pc.second\",\"state\":\"error\",\"message\":\"请先在 PC 启用\"}"));
  pet_p4_media_view(&view);assert(!strcmp(view.status,"error")&&!strcmp(view.message,"请先在 PC 启用"));
  assert(!call("media/notice","{\"source\":\"pc.first\",\"state\":\"error\",\"message\":\"stale\"}"));
  assert(!call("media/notice","{\"source\":\"pc.second\",\"state\":\"playing\",\"message\":\"fake\"}"));
  assert(call("media/begin",begin));
  assert(!call("media/library","{\"source\":\"pc.second\",\"queue\":[],\"keys\":[]}"));
  assert(!call("media/notice","{\"source\":\"pc.second\",\"state\":\"error\",\"message\":\"foreign\"}"));
  cJSON *owned=cJSON_Parse(begin);cJSON_AddStringToObject(owned,"source","pc.second");
  char *owned_json=cJSON_PrintUnformatted(owned);assert(call("media/begin",owned_json));cJSON_free(owned_json);cJSON_Delete(owned);
  assert(call("media/library","{\"source\":\"pc.second\",\"queue\":[],\"keys\":[]}"));
  music_flushes=flushes;
  assert(call("media/notice","{\"source\":\"pc.second\",\"state\":\"error\",\"message\":\"not allowed to interrupt\"}"));
  pet_p4_media_view(&view);assert(active&&flushes==music_flushes&&!strcmp(view.status,"playing"));
  clock_us+=6000000;pet_p4_media_tick(clock_us/1000);assert(!active); // notices do not renew audio lease
#if PET_P4_SYSTEM_WIFI
  pet_p4_media_authorize_live("other",PET_RADIO_SOURCE,true,true);
  assert(!submitted);
  pet_p4_media_authorize_live("city-radio","pc.other",true,true);
  pet_p4_media_visibility(PET_RADIO_SOURCE,true,8000);
  assert(!cJSON_GetObjectItemCaseSensitive(payload(),"deviceDirect")&&!submitted);
  pet_p4_media_visibility("",false,8010);
  pet_p4_media_authorize_live("city-radio",PET_RADIO_SOURCE,true,true);
  pet_p4_media_visibility(PET_RADIO_SOURCE,true,8020);
  assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(payload(),"deviceDirect")));
  pet_p4_media_view(&view);
  assert(view.count==2&&!strcmp(view.title,"Local A")&&!strcmp(view.status,"buffering"));
  uint32_t first=submitted;
  int16_t pcm[]={-1000,500,300};
  assert(pet_p4_media_radio_current(first));
  assert(pet_p4_media_radio_write(first,pcm,3)==PET_RADIO_WRITTEN);
  buffered_ms=395;
  assert(pet_p4_media_radio_write(first,pcm,3)==PET_RADIO_WRITTEN); // reach the 400 ms start threshold
  buffered_ms=600;
  assert(pet_p4_media_radio_write(first,pcm,3)==PET_RADIO_BACKPRESSURE);
  buffered_ms=200;
  clock_us+=6000000;pet_p4_media_tick(clock_us/1000);assert(active);
  assert(pet_p4_media_live_action(PET_RADIO_SOURCE,"media.toggle"));
  assert(!active&&!pet_p4_media_radio_current(first));
  pet_p4_media_view(&view);assert(!strcmp(view.status,"paused"));
  assert(pet_p4_media_live_action(PET_RADIO_SOURCE,"media.toggle"));
  uint32_t resumed=submitted;
  assert(resumed!=first&&pet_p4_media_radio_current(resumed));
  assert(pet_p4_media_radio_write(first,pcm,3)==PET_RADIO_CANCELLED);
  assert(pet_p4_media_radio_write(resumed,pcm,3)==PET_RADIO_WRITTEN);
  voice=true;strcpy(current_owner,"voice-current");music_flushes=flushes;
  assert(pet_p4_media_radio_write(resumed,pcm,3)==PET_RADIO_CANCELLED);
  assert(flushes==music_flushes&&voice); // A stale radio producer cannot flush voice.
  active=false;voice=false;
  pet_p4_media_view(&view);assert(!strcmp(view.status,"interrupted"));
  assert(call("media/library","{\"source\":\"pc.city-radio\",\"queue\":[\"HLS\"],\"keys\":[\"cr-hls\"]}"));
  assert(pet_p4_media_live_action(PET_RADIO_SOURCE,"media.queue"));
  pet_p4_media_view(&view);assert(view.count==3);
  assert(pet_p4_media_touch(PET_RADIO_SOURCE,0,-1,view.queue_revision,10000));
  assert(!strcmp(field("operation"),"select")&&!strcmp(field("key"),"cr-hls"));
  // An unsupported station requested from the PC cannot strand local controls.
  pet_p4_media_authorize_live("city-radio",PET_RADIO_SOURCE,true,false);
  uint32_t before_next=submitted;
  assert(pet_p4_media_live_action(PET_RADIO_SOURCE,"media.next"));
  assert(submitted!=before_next&&pet_p4_media_radio_current(submitted));
  pet_p4_media_authorize_live(NULL,NULL,false,false);
  pet_p4_media_visibility("",false,10100);
  pet_p4_media_authorize_live("city-radio",PET_RADIO_SOURCE,true,true);
  uint32_t current=submitted;
  assert(pet_p4_media_radio_current(current));
  pet_p4_media_radio_finished(resumed,PET_RADIO_SOURCE_FAILED);
  assert(pet_p4_media_radio_current(current)); // A stale failure cannot stop a new stream.
  pet_p4_media_radio_finished(current,PET_RADIO_FORMAT_UNSUPPORTED);
  assert(pet_p4_media_live_action(PET_RADIO_SOURCE,"media.toggle"));
  assert(!strcmp(field("operation"),"select")&&!strcmp(field("key"),"cr-a"));
  pet_p4_media_authorize_live(NULL,NULL,false,false);
  online=false;
  pet_p4_media_authorize_live("city-radio",PET_RADIO_SOURCE,true,true);
  pet_p4_media_visibility(PET_RADIO_SOURCE,true,10500);
  assert(!cJSON_GetObjectItemCaseSensitive(payload(),"deviceDirect")&&submitted==current);
  online=true;
  pet_p4_media_authorize_live("city-radio",PET_RADIO_SOURCE,true,false);
  assert(submitted!=current && pet_p4_media_radio_current(submitted));
  assert(pet_p4_media_radio_write(submitted,pcm,3)==PET_RADIO_WRITTEN);
  played=96000;
  pet_p4_media_radio_finished(submitted,PET_RADIO_COMPLETE);
  assert(ended && active);
  active=false; // Codec has drained the final samples.
  pet_p4_media_view(&view);
  assert(!strcmp(view.status,"ended") && view.position_ms==1000 && view.duration_ms==1000);
  assert(!pet_p4_media_radio_current(submitted));

  // Offline HLS selection has no local decoder. Reconnecting the PC must
  // let an explicit play reach that exact station instead of swallowing it.
  pet_p4_media_authorize_live(NULL,NULL,false,false);
  assert(call("media/library","{\"source\":\"pc.city-radio\",\"queue\":[\"HLS\"],\"keys\":[\"cr-hls\"]}"));
  pet_p4_media_authorize_live("city-radio",PET_RADIO_SOURCE,true,false);
  assert(pet_p4_media_live_action(PET_RADIO_SOURCE,"media.queue"));
  pet_p4_media_view(&view);
  assert(pet_p4_media_touch(PET_RADIO_SOURCE,0,-1,view.queue_revision,20000));
  unsigned before_hls=sends;
  uint32_t before_hls_submit=submitted;
  assert(pet_p4_media_live_action(PET_RADIO_SOURCE,"media.toggle"));
  assert(sends==before_hls && submitted==before_hls_submit); // no PC, no decoder
  pet_p4_media_authorize_live("city-radio",PET_RADIO_SOURCE,true,true);
  assert(sends==before_hls && submitted==before_hls_submit); // reconnect is not consent
  assert(pet_p4_media_live_action(PET_RADIO_SOURCE,"media.toggle"));
  assert(sends==before_hls+1);
  assert(!strcmp(field("operation"),"select")&&!strcmp(field("key"),"cr-hls"));
  assert(submitted==before_hls_submit); // never pass HLS into the local MP3 decoder
  pet_p4_media_authorize_live(NULL,NULL,false,false);

  // Fresh entry may hand off only a known pre-PCM format failure. Exercise
  // actual C lifecycle and notices; no network/codec/device is involved.
  for (int scenario=0; scenario<6; ++scenario) {
    pet_p4_media_authorize_live(NULL,NULL,false,false);
    pet_p4_media_visibility("",false,30000+scenario*10000);
    active=false;voice=false;online=true;
    assert(call("media/library","{\"source\":\"pc.city-radio\",\"queue\":[\"Local A\"],\"keys\":[\"cr-a\"]}"));
    pet_p4_media_authorize_live("city-radio",PET_RADIO_SOURCE,true,scenario!=5);
    uint32_t attempt=submitted;
    pet_p4_media_visibility(PET_RADIO_SOURCE,true,30010+scenario*10000);
    assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(payload(),"deviceDirect")));
    int entry_epoch=cJSON_GetObjectItemCaseSensitive(payload(),"epoch")->valueint;
    if(scenario==3)assert(pet_p4_media_radio_write(attempt,pcm,3)==PET_RADIO_WRITTEN);
    if(scenario==4)voice=true;
    pet_p4_media_radio_finished(attempt,scenario==1?PET_RADIO_SOURCE_FAILED:PET_RADIO_FORMAT_UNSUPPORTED);
    pet_p4_media_view(&view);assert(!strcmp(view.status,"error"));
    assert(call("media/notice","{\"source\":\"pc.city-radio\",\"state\":\"idle\",\"message\":\"catalog ready\"}"));
    pet_p4_media_view(&view);assert(!strcmp(view.status,"error")&&strcmp(view.message,"catalog ready"));
    if(scenario==2)pet_p4_media_stop_background(sent,NULL);
    pet_p4_media_visibility(PET_RADIO_SOURCE,true,33010+scenario*10000);
    if(scenario==0){
      assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(payload(),"deviceDirect")));
      assert(!strcmp(field("fallback"),"format_unsupported")&&!strcmp(field("key"),"cr-a"));
      assert(cJSON_GetObjectItemCaseSensitive(payload(),"epoch")->valueint>entry_epoch);
      int fallback_epoch=cJSON_GetObjectItemCaseSensitive(payload(),"epoch")->valueint;
      pet_p4_media_visibility(PET_RADIO_SOURCE,true,36010);
      assert(cJSON_GetObjectItemCaseSensitive(payload(),"epoch")->valueint==fallback_epoch);
      // Disconnect retires the handoff; reconnect is not a new entry.
      pet_p4_media_authorize_live("city-radio",PET_RADIO_SOURCE,true,false);
      pet_p4_media_authorize_live("city-radio",PET_RADIO_SOURCE,true,true);
      pet_p4_media_visibility(PET_RADIO_SOURCE,true,39010);
      assert(!cJSON_GetObjectItemCaseSensitive(payload(),"fallback"));
      assert(cJSON_GetObjectItemCaseSensitive(payload(),"epoch")->valueint==fallback_epoch);
    }else{
      assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(payload(),"deviceDirect")));
      assert(!cJSON_GetObjectItemCaseSensitive(payload(),"fallback"));
      assert(cJSON_GetObjectItemCaseSensitive(payload(),"epoch")->valueint==entry_epoch);
    }
    assert(submitted==attempt); // No background restart of the local decoder.
  }
  voice=false;active=false;
  pet_p4_media_authorize_live(NULL,NULL,false,false);
  pet_p4_media_visibility("",false,100000);
  assert(call("media/library","{\"source\":\"pc.city-radio\",\"queue\":[\"HLS\"],\"keys\":[\"cr-hls\"]}"));
  uint32_t no_local=submitted;
  pet_p4_media_authorize_live("city-radio",PET_RADIO_SOURCE,true,true);
  pet_p4_media_visibility(PET_RADIO_SOURCE,true,100010);
  assert(submitted==no_local && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(payload(),"deviceDirect")));
  assert(!strcmp(field("fallback"),"format_unsupported")&&!strcmp(field("key"),"cr-hls"));
  cJSON *handoff_begin=cJSON_Parse(begin);
  cJSON_AddStringToObject(handoff_begin,"source",PET_RADIO_SOURCE);
  char *handoff_json=cJSON_PrintUnformatted(handoff_begin);
  busy=true;assert(!call("media/begin",handoff_json));busy=false;
  pet_p4_media_visibility(PET_RADIO_SOURCE,true,103010);
  assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(payload(),"deviceDirect")));
  assert(call("media/begin",handoff_json));cJSON_free(handoff_json);cJSON_Delete(handoff_begin);
  pet_p4_media_visibility(PET_RADIO_SOURCE,true,106010);
  assert(!cJSON_GetObjectItemCaseSensitive(payload(),"deviceDirect"));
  assert(!cJSON_GetObjectItemCaseSensitive(payload(),"fallback"));
  pet_p4_media_stop_background(sent,NULL);
  pet_p4_media_visibility(PET_RADIO_SOURCE,true,109010);
  assert(!cJSON_GetObjectItemCaseSensitive(payload(),"fallback"));
  pet_p4_media_authorize_live("city-radio",PET_RADIO_SOURCE,true,true);
  assert(submitted==no_local && !active);
  pet_p4_media_authorize_live(NULL,NULL,false,false);

  cJSON *playlist=cJSON_Parse("[{\"title\":\"Station\",\"url\":\"https://AUDIO.example/radio\",\"live\":true},"
                              "{\"title\":\"Track\",\"url\":\"https://audio.example/track.wav\"}]");
  assert(pet_p4_media_wifi_playlist_valid(playlist));
  cJSON *bad=cJSON_Parse("[{\"title\":\"Bad\",\"url\":\"https://user@audio.example/radio\"}]");
  assert(!pet_p4_media_wifi_playlist_valid(bad));cJSON_Delete(bad);
  bad=cJSON_Parse("[{\"title\":\"One\",\"title\":\"Two\",\"url\":\"https://audio.example/radio\"}]");
  assert(!pet_p4_media_wifi_playlist_valid(bad));cJSON_Delete(bad);
  online=false;
  uint32_t prior=submitted;
  pet_p4_media_authorize_live_playlist_revision("music-a",PET_P4_MEDIA_WIFI_SOURCE,true,true,8,playlist);
  pet_p4_media_view(&view);revision=view.queue_revision;
  assert(submitted==prior && view.count==2 && !strcmp(view.title,"Station"));
  assert(pet_p4_media_live_action(PET_P4_MEDIA_WIFI_SOURCE,"media.toggle"));
  assert(submitted==prior);
  online=true;
  pet_p4_media_authorize_live_playlist_revision("music-a",PET_P4_MEDIA_WIFI_SOURCE,true,true,8,NULL);
  assert(pet_p4_media_live_action(PET_P4_MEDIA_WIFI_SOURCE,"media.toggle"));
  assert(submitted!=prior && submitted_live && !strcmp(submitted_endpoint.host,"audio.example"));
  assert(!strcmp(submitted_endpoint.path,"/radio"));
  assert(pet_p4_media_radio_write(submitted,pcm,3)==PET_RADIO_WRITTEN);
  online=false;
  pet_p4_media_tick(clock_us/1000);
  assert(pet_p4_media_radio_reconnecting(submitted) && !active);
  pet_p4_media_view(&view);
  assert(!strcmp(view.status,"buffering") && view.queue_revision==revision);
  assert(pet_p4_media_live_action(PET_P4_MEDIA_WIFI_SOURCE,"media.toggle"));
  assert(!pet_p4_media_radio_current(submitted));
  online=true;
  assert(pet_p4_media_live_action(PET_P4_MEDIA_WIFI_SOURCE,"media.queue"));
  assert(pet_p4_media_touch(PET_P4_MEDIA_WIFI_SOURCE,1,-1,revision,13000));
  assert(!submitted_live && !strcmp(submitted_endpoint.path,"/track.wav"));
  prior=submitted;
  pet_p4_media_authorize_live_playlist_revision("music-b",PET_P4_MEDIA_WIFI_SOURCE,true,true,8,playlist);
  assert(!pet_p4_media_radio_current(prior));
  pet_p4_media_view(&view);
  assert(view.selected==0 && view.queue_revision!=revision);
  assert(!pet_p4_media_touch(PET_P4_MEDIA_WIFI_SOURCE,-1,30,revision,13000));
  assert(pet_p4_media_live_action(PET_P4_MEDIA_WIFI_SOURCE,"media.toggle"));
  assert(submitted_live && submitted!=prior);
  pet_p4_media_authorize_live_playlist_revision("music-b",PET_P4_MEDIA_WIFI_SOURCE,true,true,9,playlist);
  assert(!pet_p4_media_radio_current(submitted));
  cJSON_Delete(playlist);
  pet_p4_media_authorize_live_playlist_revision("music-b",PET_P4_MEDIA_WIFI_SOURCE,true,true,9,NULL);
  pet_p4_media_view(&view);assert(view.count==2);
  pet_p4_media_authorize_live_playlist_revision("music-b",PET_P4_MEDIA_WIFI_SOURCE,false,true,9,NULL);
  assert(!pet_p4_media_live_action(PET_P4_MEDIA_WIFI_SOURCE,"media.toggle"));
#endif
  cJSON_Delete(reply);puts("PASS media stale sessions, seq, queue identities, atomic validation, controls, lease expiry, source-scoped notices");
}
''')
    exe = tmp_path / "media-test"
    results = []
    for deferred in (0, 1):
        subprocess.run(["cc", "-std=c11", "-fsanitize=address,undefined", f"-DTEST_DEFERRED={deferred}", f"-DPET_P4_SYSTEM_WIFI={wifi}", "-Wall", "-Wextra", "-Werror", "-Wno-misleading-indentation", "-Wno-deprecated-declarations", "-include", str(cjson / "cJSON.h"), "-I", str(tmp_path), "-I", str(ROOT / "main"), "-I", str(cjson), str(ROOT / "main/pet_p4_media.c"), str(ROOT / "main/pet_p4_media_codec.c"), str(cjson / "cJSON.c"), str(harness), "-lm", "-o", str(exe)], check=True)
        results.append(subprocess.run([str(exe)], check=True, capture_output=True, text=True, timeout=20).stdout)
    # Every chunk status must match the legacy path byte-for-byte, including
    # failed/stale/duplicate chunks, while the full navigation/lease suite runs.
    assert results[0] == results[1]
