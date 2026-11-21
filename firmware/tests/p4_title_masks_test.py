"""Negotiated 32px title masks: real receiver/renderer policy, ASan + UBSan."""
import pytest
from p4_bubble_pixels_test import desire, transfer, send, select, solid, key, BOOT, CLIENT, OWNER, VERSION
from p4_font_host_pixels_test import renderer_receiver, body_frame, ROOT
from p4_lan_bubble_test import request
from p4_render_trace_test import function


def manifest(title='标题', body='正文', now=0, lan=False, **kwargs):
    m = desire(assets=[(1, title), (4, body)], **kwargs)
    m['desire']['textPixels'].update(titleMasks=True, bodyMasks=True, glyphCells=True)
    return {**m, 'now': now, 'lanRoute': lan}


def title_frame(title='标题', now=10, lan=False, alpha=8, **kwargs):
    packets = transfer(title, solid(1, alpha), style=1, **kwargs)
    # style 1 uses the existing PIXEL_PAGE frame; no titleMasks frame flag.
    return [request(p, now=now+i) if lan else {**s, 'now': now+i}
            for i, (p, s) in enumerate(zip(packets, send(packets)))]


def shown(title='标题', body='正文', now=0, pending=False, mask=False, body_pending=False,
          body_mask=False, body_exact=False):
    return [select(title, body, now=now, owner='visible', hold=True),
            {'titleState': {'pending': pending, 'mask': mask}},
            {'titleDraw': {'localCalls': int(not (pending or mask))}},
            {'bodyState': {'pending': body_pending, 'mask': body_mask, 'exact': body_exact}},
            {'drawHeld': {'style': 1, 'ok': mask, 'first': (14 << 11) | (29 << 5) | 14}},
            {'release': True}]


@pytest.mark.parametrize('lan', [False, True])
def test_title_first_gray_frame_is_independent_of_missing_and_updating_body(renderer_receiver, lan):
    commands = [{'localFonts': True}, manifest(lan=lan)]
    if lan:
        commands += [request({'phase': 'capabilities'}, reply={'ok': True, 'titleMasks': True})]
    commands += shown(now=1, pending=True, body_pending=True)
    commands += title_frame(now=10, lan=lan)
    commands += shown(now=100, mask=True, body_pending=True)
    commands += [manifest(body='更新正文', now=120, lan=lan)]
    commands += shown(body='更新正文', now=130, mask=True, body_pending=True)
    commands += body_frame(4, '更新正文', 150, lan=lan)
    commands += shown(body='更新正文', now=180, mask=True, body_mask=True, body_exact=True)
    renderer_receiver(commands)


def test_body_arrives_first_without_revealing_a_local_title(renderer_receiver):
    renderer_receiver([{'localFonts': True}, manifest(),
        *body_frame(4, '正文', 10),
        *shown(now=100, pending=True, body_mask=True, body_exact=True),
        *title_frame(now=120),
        *shown(now=150, mask=True, body_mask=True, body_exact=True)])


def test_managed_title_never_flashes_local_during_slow_delivery_and_body_partials(renderer_receiver):
    commands = [{'localFonts': True}, manifest(), *shown(now=100, pending=True, body_pending=True)]
    for i in range(1, 8):
        commands += [manifest(body=str(i), now=100+i*300)]
        commands += shown(body=str(i), now=100+i*300, pending=True, body_pending=True)
    for now in range(2600, 20000, 400):
        commands += shown(body='7', now=now, pending=True)
    commands += title_frame(now=20000)
    commands += shown(body='7', now=20100, mask=True)
    renderer_receiver(commands)


def test_new_title_never_draws_old_title_or_flashes_local_after_delay(renderer_receiver):
    commands = [{'localFonts': True}, manifest(), *title_frame(), *shown(now=100, mask=True, body_pending=True)]
    for i in range(1, 8):
        commands += [manifest(title=str(i), now=100+i*300)]
        commands += shown(title=str(i), now=100+i*300, pending=True, body_pending=True)
    commands += shown(title='7', now=2900, pending=True, body_pending=True)
    commands += title_frame('7', now=3000)
    commands += shown(title='7', now=3100, mask=True, body_pending=True)
    renderer_receiver(commands)


def test_title_mask_cache_survives_manifest_renewal_and_late_capability(renderer_receiver):
    renderer_receiver([{'localFonts': True}, *shown(now=1), manifest(now=5),
        *shown(now=6, pending=True), *title_frame(now=10),
        *shown(now=100, mask=True),
        manifest(now=120, client='c'*32),
        *shown(now=130, mask=True)])


def test_legacy_and_offline_titles_do_not_wait_or_switch_covered_local_font(renderer_receiver):
    legacy = desire(assets=[(1, '标题'), (4, '正文')])
    renderer_receiver([{'localFonts': True}, *shown(now=1), legacy,
        *title_frame(now=10), *shown(now=100)])


@pytest.mark.parametrize('bad', [{'glyphCells': True}, {'bodyMasks': True}])
def test_negotiated_title_rejects_wrong_frame_format(renderer_receiver, bad):
    packet = {**transfer('标题', solid(1), style=1)[0], **bad}
    renderer_receiver([manifest(), {'payload': packet, 'ok': False}])


def test_title_owner_revocation_and_audio_gates_keep_existing_safety(renderer_receiver):
    packets = transfer('标题', solid(1), style=1)
    renderer_receiver([manifest(), *send(packets[:1]),
        {'maintain': {'now': 1, 'pause': True}}, {'payload': packets[1], 'ok': False},
        {'maintain': {'now': 2}}, manifest(version='0'*15+'2', now=3),
        {'payload': packets[0], 'ok': False},
        {'payload': {**packets[0], 'boot': 'f'*16}, 'ok': False}])


def test_title_need_reuses_page_protocol_and_only_current_owners(renderer_receiver):
    m = manifest(); m['desire']['textPixels']['bodyMasks'] = False
    renderer_receiver([{'localFonts': True}, m,
        select('标题', '', now=1, hold=True, owner='visible'),
        {'titleState': {'pending': True}}, {'release': True},
        {'need': {'now': 2, 'ok': True, 'style': 1, 'page': 0, 'bodyMasks': False, 'id': key(1, '标题')}},
        select('标题', '', now=400, owner='visible'),
        {'desire': {'textPixels': {**m['desire']['textPixels'], 'owners': []}}},
        {'need': {'now': 800, 'ok': False}}])


def test_renderer_suppresses_local_title_in_both_card_paths_and_advertises_usb_capability():
    renderer = (ROOT/'main/pet_p4_renderer.c').read_text()
    for name in ('draw_bubble', 'draw_session_queue_uncached'):
        assert 'draw_bubble_card_title(&text' in function(renderer, name)
    assert 'if (view->title_pending) return;' in function(renderer, 'draw_bubble_card_title')
    assert '"bubbleTitleMasks", true' in (ROOT/'main/pet_p4_protocol.c').read_text()


def test_dropped_title_chunks_do_not_reveal_local_title_before_fresh_complete_transfer(renderer_receiver):
    packets = transfer('标题', solid(1, 8), style=1)
    renderer_receiver([{'localFonts': True}, manifest(), *shown(now=10, pending=True, body_pending=True),
        *[{**p, 'now': 20+i} for i, p in enumerate(send(packets[:2]))],
        {'maintain': {'now': 4000}}, {'payload': packets[2], 'ok': False, 'now': 4001},
        *shown(now=4002, pending=True), *shown(now=8000, pending=True),
        *title_frame(now=8100, transfer='c'*32), *shown(now=8200, mask=True)])


def test_unmanaged_or_offline_title_is_local_even_if_the_same_mask_is_cached(renderer_receiver):
    empty = manifest(now=110); empty['desire']['textPixels']['owners'] = []
    renderer_receiver([{'localFonts': True}, manifest(), *title_frame(),
        *shown(now=100, mask=True, body_pending=True), empty, *shown(now=120),
        manifest(now=130), *shown(now=140, mask=True),
        {'desire': {}}, *shown(now=150)])


def test_conversation_hold_allows_managed_title_need_and_frame_but_not_legacy_page(renderer_receiver):
    m = manifest(body='', lan=True)
    packets = transfer('标题', solid(1, 8), style=1)
    renderer_receiver([{'localFonts': True}, m,
        {'maintain': {'now': 1, 'conversation': True}},
        select('标题', '', now=2, owner='visible'),
        request({'phase': 'need'}, now=3, gateMask=4, reply={'ok': True, 'need': {
            'boot': BOOT, 'client': CLIENT, 'owner': OWNER, 'version': VERSION,
            'id': key(1, '标题'), 'text': '标题', 'style': 1, 'page': 0,
            'glyphCells': False, 'bodyMasks': False}}),
        *[request(p, now=20+i, gateMask=4) for i,p in enumerate(packets)],
        *shown(body='', now=100, mask=True)])
    legacy = desire(assets=[(1, '标题')]); legacy['lanRoute'] = True
    renderer_receiver([legacy, request(packets[0], gateMask=4,
        reply={'ok': False, 'error': 'protected_audio_or_update'})])


@pytest.mark.parametrize('gate', [1, 2, 8, 16, 32, 64])
def test_title_masks_keep_all_capture_playback_ota_and_asset_protection(renderer_receiver, gate):
    renderer_receiver([manifest(lan=True),
        request(transfer('标题', solid(1), style=1)[0], gateMask=gate,
            reply={'ok': False, 'error': 'protected_audio_or_update'})])


def test_title_only_negotiation_does_not_block_legacy_body_page(renderer_receiver):
    m = manifest(); m['desire']['textPixels'].update(bodyMasks=False, glyphCells=False)
    renderer_receiver([m, *send(transfer('正文')),
        select('标题', '正文', now=100, owner='visible', pixels=True, hold=True),
        {'titleState': {'pending': True}}, {'titleDraw': {'localCalls': 0}},
        {'bodyDraw': {'localCalls': 0}}, {'release': True}])
