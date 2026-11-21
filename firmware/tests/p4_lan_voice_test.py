import subprocess
from pathlib import Path


def test_physical_hold_owns_wireless_dictation_and_confirmation(tmp_path):
    main = Path(__file__).resolve().parents[1] / "main"
    harness = tmp_path / "voice.c"
    harness.write_text(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <string.h>

typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
#define pdTRUE 1
static int lock = 1;
static bool paired = true, online = true;
static atomic_bool forget_pending, usb_client_active;
static atomic_bool control_trusted = true;
static uint32_t generation = 8;
static uint64_t now = 1000;
static uint64_t clock_ms(void) { return now; }
static int xSemaphoreTake(int l, int t) { (void)l; (void)t; return 1; }
static void xSemaphoreGive(int l) { (void)l; }
static bool pet_device_wifi_online(void) { return online; }
static void ota_wire_put_u32(uint8_t *p, uint32_t n) {
  for (unsigned i = 0; i < 4; ++i) p[i] = n >> (24 - i * 8);
}
typedef struct { unsigned session_queue_count; } pet_p4_runtime_state_t;
#include "pet_p4_lan_voice.inc"

int main(void) {
  pet_p4_runtime_state_t state = {0};
  bool empty = false;
  uint8_t reply[8] = {0};
  assert(!pet_p4_lan_voice_press(&state, 1, now));
  assert(!lan_voice_claim(1, generation, &empty));
  pet_p4_lan_voice_button(1, true);
  assert(pet_p4_lan_voice_press(&state, 1, now));
  lan_voice_status(generation, reply);
  assert(reply[3] == 1 && !reply[7]);
  assert(!lan_voice_claim(1, generation + 1, &empty));
  assert(lan_voice_claim(1, generation, &empty) && empty);
  assert(!lan_voice_claim(1, generation, &empty));
  atomic_store(&voice_mic_token, 1);
  assert(pet_p4_lan_voice_mic_allowed());
  atomic_store(&usb_client_active, true);
  assert(!pet_p4_lan_voice_mic_allowed());
  atomic_store(&usb_client_active, false);
  pet_p4_lan_voice_button(2, false);
  assert(pet_p4_lan_voice_mic_allowed());
  pet_p4_lan_voice_button(1, false);
  assert(!pet_p4_lan_voice_mic_allowed() && lan_voice_released(1));
  assert(pet_p4_lan_voice_confirm(++now));
  assert(!pet_p4_lan_voice_confirm(now));
  memset(reply, 0, sizeof(reply));
  lan_voice_status(generation, reply);
  assert(!reply[3] && reply[7] == 1);
  pet_p4_lan_voice_button(1, true);
  assert(pet_p4_lan_voice_press(&state, 1, ++now));
  assert(!lan_voice_released(1));
  now += LAN_VOICE_PEEK_MS;
  assert(!lan_voice_claim(2, generation, &empty));
  pet_p4_lan_voice_button(1, false);
  assert(!lan_voice_released(2) && !pet_p4_lan_voice_confirm(now));
  pet_p4_lan_voice_cancel();
}
''')
    binary = tmp_path / "voice"
    subprocess.run(["cc", "-std=c11", "-Wall", "-Werror", "-I", str(main),
                    str(harness), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
