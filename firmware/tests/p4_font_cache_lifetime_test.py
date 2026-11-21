"""Real receiver plus renderer cache-hit scheduling; ASan/UBSan, no hardware."""
from pathlib import Path
import pytest
import p4_bubble_pixels_test as receiver_tests
from p4_bubble_pixels_test import desire, transfer, send, select
from p4_render_trace_test import function

ROOT = Path(__file__).resolve().parents[1]


@pytest.fixture(scope='module')
def cached_cards(tmp_path_factory):
    source = (ROOT / 'main/pet_p4_renderer.c').read_text()
    helpers = r'''
#define PET_P4_SESSION_QUEUE_MAX 8
#define PET_P4_AGENT_MAX 64
#define PET_P4_SESSION_QUEUE_ID_MAX 128
typedef struct {char id[32],title[192],content[384],state[32];} pet_p4_session_queue_item_t;
typedef struct {unsigned session_queue_count,current_session_index;bool session_voice_active;char current_session_agent[64];pet_p4_session_queue_item_t session_queue[8];} card_state_t;
static pet_p4_runtime_state_t cards;
static bool g_session_overlay_cache_valid, cache_available=true;
static uint64_t g_session_overlay_signature,g_session_text_refresh_ms;
static unsigned selections,rebuilds,copies,local_views,direct_draws;
static void renderer_bubble_select(const char *owner,unsigned style,const char *title,const char *body,uint64_t now,pet_bubble_text_view_t *out){
 pet_bubble_pixels_select(owner,style,title,body,now,out,NULL);
 selections++;if(!out->pixels)local_views++;
}
'''
    helpers += ''.join(function(source, name) for name in (
        'session_text', 'session_overlay_visible_window', 'session_cache_hash_bytes',
        'session_cache_hash_text', 'session_overlay_signature', 'refresh_session_text_views'))
    # Only expensive raster/copy work is spied. Production cache eligibility,
    # cadence, visible-window selection, keys, receiver and release all execute.
    helpers += r'''
static bool rebuild_session_overlay_cache(const pet_p4_runtime_state_t*s,unsigned long long now,uint64_t signature){
 if(!cache_available)return false;
 rebuilds++;refresh_session_text_views(s,now);
 g_session_overlay_signature=signature;g_session_overlay_cache_valid=true;return true;
}
static void draw_session_queue_uncached(const pet_p4_runtime_state_t*s,unsigned long long now,bool markers){
 (void)markers;direct_draws++;refresh_session_text_views(s,now);
}
static void copy_session_overlay_cache(void){copies++;}
static void draw_session_queue_working_markers(const pet_p4_runtime_state_t*s,unsigned long long now){(void)s;(void)now;}
'''
    helpers += function(source, 'draw_session_queue')
    helpers = helpers.replace('pet_p4_runtime_state_t', 'card_state_t')
    original = Path.write_text

    def write(path, data, *args, **kwargs):
        if path.name == 'main.c' and 'race_writer' in data:
            data = data.replace('static void *race_writer', helpers + '\nstatic void *race_writer', 1)
            marker = '  if(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(c,"release")))'
            extra = r'''
  v=cJSON_GetObjectItemCaseSensitive(c,"cardFrame");if(v){
   const cJSON*items=cJSON_GetObjectItemCaseSensitive(v,"items");
   if(items){cards.session_queue_count=(unsigned)cJSON_GetArraySize(items);assert(cards.session_queue_count<=8);
    for(unsigned i=0;i<cards.session_queue_count;i++){
     const cJSON*item=cJSON_GetArrayItem(items,(int)i);pet_p4_session_queue_item_t*out=&cards.session_queue[i];
     snprintf(out->id,sizeof(out->id),"%s",str(item,"id"));snprintf(out->title,sizeof(out->title),"%s",str(item,"title"));
     snprintf(out->content,sizeof(out->content),"%s",str(item,"body"));snprintf(out->state,sizeof(out->state),"%s",str(item,"state"));
    }
   }
   if(cJSON_GetObjectItemCaseSensitive(v,"selected"))cards.current_session_index=num(v,"selected");
   if(cJSON_GetObjectItemCaseSensitive(v,"cacheAvailable"))cache_available=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(v,"cacheAvailable"));
   local_views=0;draw_session_queue(&cards,num(v,"now"));
   assert(selections==num(v,"selections"));assert(rebuilds==num(v,"rebuilds"));
   assert(local_views==num(v,"localViews"));assert(direct_draws==num(v,"directDraws"));
  }
'''
            assert marker in data
            data = data.replace(marker, extra + marker)
        return original(path, data, *args, **kwargs)

    with pytest.MonkeyPatch.context() as monkey:
        monkey.setattr(Path, 'write_text', write)
        return receiver_tests.receiver.__wrapped__(tmp_path_factory)


def frame(now, selections, rebuilds=1, **extra):
    return {'cardFrame': {'now': now, 'selections': selections, 'rebuilds': rebuilds, **extra}}


@pytest.mark.parametrize('count', [1, 3])
def test_static_cached_card_survives_5s_and_ninth_asset_without_raster_rebuild(cached_cards, count):
    items = [{'id': str(i), 'title': f'title{i}', 'body': f'body{i}'} for i in range(count)]
    assets = [(style, item[field]) for item in items for style, field in [(1, 'title'), (4, 'body')]]
    extra_count = 8 - len(assets)
    assets += [(4, f'extra{i}') for i in range(extra_count + 1)]
    commands = [desire(assets=assets)]
    for item in items:
        commands += send(transfer(item['title'], style=1)) + send(transfer(item['body']))
    for i in range(extra_count):
        commands += send(transfer(f'extra{i}'))
    commands += [frame(100, count, items=items)]
    for second in range(1, 7):
        # Many ordinary screen-cache hits must not repeatedly select or rasterize.
        commands += [frame(second * 1000 + 99, second * count), frame(second * 1000 + 100, (second + 1) * count)]
    commands += [{**packet, 'now': 6101} for packet in send(transfer(f'extra{extra_count}'))]
    for item in items:
        commands += [select(item['title'], item['body'], True, now=6102)]
    commands += [frame(6103, 8 * count, rebuilds=2)]
    cached_cards(commands)


def test_cached_missing_notice_retries_each_second_and_new_pair_never_uses_old_text(cached_cards):
    commands = [desire(assets=[(1, 'title'), (4, 'old'), (4, 'new')])]
    commands += send(transfer('title', style=1)) + send(transfer('old'))
    commands += [frame(100, 1, items=[{'id': 's', 'title': 'title', 'body': 'old'}]),
                 frame(200, 2, rebuilds=2, localViews=1, items=[{'id': 's', 'title': 'title', 'body': 'new'}]),
                 {'need': {'now': 201, 'ok': True, 'style': 4, 'page': 0}},
                 frame(1199, 2, rebuilds=2), {'need': {'now': 1201, 'ok': False}},
                 frame(1202, 3, rebuilds=2, localViews=1),
                 {'need': {'now': 1203, 'ok': True, 'style': 4, 'page': 0}},
                 frame(2202, 4, rebuilds=2, localViews=1),
                 {'need': {'now': 2203, 'ok': True, 'style': 4, 'page': 0}}]
    commands += [{**packet, 'now': 2300} for packet in send(transfer('new'))]
    commands += [frame(2301, 5, rebuilds=3), frame(2302, 5, rebuilds=3)]
    cached_cards(commands)


def test_refresh_only_visible_window_clock_rollback_empty_and_no_cache(cached_cards):
    items = [{'id': str(i), 'title': f't{i}', 'body': f'b{i}'} for i in range(8)]
    cached_cards([frame(100, 3, localViews=3, items=items, selected=4),
                  frame(1099, 3), frame(1100, 6, localViews=3),
                  frame(50, 9, localViews=3),  # clock rollback renews immediately
                  frame(51, 12, rebuilds=2, localViews=3, selected=8),
                  frame(52, 12, rebuilds=2, items=[]),
                  frame(53, 15, rebuilds=3, localViews=3, items=items),
                  frame(54, 18, rebuilds=3, localViews=3, selected=1, cacheAvailable=False, directDraws=1)])


@pytest.mark.parametrize('title', ['title', '标题' * 30])
def test_caption_16_pages_reclaim_refill_and_protect_current_between_frames(receiver, title):
    # Realtime title key uses 63 UTF-8 bytes even though the view retains more.
    title_key_text = title.encode()[:63].decode('utf-8')
    commands = [desire(assets=[(1, title_key_text), (2, 'caption')])]
    commands += send(transfer(title_key_text, style=1))
    commands += send(transfer('caption', style=2, page=0, pages=16))
    commands += send(transfer('caption', style=2, page=1, pages=16))
    commands += [select(title, 'caption', True, now=100, style=2, hold=True),
                 {'drawHeld': {'style': 2, 'page': 1, 'pinPage': True, 'ok': True, 'first': 0}}, {'release': True}]
    # Releasing and reselecting the view must not reset its current-page lease.
    commands += [select(title, 'caption', True, now=101, style=2)]
    for page in range(2, 16):
        commands += [{**packet, 'now': 200 + page} for packet in send(transfer('caption', style=2, page=page, pages=16))]
    commands += [select(title, 'caption', True, now=300, style=2, hold=True),
                 {'drawHeld': {'style': 1, 'ok': True, 'first': 0}},
                 {'drawHeld': {'style': 2, 'page': 1, 'pinPage': True, 'ok': True, 'first': 0}},
                 {'drawHeld': {'style': 2, 'page': 2, 'pinPage': True, 'ok': False}}, {'release': True},
                 {'need': {'now': 301, 'ok': True, 'style': 2, 'page': 2}}]
    # Page two was evicted; upload and all following pages can reclaim old pages.
    for page in range(2, 16):
        packets = transfer('caption', style=2, page=page, pages=16)
        commands += [{**packet, 'now': 400 + page * 10} for packet in send(packets)]
        commands += [select(title, 'caption', True, now=401 + page * 10, style=2, hold=True),
                     {'drawHeld': {'style': 2, 'page': page, 'pinPage': True, 'ok': True, 'first': 0}}, {'release': True}]
    receiver(commands)


# Use the existing unmodified-receiver sanitizer fixture for the page tests.
receiver = receiver_tests.receiver


def test_requested_missing_caption_page_survives_prefetch_before_first_draw(receiver):
    commands = [desire(assets=[(1, 'title'), (2, 'caption')])]
    commands += send(transfer('title', style=1)) + send(transfer('caption', style=2, pages=16))
    commands += [select('title', 'caption', True, now=100, style=2, hold=True),
                 {'drawHeld': {'style': 2, 'page': 8, 'pinPage': True, 'ok': False}}, {'release': True},
                 {'need': {'now': 101, 'ok': True, 'style': 2, 'page': 8}}]
    for page in range(1, 16):
        commands += [{**packet, 'now': 200 + page} for packet in send(transfer('caption', style=2, page=page, pages=16))]
    commands += [select('title', 'caption', True, now=300, style=2, hold=True),
                 {'drawHeld': {'style': 2, 'page': 8, 'pinPage': True, 'ok': True, 'first': 0}}, {'release': True}]
    receiver(commands)
