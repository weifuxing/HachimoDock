"""Exercise the protocol's pixel replies with real cJSON allocations."""
import subprocess
from pathlib import Path


def test_pixel_chunks_release_reply_objects(tmp_path):
    firmware = Path(__file__).resolve().parents[1]
    protocol = (firmware / "main/pet_p4_protocol.c").read_text()
    vendor = firmware / "tests/vendor/cjson"
    sender = protocol[protocol.index("static void send_topic_payload("):]
    sender = sender[:sender.index("\n}") + 2]
    start = protocol.index('} else if (strcmp(topic, "ui/text-pixels") == 0) {')
    end = protocol.index('} else if (strcmp(topic, "speech/text") == 0) {', start)
    branch = protocol[start + 1:end + 1].replace("else if", "if", 1)
    source = tmp_path / "pixel-reply.c"
    source.write_text(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
typedef void (*pet_p4_send_line_fn)(const char *, void *);
static unsigned live, calls, acks;
static void *tracked_alloc(size_t size) {
    void *p = malloc(size);
    assert(p);
    ++live;
    return p;
}
static void tracked_free(void *p) {
    if (!p) return;
    assert(live);
    --live;
    free(p);
}
static const char *json_string(const cJSON *p, const char *key) {
    const char *s = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(p, key));
    return s ? s : "";
}
static uint64_t esp_timer_get_time(void) { return 0; }
static bool pet_p4_audio_active(void) { return false; }
static bool pet_p4_audio_playback_active(void) { return false; }
static bool pet_p4_audio_stream_playing(void) { return false; }
static bool pet_p4_audio_conversation_active(void) { return false; }
static bool pet_p4_audio_music_active(void) { return false; }
static bool pet_p4_ota_active(void) { return false; }
static void pet_bubble_pixels_maintain(uint64_t now, bool paused, bool conversation) {
    assert(!now && !paused && !conversation);
}
static bool pet_bubble_pixels_handle_owned(const cJSON *p, void *ctx, uint64_t now, cJSON *reply) {
    (void)ctx;
    assert(!now);
    ++calls;
    const bool ok = strcmp(json_string(p, "requestId"), "rejected") != 0;
    cJSON_AddBoolToObject(reply, "ok", ok);
    if (!ok) cJSON_AddStringToObject(reply, "error", "invalid_chunk");
    return ok;
}
static void send_ack(const char *line, void *ctx) {
    cJSON *frame = cJSON_Parse(line);
    const cJSON *reply = cJSON_GetObjectItemCaseSensitive(frame, "payload");
    assert(!strcmp(json_string(frame, "topic"), "ui/text-pixels/status"));
    assert(!strcmp(json_string(reply, "phase"), ctx));
    assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(reply, "ok"))
           == (strcmp(json_string(reply, "requestId"), "rejected") != 0));
    ++acks;
    cJSON_Delete(frame);
}
''' + sender + r'''
static void receive(const char *phase, const char *request_id) {
    const char *topic = "ui/text-pixels";
    const struct { bool asset_transfer_active; } idle = {0}, *state = &idle;
    pet_p4_send_line_fn send_line = send_ack;
    void *ctx = (void *)phase;
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "phase", phase);
    cJSON_AddStringToObject(payload, "requestId", request_id);
''' + branch + r'''
    cJSON_Delete(payload);
}
int main(void) {
    cJSON_Hooks hooks = {tracked_alloc, tracked_free};
    cJSON_InitHooks(&hooks);
    receive("begin", "accepted");
    for (unsigned i = 0; i < 1000; ++i) {
        receive("chunk", i % 2 ? "accepted" : "rejected");
    }
    receive("end", "accepted");
    assert(calls == 1002 && acks == 1002);
    if (live) fprintf(stderr, "%u live JSON allocations after the transfer\n", live);
    return live != 0;
}
''')
    binary = tmp_path / "pixel-reply"
    subprocess.run(
        ["cc", "-std=c11", "-Wno-deprecated-declarations", "-I", str(vendor),
         str(source), str(vendor / "cJSON.c"), "-lm", "-o", str(binary)],
        check=True,
    )
    subprocess.run([str(binary)], check=True)
