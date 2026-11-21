/* Fixed operations for the authenticated desktop panel. These run on the
 * existing protocol owner and never dispatch an arbitrary protocol topic. */
#include "pet_p4_audio.h"
#include "pet_p4_audio_volume.h"
#include "pet_p4_input.h"
#include "pet_p4_lan_bubble.inc"

__attribute__((optimize("Oz"), noinline))
static bool lan_panel_valid_input(const cJSON *request) {
  const cJSON *bindings = cJSON_GetObjectItemCaseSensitive(request, "bindings");
  const int count = cJSON_GetArraySize(bindings);
  if (!cJSON_IsObject(request) || cJSON_GetArraySize(request) != 1 || !cJSON_IsArray(bindings)
      || count < 1 || count > PET_P4_INPUT_MAX_BINDINGS) return false;
  const cJSON *row;
  cJSON_ArrayForEach(row, bindings) {
    const cJSON *event = cJSON_GetObjectItemCaseSensitive(row, "event");
    const cJSON *action = cJSON_GetObjectItemCaseSensitive(row, "action");
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(row, "value");
    if (!cJSON_IsObject(row) || !cJSON_IsString(event) || !event->valuestring[0]
        || !cJSON_IsString(action) || !action->valuestring[0] || (value && !cJSON_IsString(value))
        || cJSON_GetArraySize(row) != (value ? 3 : 2)) return false;
    for (const cJSON *previous = bindings->child; previous != row; previous = previous->next) {
      const cJSON *prior_event = cJSON_GetObjectItemCaseSensitive(previous, "event");
      if (!strcmp(event->valuestring, prior_event->valuestring)) return false;
    }
  }
  return true;
}

__attribute__((optimize("Oz"), noinline))
static bool lan_panel_remove_valid(const cJSON *request) {
  const cJSON *id = cJSON_GetObjectItemCaseSensitive(request, "componentId");
  return cJSON_IsObject(request) && cJSON_GetArraySize(request) == 2
    && cJSON_IsString(id) && id->valuestring[0]
    && strlen(id->valuestring) < PET_P4_MINIAPP_WIDGET_ID_MAX
    && cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(request, "preserveSave"));
}

__attribute__((optimize("Oz"), noinline))
static cJSON *lan_panel_run(unsigned action, const cJSON *request, pet_p4_runtime_state_t *state, bool admitted) {
  const char *error = NULL;
  char storage_error[160] = {0};
  switch (action) {
    case PET_LAN_PANEL_TEXT_PIXELS:
      return lan_bubble_run(request, state, admitted);
    case PET_LAN_PANEL_WRITE_INPUT:
      if (!admitted) error = "device busy; input configuration was not written";
      else if (!pet_p4_input_handle_config(request, false, NULL, NULL)) error = "device rejected input configuration";
      else return pet_p4_input_config_state(NULL, state->board_device_id);
      break;
    case PET_LAN_PANEL_READ_INPUT:
      return pet_p4_input_config_state(NULL, state->board_device_id);
    case PET_LAN_PANEL_RESET_INPUT:
      if (!admitted) error = "device busy; input configuration was not reset";
      else if (pet_p4_input_reset_config() != ESP_OK) error = "could not restore default input configuration";
      else return pet_p4_input_config_state(NULL, state->board_device_id);
      break;
    case PET_LAN_PANEL_ENABLE_MICROPHONE:
    case PET_LAN_PANEL_DISABLE_MICROPHONE: {
      const bool enable = action == PET_LAN_PANEL_ENABLE_MICROPHONE;
      if (!admitted) error = "device busy; microphone setting was not changed";
      else if (pet_p4_audio_wifi_ptt_enable(enable) != ESP_OK) error = "device microphone unavailable";
      else if (!enable) pet_p4_lan_voice_cancel();
      break;
    }
    case PET_LAN_PANEL_READ_VOLUME: {
      pet_audio_volume_status_t volume;
      if (pet_audio_volume_get(&volume) != ESP_OK || !volume.confirmed) {
        error = "could not read current audio output volume";
        break;
      }
      cJSON *reply = cJSON_CreateObject();
      if (reply) {
        cJSON_AddStringToObject(reply, "boardDeviceId", state->board_device_id);
        cJSON_AddBoolToObject(reply, "ok", true);
        cJSON_AddStringToObject(reply, "engine", PET_AUDIO_MASTER_VOLUME_ENGINE);
        cJSON_AddNumberToObject(reply, "volume", volume.volume);
        cJSON_AddBoolToObject(reply, "persisted", volume.persisted);
        cJSON_AddBoolToObject(reply, "savePending", volume.save_pending);
      }
      return reply;
    }
    case PET_LAN_PANEL_READ_DIAGNOSTICS:
      return pet_p4_diagnostics_lan_snapshot(state);
    case PET_LAN_PANEL_RETURN_HOME:
      if (!admitted) error = "device busy; screen was not changed";
      else {
        snprintf(state->screen_page, sizeof(state->screen_page), "%s", "main");
        ++state->last_update_ms;
      }
      break;
    case PET_LAN_PANEL_READ_COMPONENTS:
      return pet_p4_widget_inventory(state, "lan");
    case PET_LAN_PANEL_REMOVE_COMPONENT: {
      if (!admitted) {
        error = "device busy; component was not removed";
        break;
      }
      const char *id = cJSON_GetObjectItemCaseSensitive(request, "componentId")->valuestring;
      const bool preserve = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(request, "preserveSave"));
      const bool removed = preserve
        ? pet_p4_miniapp_remove_preserving_save(id, storage_error, sizeof(storage_error))
        : pet_p4_miniapp_remove(id, storage_error, sizeof(storage_error));
      if (!removed) error = storage_error[0] ? storage_error : "could not remove component";
      else {
        snprintf(state->screen_page, sizeof(state->screen_page), "%s", "main");
        ++state->last_update_ms;
      }
      break;
    }
    default:
      return NULL;
  }
  cJSON *reply = cJSON_CreateObject();
  if (reply) {
    cJSON_AddStringToObject(reply, "boardDeviceId", state->board_device_id);
    cJSON_AddBoolToObject(reply, "ok", !error);
    if (error) cJSON_AddStringToObject(reply, "message", error);
  }
  return reply;
}
