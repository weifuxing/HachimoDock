/* A press is issued locally and claimable once by its paired desktop.
 * Mode 20 is the small control exchange. Mode 13 carries PCM. */
enum {
  LAN_VOICE_MODE = 20,
  LAN_VOICE_PEEK_MS = 1800,
  LAN_VOICE_CAPTURE_MS = 29000,
  LAN_VOICE_CONFIRM_MS = 20000,
  LAN_VOICE_STATUS_BYTES = 8,
  LAN_VOICE_OFFER_BYTES = 20,
};

static portMUX_TYPE voice_mux = portMUX_INITIALIZER_UNLOCKED;
static struct {
  uint32_t token, generation;
  uint64_t pressed, released, confirmed;
  bool held, claimed, queue_empty;
  uint8_t button, physical_holds;
} lan_voice;
static atomic_uint voice_mic_token;

bool pet_p4_lan_voice_press(const pet_p4_runtime_state_t *state, uint8_t button, uint64_t now) {
  if (!lock || xSemaphoreTake(lock, 0) != pdTRUE) return false;
  bool allowed = paired && !atomic_load(&forget_pending) &&
    atomic_load(&control_trusted) && !atomic_load(&usb_client_active) &&
    pet_device_wifi_online();
  portENTER_CRITICAL(&voice_mux);
  allowed = allowed && (lan_voice.physical_holds & (1u << button)) && !lan_voice.held;
  if (allowed) {
    if (!++lan_voice.token) ++lan_voice.token;
    lan_voice.generation = generation;
    lan_voice.button = button;
    lan_voice.pressed = now;
    lan_voice.released = lan_voice.confirmed = 0;
    lan_voice.held = true;
    lan_voice.claimed = false;
    lan_voice.queue_empty = state->session_queue_count == 0;
  }
  portEXIT_CRITICAL(&voice_mux);
  xSemaphoreGive(lock);
  return allowed;
}

/* Runs at the physical button sampler, even when the UI input queue is full. */
void pet_p4_lan_voice_button(uint8_t button, bool held) {
  const uint64_t now = clock_ms();
  portENTER_CRITICAL(&voice_mux);
  if (held) {
    lan_voice.physical_holds |= 1u << button;
  } else {
    lan_voice.physical_holds &= ~(1u << button);
    if (lan_voice.held && lan_voice.button == button) {
      if (lan_voice.claimed && now - lan_voice.pressed < LAN_VOICE_CAPTURE_MS)
        lan_voice.released = now;
      lan_voice.held = false;
    }
  }
  portEXIT_CRITICAL(&voice_mux);
}

void pet_p4_lan_voice_cancel(void) {
  portENTER_CRITICAL(&voice_mux);
  lan_voice.held = false;
  lan_voice.released = lan_voice.confirmed = 0;
  portEXIT_CRITICAL(&voice_mux);
}

bool pet_p4_lan_voice_confirm(uint64_t now) {
  portENTER_CRITICAL(&voice_mux);
  const bool confirm = lan_voice.released && !lan_voice.confirmed &&
    now - lan_voice.released < LAN_VOICE_CONFIRM_MS;
  if (confirm) lan_voice.confirmed = now;
  portEXIT_CRITICAL(&voice_mux);
  return confirm;
}

static bool lan_voice_claim(uint32_t token, uint32_t pair_generation, bool *empty) {
  const uint64_t now = clock_ms();
  portENTER_CRITICAL(&voice_mux);
  const bool allowed = token && token == lan_voice.token && lan_voice.held &&
    !lan_voice.claimed && lan_voice.generation == pair_generation &&
    now - lan_voice.pressed < LAN_VOICE_PEEK_MS;
  if (allowed) {
    lan_voice.claimed = true;
    *empty = lan_voice.queue_empty;
  }
  portEXIT_CRITICAL(&voice_mux);
  return allowed;
}

bool pet_p4_lan_voice_mic_allowed(void) {
  const uint32_t token = atomic_load(&voice_mic_token);
  if (!token || atomic_load(&usb_client_active) || atomic_load(&forget_pending) ||
      !atomic_load(&control_trusted) || !pet_device_wifi_online()) return false;
  const uint64_t now = clock_ms();
  portENTER_CRITICAL(&voice_mux);
  const bool held = token == lan_voice.token && lan_voice.held &&
    now - lan_voice.pressed < LAN_VOICE_CAPTURE_MS;
  portEXIT_CRITICAL(&voice_mux);
  return held;
}

static bool lan_voice_released(uint32_t token) {
  portENTER_CRITICAL(&voice_mux);
  const bool released = token == lan_voice.token && lan_voice.released != 0;
  portEXIT_CRITICAL(&voice_mux);
  return released;
}

static void lan_voice_status(uint32_t pair_generation, uint8_t reply[LAN_VOICE_STATUS_BYTES]) {
  const uint64_t now = clock_ms();
  portENTER_CRITICAL(&voice_mux);
  if (lan_voice.generation == pair_generation) {
    if (lan_voice.held && !lan_voice.claimed && now - lan_voice.pressed < LAN_VOICE_PEEK_MS)
      ota_wire_put_u32(reply, lan_voice.token);
    if (lan_voice.confirmed && now - lan_voice.confirmed < LAN_VOICE_PEEK_MS)
      ota_wire_put_u32(reply + 4, lan_voice.token);
  }
  portEXIT_CRITICAL(&voice_mux);
}
