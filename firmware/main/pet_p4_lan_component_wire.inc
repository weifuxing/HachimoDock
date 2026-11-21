/* Include after socket_io/send_sealed_response are defined. The same fixed
 * LAN scratch buffers are reused for each sequential, authenticated record. */
#define LAN_BUNDLE_FRAME_MAX (11 + 95 + PET_P4_WIDGET_CHUNK_BYTES)
#define LAN_BUNDLE_RECORD_LIMIT 4352
#define LAN_BUNDLE_SESSION_US 360000000
#define LAN_BUNDLE_IDLE_US 4000000

static bool bundle_wire_trusted(uint32_t request_generation) {
  xSemaphoreTake(lock, portMAX_DELAY);
  const bool ok = volume_available() && generation == request_generation && atomic_load(&control_trusted);
  xSemaphoreGive(lock);
  return ok;
}

static bool bundle_wire_admit(const uint8_t *boot, uint32_t request_generation) {
  const char *current = pet_p4_boot_identity();
  return strlen(current) == 16 && !memcmp(boot, current, 16) && bundle_wire_trusted(request_generation);
}

static uint8_t bundle_wire_run(int fd, bundle_job_t *job, int64_t deadline) {
  xSemaphoreTake(lock, portMAX_DELAY);
  delivery.operation = LAN_BUNDLE_MODE;
  delivery.fd = fd;
  delivery.bundle = job;
  delivery.until = deadline;
  return queue_delivery(!job->record);
}

static void bundle_wire_session(int fd, mbedtls_gcm_context *gcm, uint8_t *cipher,
                                uint8_t *plain, uint32_t request_generation) {
  bundle_job_t job = {0};
  const int enabled = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled));
  const int64_t end = esp_timer_get_time() + LAN_BUNDLE_SESSION_US;
  for (uint32_t sequence = 0; sequence < LAN_BUNDLE_RECORD_LIMIT &&
       bundle_wire_trusted(request_generation); ++sequence) {
    const int64_t next = esp_timer_get_time() + LAN_BUNDLE_IDLE_US;
    const int64_t deadline = next < end ? next : end;
    uint8_t length[2], nonce[12] = {2};
    nonce[10] = sequence >> 8;
    nonce[11] = sequence;
    if (!socket_io(fd, length, sizeof(length), false, deadline)) break;
    const size_t size = (size_t)length[0] << 8 | length[1];
    if (!size || size > LAN_BUNDLE_FRAME_MAX ||
        !socket_io(fd, cipher, size + LAN_TAG_SIZE, false, deadline) ||
        mbedtls_gcm_auth_decrypt(gcm, size, nonce, sizeof(nonce), length, sizeof(length),
          cipher + size, LAN_TAG_SIZE, cipher, plain) ||
        !bundle_wire_trusted(request_generation)) break;
    const bool terminal = plain[0] == 3;
    job.record = plain;
    job.size = size;
    const int64_t work = esp_timer_get_time() + (terminal ? LAN_COMPONENT_WORK_US : LAN_BUNDLE_IDLE_US);
    const uint8_t result = bundle_wire_run(fd, &job, work < end ? work : end);
    // Wipe only the current immutable record after the serialized worker returns.
    mbedtls_platform_zeroize(plain, size);
    nonce[0] = 3;
    length[0] = 0;
    length[1] = 1;
    if (mbedtls_gcm_crypt_and_tag(gcm, MBEDTLS_GCM_ENCRYPT, 1, nonce, sizeof(nonce),
          length, sizeof(length), &result, cipher + 2, LAN_TAG_SIZE, cipher + 3)) break;
    memcpy(cipher, length, sizeof(length));
    if (!socket_io(fd, cipher, 3 + LAN_TAG_SIZE, true, esp_timer_get_time() + LAN_IO_US) ||
        result != 1 || terminal) break;
  }
  if (job.manifest) {
    /* Match the private transaction ID, including after a lost ACK. On a
     * full queue mark the unreachable owner's staging for the next mutation. */
    job.record = NULL;
    if (!bundle_wire_run(fd, &job, 0)) pet_p4_miniapp_lan_bundle_revoke();
    cJSON_Delete(job.manifest);
  }
}
