/* NVS marker authenticates the exact enrolled key. Old firmware can still
 * read the 16-byte enrollment record and a replacement key cannot inherit it. */
static atomic_bool control_trusted;
static struct {
  uint8_t message[36];  /* nonce[16], current boot ID[16], generation BE[4] */
  int64_t expires;
  pet_p4_send_line_fn sender;
  void *route;
} trust_challenge;

static bool trust_marker(uint8_t tag[32]) {
  static const uint8_t context[] = "P4LAN-WIRED-TRUST-v1";
  return mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
    paired_key, sizeof(paired_key), context, sizeof(context) - 1, tag) == 0;
}

/* Caller holds the LAN lock. */
static bool trust_storage(nvs_handle_t handle, bool write) {
  uint8_t tag[32], saved[16];
  size_t size = sizeof(saved);
  const bool computed = trust_marker(tag);
  const bool ok = computed && (write
    ? nvs_set_blob(handle, "wired_trust", tag, sizeof(saved)) == ESP_OK && nvs_commit(handle) == ESP_OK
    : nvs_get_blob(handle, "wired_trust", saved, &size) == ESP_OK &&
      size == sizeof(saved) && mbedtls_ct_memcmp(saved, tag, size) == 0);
  mbedtls_platform_zeroize(tag, sizeof(tag));
  return ok;
}

static void trust_hex(char *out, const uint8_t *bytes, size_t size) {
  static const char hex[] = "0123456789abcdef";
  for (size_t i = 0; i < size; ++i) {
    out[2*i] = hex[bytes[i] >> 4];
    out[2*i+1] = hex[bytes[i] & 15];
  }
  out[2*size] = 0;
}

cJSON *pet_p4_lan_trust_usb(const cJSON *payload, pet_p4_send_line_fn send, void *route) {
  cJSON *reply = cJSON_CreateObject();
  if (!reply) return NULL;
  const cJSON *request_id = cJSON_GetObjectItemCaseSensitive(payload, "requestId");
  const cJSON *proof = cJSON_GetObjectItemCaseSensitive(payload, "proof");
  const char *boot = pet_p4_boot_identity();
  bool ok = false;
  if (cJSON_IsString(request_id) && strlen(request_id->valuestring) < 80)
    cJSON_AddStringToObject(reply, "requestId", request_id->valuestring);
  if (!lock || !pet_p4_lan_trust_wired(send, route) || strlen(boot) != 16 ||
      xSemaphoreTake(lock, 0) != pdTRUE) goto done;
  if (!paired || atomic_load(&forget_pending)) goto unlock;
  if (!proof) {
    esp_fill_random(trust_challenge.message, 16);
    memcpy(trust_challenge.message + 16, boot, 16);
    for (unsigned i = 0; i < 4; ++i)
      trust_challenge.message[32+i] = generation >> (24-8*i);
    trust_challenge.expires = esp_timer_get_time() + 5000000;
    trust_challenge.sender = send;
    trust_challenge.route = route;
    char challenge[73];
    trust_hex(challenge, trust_challenge.message, sizeof(trust_challenge.message));
    ok = cJSON_AddStringToObject(reply, "challenge", challenge) != NULL;
  } else if (trust_challenge.sender == send && trust_challenge.route == route &&
             esp_timer_get_time() < trust_challenge.expires) {
    trust_challenge.expires = 0;
    uint32_t scoped_generation = 0;
    for (unsigned i = 0; i < 4; ++i)
      scoped_generation = (scoped_generation << 8) | trust_challenge.message[32+i];
    uint8_t digest[32];
    if (cJSON_IsString(proof) && strlen(proof->valuestring) == 64 &&
        scoped_generation == generation && !memcmp(trust_challenge.message + 16, boot, 16) &&
        mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
          paired_key, sizeof(paired_key), trust_challenge.message,
          sizeof(trust_challenge.message), digest) == 0) {
      char expected[65];
      trust_hex(expected, digest, sizeof(digest));
      nvs_handle_t handle;
      if (mbedtls_ct_memcmp(expected, proof->valuestring, 64) == 0 &&
          pet_p4_lan_trust_wired(send, route) && !atomic_load(&forget_pending) &&
          nvs_open("p4_agent_lan", NVS_READWRITE, &handle) == ESP_OK) {
        ok = trust_storage(handle, true);
        nvs_close(handle);
        if (ok) atomic_store(&control_trusted, true);
      }
      mbedtls_platform_zeroize(digest, sizeof(digest));
      mbedtls_platform_zeroize(expected, sizeof(expected));
    }
  }
unlock:
  xSemaphoreGive(lock);
done:
  cJSON_AddBoolToObject(reply, "ok", ok);
  return reply;
}
