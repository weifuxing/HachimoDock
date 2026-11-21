"""Actual renderer pin policy + production receiver, cJSON and pthreads under ASan/UBSan."""
from pathlib import Path
import pytest
import p4_bubble_pixels_test as receiver_tests
from p4_bubble_pixels_test import desire, transfer, send, select, solid
from p4_render_trace_test import function
from p4_lan_bubble_test import request
ROOT=Path(__file__).resolve().parents[1]

@pytest.fixture(scope="module")
def renderer_receiver(tmp_path_factory):
    source=(ROOT/'main/pet_p4_renderer.c').read_text()
    helpers=r'''
#define LV_FONT_GLYPH_FORMAT_A1 1
#define LV_FONT_GLYPH_FORMAT_A2 2
#define LV_FONT_GLYPH_FORMAT_A4 4
#define LV_FONT_GLYPH_FORMAT_A8 8
typedef struct { bool is_placeholder; int box_w,box_h,stride,format; } lv_font_glyph_dsc_t;
static int pet_p4_font_cn_16;
static bool local_fonts;
static bool lv_font_get_glyph_dsc(const int *font, lv_font_glyph_dsc_t *glyph, uint32_t cp, uint32_t next) {
 (void)font;(void)next;
 glyph->is_placeholder=cp==0x3400 || cp==0x1f9ea;
 return local_fonts;
}
static bool scaled_glyph_dsc(uint32_t cp,uint32_t next,lv_font_glyph_dsc_t *glyph,int *scale) {
 (void)scale;return lv_font_get_glyph_dsc(&pet_p4_font_cn_16,glyph,cp,next);
}
static struct {uint32_t bubble_select_us,pinned_blit_us;} g_render_trace;
static int64_t esp_timer_get_time(void){return 0;}
static uint32_t elapsed_us_clamped(int64_t a,int64_t b){return (uint32_t)(b-a);}
static void pet_p4_render_trace_add(uint32_t*a,uint32_t b){*a+=b;}
static unsigned local_calls;
static const char *draw_text_line(const char*s,int x,int y,int w,uint16_t c,int scale,bool e){
 (void)s;(void)x;(void)y;(void)w;(void)c;(void)scale;(void)e;local_calls++;return "";
}
static uint16_t *g_framebuffer=framebuffer+1;
static const pet_bubble_text_view_t *g_bubble_glyph_view;
static unsigned g_bubble_glyph_style;
static void fill_rect(int x,int y,int w,int h,uint16_t c){
 for(int dy=0;dy<h;dy++)for(int dx=0;dx<w;dx++)g_framebuffer[(y+dy)*640+x+dx]=c;
}
static void blend_px(int x,int y,uint8_t r,uint8_t g,uint8_t b,uint8_t a){
 (void)r;(void)g;(void)b;assert(a);fill_rect(x,y,1,1,0);
}
static bool draw_bubble_cell(uint32_t cp,int x,int y,uint16_t color,bool medium);
static const char *draw_text_line_medium(const char*s,int x,int y,int w,uint16_t c,bool e){
 (void)w;(void)e;local_calls++;
 if(g_bubble_glyph_view&&g_bubble_glyph_view->glyph_cells){
  fill_rect(x+2,y+4,3,3,0x8080); /* native prefix */
  if(strstr(s,"㐀")&&!draw_bubble_cell(0x3400,x+80,y,c,true))fill_rect(x+90,y+10,3,3,0);
 }
 return "";
}
static const char *draw_text_line_medium_vcenter(const char*s,int x,int y,int h,int w,uint16_t c,bool e){
 (void)s;(void)x;(void)y;(void)h;(void)w;(void)c;(void)e;local_calls++;return "";
}
'''
    helpers+=''.join(function(source,n) for n in ('utf8_next','glyph_alpha_at','medium_glyph_coverage','draw_bubble_cell',
        'bubble_glyph_coverage','renderer_bubble_select','renderer_bubble_draw_pinned',
        'draw_bubble_large_line','draw_bubble_card_title','draw_card_body_lines'))
    # Reuse the receiver's real cJSON/pthread/ASan harness. Only its main TU is
    # extended; the production receiver is compiled unchanged as a separate TU.
    original=Path.write_text
    def write(path,data,*args,**kwargs):
        if path.name=='main.c' and 'race_writer' in data:
            data=data.replace('pet_bubble_pixels_select(', 'renderer_bubble_select(')
            data=data.replace('&view,NULL);','&view);').replace('&held,NULL);','&held);')
            data=data.replace('pet_bubble_pixels_draw_pinned(', 'renderer_bubble_draw_pinned(')
            data=data.replace('static void *race_writer',helpers+'\nstatic void *race_writer',1)
            data=data.replace('  unsigned route=num(c,"route");',
                '  if(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(c,"localFonts")))local_fonts=true;\n  unsigned route=num(c,"route");')
            data=data.replace('bool ok=renderer_bubble_draw_pinned(&held,','bool aggregate=held.pixels;int title_pin=held.title_pin,body_pin=held.body_pin;bool ok=renderer_bubble_draw_pinned(&held,')
            data=data.replace('assert(ok==cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(v,"ok")));assert(framebuffer',
                'assert(held.pixels==aggregate&&held.title_pin==title_pin&&held.body_pin==body_pin);assert(ok==cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(v,"ok")));assert(framebuffer')
            marker='  if(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(c,"release")))'
            extra=r'''
  v=cJSON_GetObjectItemCaseSensitive(c,"bodyDraw");if(v){
   assert(holding);local_calls=0;
   draw_card_body_lines(held.body,0,0,496,0,&held);
   assert(local_calls==num(v,"localCalls"));
  }
  v=cJSON_GetObjectItemCaseSensitive(c,"titleDraw");if(v){
   assert(holding);local_calls=0;
   draw_bubble_card_title(&held,0,0,474,0);
   assert(local_calls==num(v,"localCalls"));
  }
  v=cJSON_GetObjectItemCaseSensitive(c,"localGlyphDraw");if(v){
   assert(holding&&held.glyph_cells&&!held.pixels);
   memset(g_framebuffer,0xff,640*480*sizeof(*g_framebuffer));
   draw_card_body_lines(held.body,0,0,496,0,&held);
   assert(g_framebuffer[4*640+2]==0x8080);
   if(!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(v,"reuse"))){
    assert(g_framebuffer[80]==(num(v,"available")?0:0xffff));
    assert(g_framebuffer[10*640+90]==(num(v,"available")?0xffff:0));
   } else assert(g_framebuffer[80]==0);
   g_bubble_glyph_view=&held;g_bubble_glyph_style=PET_BUBBLE_TITLE;
   assert(draw_bubble_cell(0x3400,10,40,0,false)==(num(v,"available")!=0));
   if(num(v,"available"))assert(g_framebuffer[40*640+10]==0);
   g_bubble_glyph_view=NULL;
  }
'''
            assert marker in data;data=data.replace(marker,extra+marker)
        return original(path,data,*args,**kwargs)
    with pytest.MonkeyPatch.context() as monkey:
        monkey.setattr(Path,'write_text',write)
        return receiver_tests.receiver.__wrapped__(tmp_path_factory)


def body_manifest(style, text, now=0, *, lan=False, version=receiver_tests.VERSION, extras=()):
    message = desire(assets=[(style, text), *extras], version=version)
    message['desire']['textPixels'].update(bodyMasks=True, glyphCells=True)
    return {**message, 'now': now, 'lanRoute': lan}


def body_frame(style, text, now, *, lan=False, alpha=15, page=0, pages=1):
    packets = transfer(text, solid(style, alpha), style=style, page=page, pages=pages)
    packets[0]['bodyMasks'] = True
    if lan:
        return [request(p, now=now + i, gateMask=4) for i, p in enumerate(packets)]
    return [{**p, 'now': now + i} for i, p in enumerate(send(packets))]


@pytest.mark.parametrize('style', [2, 4])
@pytest.mark.parametrize('lan', [False, True])
def test_medium_body_waits_for_gray_and_keeps_previous_during_updates(renderer_receiver, style, lan):
    original, updated = '中文字体任务完成', '中文字体任务完成。㐀'

    def shown(text, now, *, pending=False, exact=False, mask=False, shade=None):
        steps = [
            select('你', text, now=now, style=style, hold=True, owner='live'),
            {'bodyState': {'pending': pending, 'mask': mask, 'exact': exact}},
            {'drawHeld': {'style': style, 'ok': mask, 'first': shade}},
        ]
        if style == 4:
            steps.append({'bodyDraw': {'localCalls': int(not (pending or mask))}})
        return steps + [{'release': True}]

    commands = [{'localFonts': True}, body_manifest(style, original, 100, lan=lan)]
    if lan:
        commands.append(request({'phase': 'capabilities'}, reply={'bodyMasks': True}))
    commands += shown(original, 100, pending=True)
    if lan:
        commands.append(request({'phase': 'need'}, now=101, gateMask=4,
                                reply={'ok': True, 'need': {
                                    'boot': receiver_tests.BOOT, 'client': receiver_tests.CLIENT,
                                    'owner': receiver_tests.OWNER, 'version': receiver_tests.VERSION,
                                    'id': receiver_tests.key(style, original), 'text': original,
                                    'style': style, 'page': 0, 'bodyMasks': True, 'glyphCells': False,
                                }}))
    else:
        commands += [
            {'maintain': {'now': 101, 'conversation': True}},
            {'need': {'now': 101, 'style': style, 'ok': True, 'bodyMasks': True}},
        ]
    commands += body_frame(style, original, 200, lan=lan, alpha=8)
    # 4-bit coverage reaches the RGB565 blend without re-rasterizing resident glyphs.
    blended = (14 << 11) | (29 << 5) | 14
    commands += shown(original, 220, mask=True, exact=True, shade=blended)
    commands.append(body_manifest(style, updated, 240, lan=lan))
    commands += shown(updated, 250, mask=True, shade=blended)
    commands += body_frame(style, updated, 800, lan=lan)
    commands += shown(updated, 820, mask=True, exact=True, shade=0)
    if lan:
        commands += [{'retireLan': True}]
    else:
        commands += [{'desire': {'textPixels': False}}]
    commands += shown(updated, 850, mask=True, exact=True, shade=0)
    commands += shown('新内容', 900)
    renderer_receiver(commands)


def test_body_paging_keeps_selected_mask_under_eviction_pressure(renderer_receiver):
    style, text = 2, '中文字体任务完成'
    fillers = [f'prefetch-{i}' for i in range(8)]
    commands = [{'localFonts': True}, body_manifest(style, text, extras=[(4, t) for t in fillers])]
    commands += body_frame(style, text, 10, pages=3)
    commands += [
        select('你', text, now=30, style=style, hold=True, owner='live'),
        {'drawHeld': {'style': style, 'page': 1, 'pinPage': True, 'ok': False}},
        {'bodyState': {'mask': True, 'exact': True, 'page': 0}},
        {'drawHeld': {'style': style, 'page': 0, 'ok': True, 'first': 0}},
        {'release': True},
    ]
    commands += body_frame(style, text, 40, alpha=8, page=1, pages=3)
    commands += body_frame(style, text, 50, page=2, pages=3)
    gray = (14 << 11) | (29 << 5) | 14
    commands += [
        select('你', text, now=100, style=style, hold=True, owner='live'),
        {'drawHeld': {'style': style, 'page': 1, 'pinPage': True, 'ok': True, 'first': gray}},
        {'bodyState': {'mask': True, 'exact': True, 'page': 1}},
        {'release': True},
    ]
    # The incoming phase timestamps were sampled before the most recent draw.
    # Pressure must evict page 2 while page 1 still has a visible lease.
    for i, filler in enumerate(fillers):
        commands += body_frame(4, filler, 60 + i * 8)
    commands += [
        select('你', text, now=200, style=style, hold=True, owner='live'),
        {'drawHeld': {'style': style, 'page': 2, 'pinPage': True, 'ok': False}},
        {'bodyState': {'mask': True, 'exact': True, 'page': 1}},
        {'drawHeld': {'style': style, 'page': 1, 'ok': True, 'first': gray}},
        {'release': True},
    ]
    commands += body_frame(style, text, 210, page=2, pages=3)
    commands += [
        select('你', text, now=230, style=style, hold=True, owner='live'),
        {'drawHeld': {'style': style, 'page': 2, 'pinPage': True, 'ok': True, 'first': 0}},
        {'bodyState': {'mask': True, 'exact': True, 'page': 2}},
        {'release': True},
        {'desire': {'textPixels': False}},
        select('你', text, now=250, style=style, hold=True, owner='live'),
        {'bodyState': {'mask': True, 'exact': True, 'page': 2}},
        {'drawHeld': {'style': style, 'page': 2, 'ok': True, 'first': 0}},
        {'release': True},
    ]
    renderer_receiver(commands)


@pytest.mark.parametrize('initial', [False, True])
def test_partial_pressure_and_audio_gate_cannot_extend_the_body_deadline(renderer_receiver, initial):
    style, original, start = 4, '中文字体任务完成', 100
    commands = [{'localFonts': True}]
    if initial:
        commands += [body_manifest(style, original)]
        commands += body_frame(style, original, 10)
        commands += [select('Agent', original, now=40, owner='live')]
    # Repeated stopped or superseded owners occupy no need slots permanently.
    # Exercise 100 distinct versions in one continuously visible owner.
    deadline = 3200 if initial else 2500
    for i in range(100):
        now, text = start + i * 40, f'{original}{i}'
        version = f'{i + 2:016x}'
        commands.extend([
            body_manifest(style, text, now, version=version),
            {'maintain': {'now': now, 'pause': i % 3 == 0, 'conversation': True}},
            select('Agent', text, now=now, hold=True, owner='live'),
            {'bodyState': {'mask': initial and now - start < deadline,
                           'pending': not initial and now - start < deadline}},
            {'bodyDraw': {'localCalls': int(now - start >= deadline)}},
            {'release': True},
        ])
    commands += [
        {'maintain': {'now': 4300}},
        select('Agent', text, now=4300, owner='live'),
        {'need': {'now': 4301, 'style': style, 'ok': True, 'bodyMasks': True,
                  'id': receiver_tests.key(style, text)}},
    ]
    late = body_frame(style, text, 4400)
    for frame in late:
        frame['payload']['version'] = version
    commands += late
    commands += [
        select('Agent', text, now=4500, hold=True, owner='live'),
        {'bodyState': {'mask': True, 'exact': True}}, {'bodyDraw': {'localCalls': 0}}, {'release': True},
        select('Agent', text, now=4600, hold=True, owner='live'),
        {'bodyState': {'mask': True, 'exact': True}}, {'bodyDraw': {'localCalls': 0}}, {'release': True},
        body_manifest(style, text + '新', 4700),
        select('Agent', text + '新', now=4710, hold=True, owner='live'),
        {'bodyState': {'mask': True}}, {'drawHeld': {'style': style, 'ok': True, 'first': 0}},
        {'release': True},
    ]
    renderer_receiver(commands)


@pytest.mark.parametrize('boundary', ['owner', 'title', 'peer', 'lease'])
def test_previous_body_mask_is_bounded_to_its_display_scope(renderer_receiver, boundary):
    style = 4
    first, second = '开始', '下一段'
    now = 5300 if boundary == 'lease' else 300
    lan = boundary == 'peer'
    owner = 'other' if boundary == 'owner' else 'live'
    title = '另一标题' if boundary == 'title' else 'Agent'
    renderer_receiver([
        {'localFonts': True}, body_manifest(style, first, 10),
        *body_frame(style, first, 20),
        select('Agent', first, now=40, hold=True, owner='live'),
        {'bodyState': {'mask': True, 'exact': True}}, {'release': True},
        body_manifest(style, second, now, lan=lan),
        select(title, second, now=now, hold=True, owner=owner),
        {'bodyState': {'pending': True}}, {'bodyDraw': {'localCalls': 0}}, {'release': True},
        select(title, second, now=now+10, hold=True, owner=owner),
        {'bodyState': {'pending': True}}, {'drawHeld': {'style': style, 'ok': False}},
        {'release': True},
        *body_frame(style, second, now+20, lan=lan),
        select(title, second, now=now+40, hold=True, owner=owner),
        {'bodyState': {'mask': True, 'exact': True}}, {'release': True},
    ])


@pytest.mark.parametrize('lan', [False, True])
def test_new_update_after_fallback_can_wait_without_inheriting_the_previous_key(renderer_receiver, lan):
    first, second = '开始', '下一段'
    renderer_receiver([
        {'localFonts': True}, body_manifest(4, first, 100, lan=lan),
        select('Agent', first, now=100, hold=True, owner='live'),
        {'bodyState': {'pending': True}}, {'release': True},
        select('Agent', first, now=2600, hold=True, owner='live'),
        {'bodyState': {}}, {'bodyDraw': {'localCalls': 1}}, {'release': True},
        body_manifest(4, second, 2700, lan=lan),
        select('Agent', second, now=2700, hold=True, owner='live'),
        {'bodyState': {'pending': True}}, {'bodyDraw': {'localCalls': 0}}, {'release': True},
        select('Agent', second, now=5200, hold=True, owner='live'),
        {'bodyState': {}}, {'bodyDraw': {'localCalls': 1}}, {'release': True},
        *body_frame(4, second, 5250, lan=lan),
        select('Agent', second, now=5280, hold=True, owner='live'),
        {'bodyState': {'mask': True, 'exact': True}}, {'bodyDraw': {'localCalls': 0}}, {'release': True},
        select('Agent', second, now=5380, hold=True, owner='live'),
        {'bodyState': {'mask': True, 'exact': True}}, {'bodyDraw': {'localCalls': 0}}, {'release': True},
    ])


def test_body_mask_admission_disallows_unlisted_legacy_and_protected_transfers(renderer_receiver):
    style, text = 4, '中文字体任务完成'
    legacy = transfer(text)[0]
    marked = {**legacy, 'bodyMasks': True}
    renderer_receiver([
        {'localFonts': True}, body_manifest(style, text, lan=True),
        select('Agent', '未列出的文本', now=1, hold=True, owner='unlisted'),
        {'bodyState': {}}, {'bodyDraw': {'localCalls': 1}}, {'release': True},
        request(legacy, now=2, reply={'ok': False, 'error': 'invalid_metadata'}),
        request(marked, now=3, gateMask=64 | 4,
                reply={'ok': False, 'error': 'protected_audio_or_update'}),
        request(marked, now=4, gateMask=4, reply={'ok': True}),
        request(transfer(text)[1], now=5, gateMask=1 | 4,
                reply={'ok': False, 'error': 'protected_audio_or_update'}),
    ])


def test_lan_caption_can_arrive_before_its_manifest(renderer_receiver):
    style, caption = 2, '中文字体任务完成'
    renderer_receiver([
        {'localFonts': True}, body_manifest(style, '准备中', lan=True),
        select('你', caption, now=100, style=style, hold=True, owner='live'),
        {'bodyState': {'pending': True}}, {'release': True},
        request({'phase': 'need'}, now=110, gateMask=4, noNeed=True),
        body_manifest(style, caption, now=120, lan=True),
        *body_frame(style, caption, 130, lan=True),
        select('你', caption, now=160, style=style, hold=True, owner='live'),
        {'bodyState': {'mask': True, 'exact': True}},
        {'drawHeld': {'style': style, 'ok': True, 'first': 0}},
        {'release': True},
    ])

def test_title_arrival_alone_keeps_entire_view_local(renderer_receiver):
    renderer_receiver([desire()]+send(transfer('title',style=1))+[
        select(title='title',body='body',pixels=False,hold=True),
        {'drawHeld':{'style':1,'ok':False}},
        {'bodyDraw':{'localCalls':1}},
        {'drawHeld':{'style':4,'ok':False}},
        {'release':True}])

def test_body_arrival_alone_keeps_entire_view_local(renderer_receiver):
    renderer_receiver([desire()]+send(transfer('body'))+[
        select(title='title',body='body',pixels=False,hold=True),
        {'bodyDraw':{'localCalls':1}},
        {'drawHeld':{'style':4,'ok':False}},
        {'drawHeld':{'style':1,'ok':False}}, {'release':True}])

def test_pinned_title_and_body_keep_same_snapshot_after_peer_retirement(renderer_receiver):
    renderer_receiver([desire()]+send(transfer('title',style=1))+send(transfer('body'))+[
        select(title='title',body='body',pixels=True,hold=True),
        desire(client='c'*32),
        {'drawHeld':{'style':1,'ok':True,'first':0}},
        {'bodyDraw':{'localCalls':0}},
        {'drawHeld':{'style':4,'ok':True,'first':0}}, {'release':True},
        select(title='new title',body='new body',pixels=False,hold=True),
        {'drawHeld':{'style':1,'ok':False}}, {'bodyDraw':{'localCalls':1}}, {'release':True}])

def test_caption_page_miss_does_not_flicker_already_pinned_host_title(renderer_receiver):
    renderer_receiver([desire()]+send(transfer('title',style=1))+
        send(transfer('caption',style=2,pages=3))+[
        select(title='title',body='caption',style=2,pixels=True,hold=True),
        {'drawHeld':{'style':1,'ok':True,'first':0}},
        {'drawHeld':{'style':2,'page':1,'pinPage':True,'ok':False}}, {'release':True}]+
        send(transfer('caption',style=2,page=1,pages=3))+[
        select(title='title',body='caption',style=2,pixels=True,hold=True),
        {'drawHeld':{'style':2,'page':1,'pinPage':True,'ok':True,'first':0}},
        {'drawHeld':{'style':1,'ok':True,'first':0}}, {'release':True}])

def test_actual_host_policy_concurrent_retirement(renderer_receiver):
    receiver_tests.test_real_mutex_concurrent_draw_and_peer_retirement_keep_pins_safe(renderer_receiver)

def test_supported_text_stays_local_with_first_late_and_incremental_usb_bitmaps(renderer_receiver):
    title, body, expanded = '标题', '字重一致 A1', '字重一致 A1，追加内容 B2'
    def rendered(text, now):
        return [select(title, text, hold=True, now=now, owner='same-view'),
                {'drawHeld': {'style': 1, 'ok': False}}, {'bodyDraw': {'localCalls': 1}},
                {'drawHeld': {'style': 4, 'ok': False}}, {'release': True}]

    renderer_receiver([{'localFonts': True}] + rendered(body, 10)  # Wi-Fi, no bitmap peer
        + [desire(assets=[(1, title), (4, body), (4, expanded)])]
        + rendered(body, 20) + [{'need': {'now': 21, 'ok': False}}]
        + send(transfer(title, style=1)) + send(transfer(body))
        + rendered(body, 30) + rendered(expanded, 31)
        + send(transfer(expanded)) + rendered(expanded, 40)
        + [desire(client='c'*32)] + rendered(expanded, 50))


def test_missing_body_glyph_can_use_bitmap_without_switching_covered_title(renderer_receiver):
    title, body = '标题', '🧪'
    renderer_receiver([{'localFonts': True}, desire(assets=[(1, title), (4, body)]),
        select(title, body, hold=True, owner='mixed'), {'bodyDraw': {'localCalls': 1}}, {'release': True},
        {'need': {'now': 1, 'ok': True, 'style': 4, 'page': 0}}]
        + send(transfer(body)) + [select(title, body, pixels=True, hold=True, owner='mixed'),
        {'drawHeld': {'style': 1, 'ok': False}}, {'bodyDraw': {'localCalls': 0}},
        {'release': True}, desire(client='c'*32), select(title, body, hold=True, owner='mixed'),
        {'bodyDraw': {'localCalls': 1}}, {'release': True}])


@pytest.mark.parametrize('style', [2, 4])
def test_legacy_bitmap_cannot_replace_resident_chinese_prefix(renderer_receiver, style):
    title, plain = '字重测试', 'ABCD 字重测试 A1'
    # U+3400 is in the desktop MiSans cmap and outside the resident P4 cmap.
    expanded = plain + ' 㐀'
    owner = 'streaming-body'
    renderer_receiver([{'localFonts': True}, desire(assets=[(style, plain), (style, expanded)]),
        select(title, plain, style=style, now=100, owner=owner),
        select(title, expanded, style=style, now=150, owner=owner),
        {'need': {'now': 151, 'ok': False}}]
        + [{**packet, 'now': 160} for packet in send(transfer(expanded, style=style))]
        + [select(title, expanded, style=style, hold=True, now=200, owner=owner),
           {'drawHeld': {'style': 1, 'ok': False}},
           {'drawHeld': {'style': style, 'ok': False}}, {'release': True}])


@pytest.mark.parametrize('style,lan', [(4, False), (2, True)])
def test_glyph_cells_paint_only_the_missing_glyph_and_survive_append(renderer_receiver, style, lan):
    title, body = 'MiSans 标题', 'ABCD 㐀'
    expanded = body + ' B2'
    meta = desire(assets=[(style, body), (style, expanded)])
    meta['desire']['textPixels']['glyphCells'] = True
    meta['lanRoute'] = lan
    cells = b'\x00\x34\x00\x00\x80' + bytes(35)
    commands = transfer(body, data=cells, style=style)
    commands[0]['glyphCells'] = True
    upload = [{**e, 'lanRoute': lan, 'now': 200} for e in send(commands)]
    owner = 'streaming-cells'
    renderer_receiver([{'localFonts': True}, meta,
        select(title, body, style=style, hold=True, now=100, owner=owner),
        {'localGlyphDraw': {'available': 0}}, {'release': True}]
        + upload + [select(title, body, style=style, hold=True, now=300, owner=owner),
        {'drawHeld': {'style': style, 'page': 2, 'pinPage': True, 'ok': False}},
        {'localGlyphDraw': {'available': 1}}, {'release': True},
        select(title, expanded, style=style, hold=True, now=400, owner=owner),
        {'localGlyphDraw': {'available': 1, 'reuse': True}}, {'release': True},
        desire(client='c'*32),
        select(title, expanded, style=style, hold=True, now=500, owner=owner),
        {'bodyDraw': {'localCalls': 1}}, {'release': True}])


def test_duplicate_scalar_cells_never_commit(renderer_receiver):
    body = '㐀'
    meta = desire(assets=[(4, body)])
    meta['desire']['textPixels']['glyphCells'] = True
    cells = (b'\x00\x34\x00\x00\x80' + bytes(35)) * 2
    commands = transfer(body, data=cells)
    commands[0]['glyphCells'] = True
    packets = send(commands)
    packets[-1]['ok'] = False
    renderer_receiver([meta] + packets + [select(body=body, hold=True),
        {'localGlyphDraw': {'available': 0}}, {'release': True}])


def test_caption_title_draws_before_page_selection():
    text=(ROOT/'main/pet_p4_renderer.c').read_text()
    bubble=function(text,'draw_bubble')
    assert bubble.index('draw_bubble_card_title(&text') < bubble.index('pet_bubble_pixels_pin_page(&text,host_page)')
    assert 'realtime && text.body_bitmap && !text.glyph_cells' in bubble
    assert 'if (realtime && host_pages && text.pixels)' in bubble
    assert 'pet_bubble_pixels_revision()' in function(text,'session_overlay_signature')
    assert 'NULL /* Approval text is local-only' in function(text,'draw_approval_overlay')

def test_session_cache_signature_tracks_pixel_arrival_without_text_change(tmp_path):
    import subprocess
    text=(ROOT/'main/pet_p4_renderer.c').read_text()
    code=r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#define PET_P4_SESSION_QUEUE_MAX 4
typedef struct {char id[16],title[16],content[16],state[16];} pet_p4_session_queue_item_t;
typedef struct {unsigned session_queue_count,current_session_index;bool session_voice_active;char current_session_agent[64];pet_p4_session_queue_item_t session_queue[4];} pet_p4_runtime_state_t;
static uint32_t revision;
static uint32_t pet_bubble_pixels_revision(void){return revision;}
'''
    code+=''.join(function(text,n) for n in ('session_overlay_visible_window','session_cache_hash_bytes','session_cache_hash_text','session_overlay_signature'))
    code+=r'''
int main(void){
 pet_p4_runtime_state_t state={.session_queue_count=1};
 strcpy(state.session_queue[0].id,"session");strcpy(state.session_queue[0].title,"same title");
 uint64_t old=session_overlay_signature(&state);
 assert(session_overlay_signature(&state)==old);
 revision++;uint64_t arrived=session_overlay_signature(&state);
 assert(arrived!=old&&session_overlay_signature(&state)==arrived);
 strcpy(state.current_session_agent,"other");
 assert(session_overlay_signature(&state)!=arrived);
 return 0;
}
'''
    path=tmp_path/'cache.c';path.write_text(code);binary=tmp_path/'cache'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(path),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
