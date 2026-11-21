/* Fixed, authenticated optional bitmap operation. Payloads and receiver are
 * identical to USB; this is not a general protocol dispatch facility. */
#include "pet_p4_bubble_pixels.h"

static bool lan_bubble_valid(const cJSON *request) {
  const cJSON *phase = cJSON_GetObjectItemCaseSensitive(request, "phase");
  if (!cJSON_IsObject(request) || !cJSON_IsString(phase)) return false;
  const char *p = phase->valuestring;
  return !strcmp(p, "capabilities") || !strcmp(p, "need") || !strcmp(p, "begin")
    || !strcmp(p, "chunk") || !strcmp(p, "end") || !strcmp(p, "abort") || !strcmp(p, "status");
}

/* Called on the state lane with the pairing lock held. No socket IO here. */
static cJSON *lan_bubble_run(const cJSON *request, pet_p4_runtime_state_t *state, bool admitted) {
  cJSON *reply = cJSON_CreateObject();
  if (!reply) return NULL;
  static const char *const fields[] = {"requestId", "phase", "client", "owner", "version", "transfer", "id"};
  for (unsigned i = 0; i < sizeof(fields) / sizeof(*fields); ++i) {
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(request, fields[i]);
    cJSON_AddStringToObject(reply, fields[i], cJSON_IsString(value) && strlen(value->valuestring) <= 64 ? value->valuestring : "");
  }
  cJSON_AddStringToObject(reply, "boardDeviceId", state->board_device_id);
  const char *phase = cJSON_GetObjectItemCaseSensitive(request, "phase")->valuestring;
  const uint64_t now = clock_ms();
  const bool blocked = !admitted || pet_p4_audio_active() || pet_p4_audio_playback_active()
    || pet_p4_audio_stream_playing() || pet_p4_audio_music_active()
    || pet_p4_ota_active() || state->asset_transfer_active;
  pet_bubble_pixels_maintain(now, blocked, pet_p4_audio_conversation_active());
  if (!strcmp(phase, "capabilities") || !strcmp(phase, "need")) {
    cJSON_AddBoolToObject(reply, "ok", !blocked || !strcmp(phase, "capabilities"));
    cJSON_AddStringToObject(reply, "schema", "p4-text-alpha-v2");
    cJSON_AddStringToObject(reply, "boot", pet_bubble_pixels_boot());
    if (!strcmp(phase, "capabilities")) cJSON_AddBoolToObject(reply, "glyphCells", true);
    if (!strcmp(phase, "capabilities")) cJSON_AddBoolToObject(reply, "bodyMasks", true);
    if (!strcmp(phase, "capabilities")) cJSON_AddBoolToObject(reply, "titleMasks", true);
    pet_bubble_need_t need;
    if (!strcmp(phase, "need") && !blocked && pet_bubble_pixels_next_lan_need(now, &need)) {
      cJSON *n = cJSON_AddObjectToObject(reply, "need");
      if (n) {
        cJSON_AddStringToObject(n, "boot", need.boot);
        cJSON_AddStringToObject(n, "client", need.client);
        cJSON_AddStringToObject(n, "owner", need.owner);
        cJSON_AddStringToObject(n, "version", need.version);
        cJSON_AddStringToObject(n, "id", need.id);
        cJSON_AddStringToObject(n, "text", need.text);
        cJSON_AddNumberToObject(n, "style", need.style);
        cJSON_AddNumberToObject(n, "page", need.page);
        cJSON_AddBoolToObject(n, "glyphCells", need.glyph_cells);
        cJSON_AddBoolToObject(n, "bodyMasks", need.body_mask);
      }
    }
  } else {
    (void)pet_bubble_pixels_handle_owned(request, PET_BUBBLE_LAN_ROUTE, now, reply);
  }
  return reply;
}
