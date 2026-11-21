"""Compile production ingress functions with Wi-Fi ownership enabled/disabled."""
from pathlib import Path
import subprocess
import pytest
from p4_control_integrity_test import _c_function as function

ROOT = Path(__file__).resolve().parents[1]

@pytest.mark.parametrize('wifi', [0, 1])
def test_actual_ingress_owner_recheck_and_bounded_order(tmp_path, wifi):
    source = (ROOT / 'main/pet_p4_audio.c').read_text()
    code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdatomic.h>
#include <pthread.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_INVALID_STATE 2
#define ESP_ERR_NO_MEM 3
#define portMAX_DELAY 0
#define PET_P4_AUDIO_STREAM_RING_BYTES 64
static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t *g_stream_ingress_lock=&mutex;
static atomic_bool g_stream_active;
static atomic_uint g_stream_received_bytes;
static atomic_size_t g_stream_head,g_stream_tail;
static uint8_t ring[64],*g_stream_ring=ring;
static char g_stream_session[64];
#if PET_P4_SYSTEM_WIFI
static atomic_uint g_wifi_audio_owner;
#endif
static void xSemaphoreTake(pthread_mutex_t *m,int wait){(void)wait;assert(!pthread_mutex_lock(m));}
static void xSemaphoreGive(pthread_mutex_t *m){assert(!pthread_mutex_unlock(m));}
'''
    for name in ('stream_buffered', 'stream_session_matches_locked',
                 'pet_p4_audio_stream_matches', 'audio_stream_push_locked',
                 'pet_p4_audio_stream_push_session'):
        code += function(source, name)
    code += r'''
int main(void){
  uint8_t a[]={1,2,3,4},b[]={5,6,7,8};
  strcpy(g_stream_session,"old");g_stream_active=true;
  assert(pet_p4_audio_stream_matches("old"));
#if PET_P4_SYSTEM_WIFI
  // Wi-Fi claims the same textual ID after the early protocol check.
  // Private ownership, not text alone, must reject the wired producer.
  g_wifi_audio_owner=17;
  assert(!pet_p4_audio_stream_matches("old"));
  assert(pet_p4_audio_stream_push_session("old",a,4)==ESP_OK);
  assert(!g_stream_head&&!g_stream_received_bytes);
  g_wifi_audio_owner=0;
#endif
  // Replacement between the early check and final copy.
  strcpy(g_stream_session,"new");
  assert(pet_p4_audio_stream_push_session("old",a,4)==ESP_OK);
  assert(!g_stream_head&&!g_stream_received_bytes);
  assert(pet_p4_audio_stream_push_session("new",a,4)==ESP_OK);
  assert(pet_p4_audio_stream_push_session("new",b,4)==ESP_OK);
  assert(g_stream_head==8&&g_stream_received_bytes==8);
  assert(!memcmp(ring,a,4)&&!memcmp(ring+4,b,4));
  assert(pet_p4_audio_stream_push_session("new",a,SIZE_MAX)==ESP_ERR_NO_MEM);
  assert(pet_p4_audio_stream_push_session("new",NULL,4)==ESP_ERR_INVALID_ARG);
  assert(g_stream_head==8&&g_stream_received_bytes==8);
  g_stream_tail=10; // 62 buffered: existing capacity boundary rejects atomically.
  assert(pet_p4_audio_stream_push_session("new",a,4)==ESP_ERR_NO_MEM);
  assert(g_stream_head==8&&g_stream_received_bytes==8);
  g_stream_active=false;
  assert(pet_p4_audio_stream_push_session("new",a,4)==ESP_OK);
  assert(g_stream_head==8&&g_stream_received_bytes==8);
  return 0;
}
'''
    src = tmp_path / 'ingress.c'; src.write_text(code)
    exe = tmp_path / 'ingress'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-pthread',
                    f'-DPET_P4_SYSTEM_WIFI={wifi}', str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True, timeout=10)
