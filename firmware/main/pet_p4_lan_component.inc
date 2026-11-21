/* One local-only, operation-scoped control request. The socket owner retains its immutable
 * plaintext buffer until the native storage worker returns. */
#define LAN_BUNDLE_MODE 14
#define LAN_COMPONENT_MAX (21 + 32 + 4095 + 2047)
#define LAN_REBOOT_WORK_US 2500000
#define LAN_COMPONENT_WORK_US 55000000
#define LAN_WIDGET_DATA_WORK_US 350000

typedef struct {
  cJSON *manifest;
  const uint8_t *record;
  size_t size;
  uint32_t sequence;
} bundle_job_t;

enum component_phase { COMPONENT_EMPTY, COMPONENT_QUEUED, COMPONENT_RUNNING, COMPONENT_DONE };

static struct {
  enum component_phase phase;
  uint8_t operation;
  uint32_t token;
  char id[33], digest[65];
  const uint8_t *files[2];
  size_t sizes[2];
  int fd;
  int64_t until;
  uint8_t result;
  bool publication_started;
  bundle_job_t *bundle;
  unsigned action;
  const cJSON *keyboard_request;
  cJSON **keyboard_reply;
  uint32_t paired_generation;
  char *appearance_reply;
  const uint8_t *widget_data_board;
} delivery;

static uint32_t next_component_token;
static const char *const component_paths[] = {"buttons.json", "runtime/widget.json"};

static bool component_socket_live(void *unused) {
  (void)unused;
  if (!atomic_load(&control_trusted) || atomic_load(&forget_pending) ||
      !pet_device_wifi_online() || esp_timer_get_time() >= delivery.until) return false;
  uint8_t extra;
  const int got = recv(delivery.fd, &extra, 1, MSG_PEEK);
  return got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);
}

bool pet_p4_lan_appearance_current(void *route) {
  return (uintptr_t)route == delivery.paired_generation &&
    !atomic_load(&usb_client_active) && component_socket_live(NULL);
}

void pet_p4_lan_appearance_reply(const char *line, void *route) {
  if (delivery.appearance_reply && pet_p4_lan_appearance_current(route) &&
      strlen(line) < LAN_APPEARANCE_REPLY_MAX)
    strcpy(delivery.appearance_reply, line);
}

static bool component_publish(void *unused) {
  if (!component_socket_live(unused)) return false;
  delivery.publication_started = true;
  return true;
}

static bool component_hash(char out[65]) {
  mbedtls_sha256_context hash;
  mbedtls_sha256_init(&hash);
  bool ok = mbedtls_sha256_starts(&hash, 0) == 0;
  uint8_t digest[32];
  for (unsigned i = 0; ok && i < 2; ++i) {
    const size_t n = delivery.sizes[i];
    const uint8_t le[4] = {n, n >> 8, 0, 0};
    ok = mbedtls_sha256_update(&hash, (const uint8_t *)component_paths[i],
            strlen(component_paths[i]) + 1) == 0 &&
         mbedtls_sha256_update(&hash, le, sizeof(le)) == 0 &&
         mbedtls_sha256_update(&hash, delivery.files[i], n) == 0;
  }
  ok = ok && mbedtls_sha256_finish(&hash, digest) == 0;
  mbedtls_sha256_free(&hash);
  if (ok) for (unsigned i = 0; i < 32; ++i) snprintf(out + 2*i, 3, "%02x", digest[i]);
  return ok;
}

static bool component_chunk(const char *transfer, const char *path, const uint8_t *data, size_t size,
                            uint32_t file_index, uint32_t offset, uint32_t file_bytes, uint32_t sequence,
                            char *scratch, size_t capacity, char *error, size_t error_size) {
  uint64_t checksum = 0xcbf29ce484222325ULL;
  for (size_t i = 0; i < size; ++i) checksum = (checksum ^ data[i]) * 0x100000001b3ULL;
  size_t encoded;
  char hex[17];
  snprintf(hex, sizeof(hex), "%016llx", (unsigned long long)checksum);
  cJSON *p = cJSON_CreateObject();
  const bool built = p && mbedtls_base64_encode((uint8_t *)scratch, capacity, &encoded, data, size) == 0 &&
    cJSON_AddStringToObject(p, "transferId", transfer) &&
    cJSON_AddStringToObject(p, "path", path) &&
    cJSON_AddStringToObject(p, "data", scratch) &&
    cJSON_AddStringToObject(p, "checksum", hex) &&
    cJSON_AddNumberToObject(p, "index", offset / PET_P4_WIDGET_CHUNK_BYTES) &&
    cJSON_AddNumberToObject(p, "offset", offset) &&
    cJSON_AddNumberToObject(p, "fileIndex", file_index) &&
    cJSON_AddNumberToObject(p, "sequence", sequence) &&
    cJSON_AddNumberToObject(p, "fileBytes", file_bytes) &&
    cJSON_AddNumberToObject(p, "decodedSize", size);
  const bool ok = built && (delivery.bundle
    ? pet_p4_miniapp_lan_bundle_chunk(p, error, error_size)
    : pet_p4_miniapp_transfer_chunk(p, error, error_size));
  cJSON_Delete(p);
  return ok;
}

static uint32_t bundle_u32(const uint8_t *p) {
  return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

__attribute__((optimize("Oz"), noinline))
static bool bundle_begin(bundle_job_t *job, char *error, size_t error_size) {
  const uint8_t *p = job->record;
  if (job->manifest || job->size < 40 || !p[1] || p[1] > job->size - 39 ||
      p[1] >= PET_P4_MINIAPP_WIDGET_ID_MAX || !p[2] || p[2] > 128) return false;
  const size_t version_size = job->size - 39 - p[1];
  const bool replace = p[0] == 4;
  if (replace ? !version_size || version_size > 63 : version_size != 0) return false;
  char id[PET_P4_MINIAPP_WIDGET_ID_MAX], transfer[36], digest[65];
  if (memchr(p + 39, 0, job->size - 39)) return false;
  memcpy(id, p + 39, p[1]);
  id[p[1]] = 0;
  if (replace) {
    if (!pet_p4_miniapp_lan_install_idle() || pet_p4_miniapp_catalog_recovery_blocked()) return false;
    bool matched = false;
    const size_t count = pet_p4_miniapp_catalog_count();
    for (size_t i = 0; i < count; i++) {
      pet_p4_miniapp_catalog_entry_t entry;
      if (pet_p4_miniapp_catalog_get(i, &entry) && !strcmp(entry.widget_id, id)) {
        const char *version = entry.package_metadata.version;
        matched = strlen(version) == version_size && !memcmp(version, p + 39 + p[1], version_size);
        break;
      }
    }
    if (!matched) return false;
  } else if (!pet_p4_miniapp_lan_new_id(id) ||
      pet_p4_miniapp_catalog_count() >= pet_p4_miniapp_catalog_capacity()) return false;
  snprintf(transfer, sizeof(transfer), "lan-%08lx%08lx", (unsigned long)esp_random(), (unsigned long)esp_random());
  trust_hex(digest, p + 7, 32);
  cJSON *manifest = cJSON_CreateObject();
  if (manifest && cJSON_AddStringToObject(manifest, "transferId", transfer) &&
      cJSON_AddStringToObject(manifest, "widgetId", id) &&
      cJSON_AddStringToObject(manifest, "transferProtocol", PET_P4_WIDGET_TRANSFER_PROTOCOL) &&
      cJSON_AddStringToObject(manifest, "packageSha256", digest) &&
      cJSON_AddNumberToObject(manifest, "fileCount", p[2]) &&
      cJSON_AddNumberToObject(manifest, "totalBytes", bundle_u32(p + 3)) &&
      pet_p4_miniapp_lan_bundle_begin(manifest, error, error_size)) {
    job->manifest = manifest;
    return true;
  }
  cJSON_Delete(manifest);
  return false;
}

static bool bundle_step(bundle_job_t *job, bool admitted) {
  if (!job->record) {
    if (job->manifest) pet_p4_miniapp_lan_abort(job->manifest);
    return true;
  }
  if (!admitted || !component_socket_live(NULL)) return false;
  char error[160] = {0};
  const uint8_t *p = job->record;
  if (p[0] == 1 || p[0] == 4) return bundle_begin(job, error, sizeof(error));
  if (!job->manifest) return false;
  if (p[0] == 3 && job->size == 1)
    return pet_p4_miniapp_lan_commit(job->manifest, component_publish, NULL, error, sizeof(error));
  if (p[0] != 2 || job->size < 13 || !p[10] || p[10] >= 96 ||
      job->size <= 11U + p[10] || memchr(p + 11, 0, p[10])) return false;
  const size_t size = job->size - 11U - p[10];
  if (size > PET_P4_WIDGET_CHUNK_BYTES) return false;
  char path[96];
  memcpy(path, p + 11, p[10]);
  path[p[10]] = 0;
  const size_t capacity = ((size + 2) / 3) * 4 + 1;
  char *scratch = heap_caps_malloc(capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!scratch) return false;
  const cJSON *transfer = cJSON_GetObjectItemCaseSensitive(job->manifest, "transferId");
  const bool ok = component_chunk(transfer->valuestring, path, p + 11 + p[10], size,
    p[1], bundle_u32(p + 2), bundle_u32(p + 6), job->sequence, scratch, capacity, error, sizeof(error));
  free(scratch);
  if (ok) ++job->sequence;
  return ok;
}

void pet_p4_lan_control_run(pet_p4_runtime_state_t *state, uint32_t token, bool admitted) {
  if (!lock) return;
  xSemaphoreTake(lock, portMAX_DELAY);
  if (delivery.phase != COMPONENT_QUEUED || delivery.token != token) {
    xSemaphoreGive(lock);
    return;
  }
  delivery.phase = COMPONENT_RUNNING;
  xSemaphoreGive(lock);

  bool installed = false;
  bool published = false;
  if (delivery.operation != PET_LAN_CONTROL_APPEARANCE || delivery.bundle)
    admitted = admitted && !state->asset_transfer_active && !pet_p4_appearance_active() &&
      !pet_p4_appearance_mutation_blocked();
  if (delivery.operation == PET_LAN_CONTROL_APPEARANCE && !delivery.bundle) {
    void *route = (void *)(uintptr_t)delivery.paired_generation;
    if (admitted && pet_p4_lan_appearance_current(route))
      pet_p4_handle_lan_appearance(state, delivery.action, delivery.keyboard_request, route);
    installed = delivery.appearance_reply[0] != 0;
  } else if (delivery.bundle) {
    installed = bundle_step(delivery.bundle, admitted);
    published = installed && delivery.publication_started;
  } else if (delivery.operation == PET_LAN_CONTROL_KEYBOARD) {
    if (admitted && !atomic_load(&usb_client_active) && component_socket_live(NULL)) {
      *delivery.keyboard_reply = pet_p4_keyboard_config(delivery.keyboard_request, state->board_device_id);
      installed = *delivery.keyboard_reply != NULL;
    }
  } else if (delivery.operation == PET_LAN_CONTROL_PANEL) {
    /* Linearize each bitmap phase with pairing revocation. The state lane
     * already excludes concurrent USB protocol application. */
    const bool pixels = delivery.action == PET_LAN_PANEL_TEXT_PIXELS;
    if (pixels) xSemaphoreTake(lock, portMAX_DELAY);
    if ((!pixels || (delivery.paired_generation == generation && volume_available()
          && !pet_p4_state_usb_client_connected(state, clock_ms())))
        && !atomic_load(&usb_client_active) && component_socket_live(NULL)) {
      *delivery.keyboard_reply = lan_panel_run(delivery.action, delivery.keyboard_request, state, admitted);
      installed = *delivery.keyboard_reply != NULL;
    }
    if (pixels) xSemaphoreGive(lock);
  } else if (delivery.operation == PET_LAN_CONTROL_WIDGET_DATA) {
    const uint64_t now = clock_ms();
    if (admitted && !atomic_load(&usb_client_active) &&
        !pet_p4_state_usb_client_connected(state, now) &&
        strlen(state->board_device_id) == LAN_WIDGET_BOARD_BYTES &&
        !memcmp(delivery.widget_data_board, state->board_device_id, LAN_WIDGET_BOARD_BYTES) &&
        component_socket_live(NULL) && pet_p4_widget_data_apply_lan(delivery.keyboard_request, now)) {
      state->last_update_ms += 1;
      installed = true;
    }
  } else if (admitted && delivery.operation == PET_LAN_CONTROL_REBOOT) {
    installed = pet_p4_miniapp_lan_install_idle() && component_socket_live(NULL) &&
      pet_p4_diagnostics_schedule_reboot(state);
  } else if (admitted && component_socket_live(NULL) && pet_p4_miniapp_lan_new_id(delivery.id) &&
      pet_p4_miniapp_catalog_count() < pet_p4_miniapp_catalog_capacity()) {
    char transfer[32], error[160] = {0};
    snprintf(transfer, sizeof(transfer), "lan-%08lx%08lx", (unsigned long)esp_random(), (unsigned long)esp_random());
    const size_t capacity = ((4095 + 2) / 3) * 4 + 1;
    char *scratch = heap_caps_malloc(capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    cJSON *p = cJSON_CreateObject();
    const bool built = scratch && p && cJSON_AddStringToObject(p, "transferId", transfer) &&
      cJSON_AddStringToObject(p, "widgetId", delivery.id) &&
      cJSON_AddStringToObject(p, "transferProtocol", PET_P4_WIDGET_TRANSFER_PROTOCOL) &&
      cJSON_AddStringToObject(p, "packageSha256", delivery.digest) &&
      cJSON_AddNumberToObject(p, "fileCount", 2) &&
      cJSON_AddNumberToObject(p, "totalBytes", delivery.sizes[0] + delivery.sizes[1]);
    if (built && pet_p4_miniapp_transfer_begin(p, error, sizeof(error))) {
      installed = true;
      for (unsigned i = 0; installed && i < 2; ++i)
        installed = component_chunk(transfer, component_paths[i], delivery.files[i], delivery.sizes[i],
          i, 0, delivery.sizes[i], i, scratch, capacity, error, sizeof(error));
      installed = installed &&
        component_socket_live(NULL) &&
        pet_p4_miniapp_lan_commit(p, component_publish, NULL, error, sizeof(error));
      if (!installed) pet_p4_miniapp_lan_abort(p);
    }
    cJSON_Delete(p);
    free(scratch);
  }
  if (published || (installed && delivery.operation == PET_LAN_CONTROL_COMPONENT)) {
    pet_p4_miniapp_sync_stats(&state->stats);
    state->last_update_ms += 1;
  }

  xSemaphoreTake(lock, portMAX_DELAY);
  delivery.result = installed ? 1 : delivery.publication_started ? 2 : 0;
  delivery.phase = COMPONENT_DONE;
  xSemaphoreGive(lock);
}

static bool component_declarative(const char *widget, size_t size) {
  static const char *const excluded[] = {
    "hachiWasm", "hachiCanvas", "hachiHost", "hachiAudio", "hachiAudioSha256",
    "data", "media", "instrument", "fetchers", "readers", "host", "host_context",
    "host_contract", "program"
  };
  cJSON *root = cJSON_ParseWithLengthOpts(widget, size + 1, NULL, true);
  bool ok = cJSON_IsObject(root);
  for (size_t i = 0; ok && i < sizeof(excluded) / sizeof(*excluded); ++i)
    ok = !cJSON_HasObjectItem(root, excluded[i]);
  cJSON_Delete(root);
  return ok;
}

/* Component extension after the common boot ID: ID length, file lengths,
 * component ID and the exact immutable file bytes. */
static bool prepare_component(uint8_t *body, size_t size) {
  const size_t id_size = body[16], buttons_size = (size_t)body[17] << 8 | body[18];
  const size_t widget_size = (size_t)body[19] << 8 | body[20];
  if (!id_size || id_size > 32 || !buttons_size || buttons_size > 2047 ||
      !widget_size || widget_size > 4095 || size != 21 + id_size + buttons_size + widget_size ||
      body[21] < 'a' || body[21] > 'z') return 0;
  for (size_t i = 0; i < id_size; ++i) {
    const uint8_t c = body[21 + i];
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return 0;
  }
  const uint8_t *buttons = body + 21 + id_size, *widget = buttons + buttons_size;
  body[size] = 0;  /* The LAN plaintext buffer reserves this terminator. */
  if (!pet_json_source_text_limit_bytes(buttons, buttons_size, 16) ||
      !pet_json_source_text_limit_bytes(widget, widget_size, 16) ||
      !component_declarative((const char *)widget, widget_size)) return 0;

  memcpy(delivery.id, body + 21, id_size);
  delivery.id[id_size] = 0;
  delivery.files[0] = buttons;
  delivery.files[1] = widget;
  delivery.sizes[0] = buttons_size;
  delivery.sizes[1] = widget_size;
  return component_hash(delivery.digest);
}

/* The LAN task retains the record until the serialized worker relinquishes it. */
static uint8_t queue_delivery(bool cleanup) {
  delivery.publication_started = false;
  if (++next_component_token == 0) ++next_component_token;
  delivery.token = next_component_token;
  if ((!cleanup && !component_socket_live(NULL)) || !pet_p4_lan_schedule_control(delivery.token)) {
    xSemaphoreGive(lock);
    return 0;
  }
  delivery.phase = COMPONENT_QUEUED;
  xSemaphoreGive(lock);

  for (;;) {
    xSemaphoreTake(lock, portMAX_DELAY);
    if (delivery.phase == COMPONENT_DONE) {
      const uint8_t result = delivery.result;
      delivery.phase = COMPONENT_EMPTY;
      xSemaphoreGive(lock);
      return result;
    }
    if (!cleanup && delivery.phase != COMPONENT_RUNNING) {
      if (!component_socket_live(NULL)) {
        delivery.phase = COMPONENT_EMPTY;
        xSemaphoreGive(lock);
        return 0;
      }
    }
    xSemaphoreGive(lock);
    vTaskDelay(delivery.bundle || delivery.operation == PET_LAN_CONTROL_APPEARANCE ? 1 : pdMS_TO_TICKS(50));
  }
}

static uint8_t control_deliver(int fd, unsigned operation, uint8_t *body, size_t size,
                               uint32_t paired_generation, const cJSON *keyboard_request,
                               cJSON **keyboard_reply, char *appearance_reply) {
  if (strlen(pet_p4_boot_identity()) != 16 || memcmp(body, pet_p4_boot_identity(), 16)) return 0;

  xSemaphoreTake(lock, portMAX_DELAY);
  if (delivery.phase != COMPONENT_EMPTY || !volume_available() || paired_generation != generation ||
      !atomic_load(&control_trusted)) {
    xSemaphoreGive(lock);
    return 0;
  }
  if (operation == PET_LAN_CONTROL_COMPONENT && !prepare_component(body, size)) {
    xSemaphoreGive(lock);
    return 0;
  }
  delivery.operation = operation;
  delivery.action = (operation == PET_LAN_CONTROL_PANEL ||
    operation == PET_LAN_CONTROL_APPEARANCE) ? body[16] : 0;
  delivery.paired_generation = paired_generation;
  delivery.appearance_reply = appearance_reply;
  delivery.fd = fd;
  delivery.bundle = NULL;
  delivery.keyboard_request = keyboard_request;
  delivery.keyboard_reply = keyboard_reply;
  if (operation == PET_LAN_CONTROL_WIDGET_DATA) delivery.widget_data_board = body + 16;
  delivery.until = esp_timer_get_time() +
    (operation == PET_LAN_CONTROL_WIDGET_DATA ? LAN_WIDGET_DATA_WORK_US :
     (operation == PET_LAN_CONTROL_COMPONENT || operation == PET_LAN_CONTROL_APPEARANCE)
      ? LAN_COMPONENT_WORK_US : LAN_REBOOT_WORK_US);
  return queue_delivery(false);
}
