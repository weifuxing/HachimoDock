"""Production LAN panel adapter + shared receiver, ASAN/UBSAN. No device IO."""
import subprocess

import pytest
from p4_bubble_pixels_test import ROOT, receiver, desire, select, transfer, send, BOOT, CLIENT, OWNER, VERSION, key
from p4_render_trace_test import function


def owned():
    return {**desire(assets=[(4, '中English缺字')]), 'lanRoute': True}


def request(payload, **extra):
    return {'lan': {**payload, 'requestId': 'local-test'}, 'admitted': True,
            'reply': {'ok': True, 'phase': payload['phase'], 'boardDeviceId': 'board',
                      'boot': BOOT, 'requestId': 'local-test'}, **extra}


def test_wifi_need_never_goes_to_serial_and_shared_transfer_draws(receiver):
    packets = transfer('中English缺字')
    receiver([
        request({'phase': 'capabilities'}, reply={'ok': True, 'schema': 'p4-text-alpha-v2', 'boot': BOOT}),
        owned(), select(body='中English缺字'),
        {'need': {'now': 1, 'ok': False}},
        request({'phase': 'need'}, now=1, reply={'ok': True, 'need': {
            'boot': BOOT, 'client': CLIENT, 'owner': OWNER, 'version': VERSION,
            'id': key(4, '中English缺字'), 'text': '中English缺字', 'style': 4, 'page': 0, 'glyphCells': False, 'bodyMasks': False}}),
        request({'phase': 'need'}, now=2, noNeed=True),
        *[request(p, now=20 + i) for i, p in enumerate(packets)],
        select(body='中English缺字', pixels=True),
        {'retireLan': True}, select(body='中English缺字', pixels=False),
    ])


@pytest.mark.parametrize('mask', [1, 2, 4, 8, 16, 32, 64])
def test_each_audio_ota_asset_gate_refuses_need_and_begin(receiver, mask):
    receiver([owned(), select(body='中English缺字'),
        request({'phase': 'need'}, gateMask=mask, reply={'ok': mask == 4}, noNeed=True),
        request(transfer('中English缺字')[0], gateMask=mask, reply={'ok': False, 'error': 'protected_audio_or_update'}),
    ])


def test_main_admission_gate_and_usb_takeover_invalidate_old_wifi_owner(receiver):
    packets = transfer('中English缺字')
    receiver([owned(), request(packets[0], admitted=False,
        reply={'ok': False, 'error': 'protected_audio_or_update'}),
        request(packets[0]),
        desire(assets=[(4, '中English缺字')]),
        request(packets[1], reply={'ok': False}),
        {'retireLan': True},  # Must not retire the new USB owner.
        *send(packets), select(body='中English缺字', pixels=True),
    ])


def test_old_wifi_frame_cannot_follow_pairing_revocation(receiver):
    packets = transfer('中English缺字')
    receiver([owned(), request(packets[0]), {'retireLan': True},
              request(packets[1], reply={'ok': False}), select(body='中English缺字')])


def test_wifi_forwards_negotiated_glyph_cell_need_and_shared_receiver_format(receiver):
    # Transport-only fixture: synthetic cells, no font selection/raster policy.
    d = owned()
    d['desire']['textPixels']['glyphCells'] = True
    chars = sorted(set('中English缺字'), key=ord)
    cells = b''.join(ord(ch).to_bytes(4, 'little') + bytes([0x55] * 36) for ch in chars)
    packets = transfer('中English缺字', data=cells)
    packets[0]['glyphCells'] = True
    receiver([d, select(body='中English缺字'),
        request({'phase': 'need'}, now=1, reply={'ok': True, 'need': {
            'boot': BOOT, 'client': CLIENT, 'owner': OWNER, 'version': VERSION,
            'id': key(4, '中English缺字'), 'text': '中English缺字', 'style': 4, 'page': 0, 'glyphCells': True, 'bodyMasks': False}}),
        *[request(p, now=20 + i) for i, p in enumerate(packets)],
        request(packets[0], now=30, reply={'ok': True, 'cached': True}),
    ])


def test_only_negotiated_body_masks_can_use_a_pixels_only_snapshot(tmp_path):
    source = (ROOT / 'main/pet_p4_lan.c').read_text()
    program = tmp_path / 'snapshot.c'
    program.write_text(r'''
#include <assert.h>
#include <stdbool.h>
#include <string.h>
#include "cJSON.h"
#include "pet_p4_protocol.h"
''' + function(source, 'valid_source') + function(source, 'valid_snapshot') + r'''
static void check(const char *text, bool allowed) {
  cJSON *snapshot = cJSON_Parse(text);
  assert(snapshot && valid_snapshot(snapshot) == allowed);
  cJSON_Delete(snapshot);
}
int main(void) {
  check("{\"source\":\"codex\",\"status\":{}}", true);
  check("{\"source\":\"codex\",\"textPixels\":false}", false);
  check("{\"source\":\"codex\",\"textPixels\":{\"glyphCells\":true}}", false);
  check("{\"source\":\"codex\",\"textPixels\":{\"bodyMasks\":true}}", true);
  check("{\"source\":\"codex\",\"textPixels\":{\"bodyMasks\":true},\"other\":{}}", false);
  check("{\"source\":\"codex\",\"textPixels\":{\"bodyMasks\":true},\"textPixels\":{}}", false);
}
''')
    vendor = ROOT / 'tests/vendor/cjson'
    binary = tmp_path / 'snapshot'
    subprocess.run(['cc', '-std=c11', '-Wno-deprecated-declarations', '-I', str(vendor), '-I', str(ROOT / 'main'),
                    str(program), str(vendor / 'cJSON.c'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
