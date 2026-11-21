"""Compile the actual device card renderer; exercise every catalog size/focus.

Optional --preview PATH emits the same draw calls for a host layout preview.
This is not a hardware display, serial or audio acceptance test.
"""
from pathlib import Path
import json
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

HARNESS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define PET_P4_UI_WIDTH 640
#define PET_P4_UI_HEIGHT 480
#define EXT_RAM_BSS_ATTR
typedef struct {
 char widget_id[64],title[128];
 bool active;
 uint32_t center_title_checksum;
 uint8_t center_title_slot,center_title_generation;
} pet_p4_miniapp_catalog_entry_t;
static size_t pet_p4_miniapp_center_read_title(const pet_p4_miniapp_catalog_entry_t *entry,uint8_t *data,size_t capacity){
 (void)entry;(void)data;(void)capacity;return 0;
}
static size_t count,selected,reads;
static pet_center_filter_t active_filter;
static pet_center_filter_t pet_p4_miniapp_center_filter(void){return active_filter;}
static bool preview;
static const char *ids[]={"city-radio","xiangqi","wasm-2046","wasm-piano"};
static const char *titles[]={"城市电台","中国象棋","2046","模拟琴键"};
static const char *media_ids[]={"city-radio","daily-news","book-pocket","hachimi-sounds","hachimi-learn"};
static const char *media_titles[]={"城市电台","每日新鲜事","每天一本书","自然声景","每日新知"};
static size_t pet_p4_miniapp_center_count(void){return count;}
static size_t pet_p4_miniapp_center_selected(void){return selected;}
static bool pet_p4_miniapp_center_get(size_t i,pet_p4_miniapp_catalog_entry_t *out){
 assert(i<count);assert(i/4==selected/4);++reads;
 const bool media=active_filter==PET_CENTER_MEDIA;
 const char *id=media&&i<5?media_ids[i]:i<4?ids[i]:"other";
 const char *title=media&&i<5?media_titles[i]:i<4?titles[i]:"名称很长很长很长的组件";
 snprintf(out->widget_id,sizeof(out->widget_id),"%s",id);
 snprintf(out->title,sizeof(out->title),"%s",title);out->active=i==0;return true;
}
static uint16_t rgb565(int r,int g,int b){return ((r>>3)<<11)|((g>>2)<<5)|(b>>3);}
static void bounds(int x,int y,int w,int h){assert(x>=0&&y>=0&&w>0&&h>0&&x+w<=640&&y+h<=480);}
static void fill_round_rect(int x,int y,int w,int h,int r,uint16_t c){
 bounds(x,y,w,h);assert(r>=0&&r<=w/2&&r<=h/2);
 if(preview)printf("[\"rect\",%d,%d,%d,%d,%d,%u]\n",x,y,w,h,r,c);
}
static void fill_rect(int x,int y,int w,int h,uint16_t c){fill_round_rect(x,y,w,h,0,c);}
static void fill_round_rect_outline(int x,int y,int w,int h,int r,uint16_t f,uint16_t o){
 fill_round_rect(x,y,w,h,r,o);fill_round_rect(x+2,y+2,w-4,h-4,r-2,f);
}
static void fill_ellipse(int x,int y,int rx,int ry,uint16_t c){
 bounds(x-rx,y-ry,rx*2,ry*2);if(preview)printf("[\"ellipse\",%d,%d,%d,%d,%u]\n",x,y,rx,ry,c);
}
static void draw_line(int x,int y,int x1,int y1,uint16_t c){
 assert(x>=0&&x<640&&x1>=0&&x1<640&&y>=0&&y<480&&y1>=0&&y1<480);
 if(preview)printf("[\"line\",%d,%d,%d,%d,%u]\n",x,y,x1,y1,c);
}
static int text_y_in_box(const char *t,int s,int y,int h){(void)t;return y+(h-16*s)/2;}
static void text(const char *t,int x,int y,int max,uint16_t c,int s,const char *anchor){
 assert(y>=0&&y+16*s<=480&&max>0&&s>0);assert(x>=0&&x<=640);
 if(preview)printf("[\"text\",%d,%d,%d,%d,%u,\"%s\",\"%s\"]\n",x,y,max,s,c,anchor,t);
}
static void draw_text_line(const char*t,int x,int y,int w,uint16_t c,int s,bool e){(void)e;bounds(x,y,w,16*s);text(t,x,y,w,c,s,"left");}
static void draw_text_right(const char*t,int x,int y,int w,uint16_t c,int s){text(t,x,y,w,c,s,"right");}
static void draw_text_center(const char*t,int x,int y,int w,uint16_t c,int s){text(t,x,y,w,c,s,"center");}
static uint16_t g_framebuffer[640*480], background_pixels[640*480];
static void pet_text_alpha_blit(const uint8_t *data,size_t size,unsigned width,uint16_t *frame,int x,int y,int clip,uint16_t c){
 (void)frame;
 unsigned pixels=0;
 for(size_t i=0;i<size;i++)pixels+=(data[i]&15u)+1u;
 assert(width&&pixels%width==0&&clip>0);
 bounds(x,y,(int)width,(int)(pixels/width));
 if(preview){
  printf("[\"alpha\",%d,%d,%u,%u,%d,%u,\"",x,y,width,pixels/width,clip,c);
  for(size_t i=0;i<size;i++)printf("%02x",data[i]);
  puts("\"]");
 }
}

/* Run the same zlib stream through the host decoder when exercising the
 * extracted renderer. ESP-IDF uses tinfl and PSRAM for the same buffers. */
typedef struct { unsigned unused; } tinfl_decompressor;
typedef int tinfl_status;
enum { MALLOC_CAP_SPIRAM = 1, MALLOC_CAP_8BIT = 2, TINFL_STATUS_DONE = 0,
       TINFL_FLAG_PARSE_ZLIB_HEADER = 1, TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF = 2 };
static void *heap_caps_malloc(size_t size, unsigned caps) {
 assert(caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
 return malloc(size);
}
static void heap_caps_free(void *p){ free(p); }
static void tinfl_init(tinfl_decompressor *state){ state->unused=0; }
static tinfl_status tinfl_decompress(tinfl_decompressor *state, const uint8_t *input,
                                     size_t *in_len, uint8_t *start, uint8_t *output,
                                     size_t *out_len, unsigned flags){
 assert(state->unused==0 && start==output);
 assert(flags==(TINFL_FLAG_PARSE_ZLIB_HEADER|TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF));
 z_stream stream={.next_in=(Bytef *)input,.avail_in=(uInt)*in_len,
                  .next_out=output,.avail_out=(uInt)*out_len};
 assert(inflateInit(&stream)==Z_OK);
 int result=inflate(&stream,Z_FINISH);
 *in_len=stream.total_in;
 *out_len=stream.total_out;
 assert(inflateEnd(&stream)==Z_OK);
 return result==Z_STREAM_END?TINFL_STATUS_DONE:-1;
}

static uint16_t *center_background=background_pixels;
static void center_art_prepare(void){if(preview)puts("[\"background\"]");}
static bool center_draw_thumbnail(const char *id,int x,int y){
 for(size_t i=0;i<CENTER_ART_COUNT;i++)if(!strcmp(id,center_art_catalog[i].id)&&center_art_catalog[i].art){
  bounds(x,y,64,48);if(preview)printf("[\"thumbnail\",\"%s\",%d,%d]\n",id,x,y);return true;
 }
 return false;
}
'''
MAIN = r'''
int main(int argc,char **argv){
 (void)argv;
 for(count=0;count<=32;++count){
   for(selected=0;selected<(count?count:1);++selected){
     reads=0;render_component_center_page(false);
     size_t remaining=count-selected/4*4;assert(reads==(remaining<4?remaining:4));
   }
 }
 count=0;selected=99;render_component_center_page(true);
 for(active_filter=PET_CENTER_GAMES;active_filter<PET_CENTER_FILTER_COUNT;++active_filter)render_component_center_page(false);
 active_filter=PET_CENTER_ALL;
 if(argc>1){
   preview=true;
   active_filter=!strcmp(argv[1],"media")?PET_CENTER_MEDIA:PET_CENTER_ALL;
   count=active_filter==PET_CENTER_MEDIA?5:CENTER_ART_COUNT;selected=0;
   render_component_center_page(false);
 }
 return 0;
}
'''


def run_renderer(preview=False):
    source = (ROOT / "main/pet_p4_renderer.c").read_text()
    renderer = "static void draw_component_icon" + source.split(
        "static void draw_component_icon", 1)[1].split("static void draw_connection_banner", 1)[0]
    # The diagnostic page is a separate renderer with its own state/physics.
    start = renderer.index("/* Engineering touch/slicing diagnostic")
    end = renderer.index("typedef struct {\n  uint32_t checksum;", start)
    renderer = renderer[:start] + renderer[end:]
    with tempfile.TemporaryDirectory() as tmp:
        code, binary = Path(tmp)/"cards.c", Path(tmp)/"cards"
        headers = '#include "pet_p4_center_layout.h"\n#include <stdbool.h>\n#include <stdlib.h>\n#include <zlib.h>\n#include "pet_p4_center_text.h"\n#include "../assets/component-center/catalog.h"\n#include "../assets/component-center/text_masks.h"\n'
        code.write_text(headers + HARNESS + renderer + MAIN)
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "main"), str(code), "-lz", "-o", str(binary)], check=True)
        return subprocess.check_output([str(binary)] + ([preview] if preview else []), text=True)


def test_card_renderer_bounds_empty_single_partial_full_pages():
    run_renderer()


def test_center_hit_targets_and_partial_page_navigation(tmp_path):
    binary=tmp_path/'center-layout'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-I',str(ROOT/'main'),
        str(ROOT/'tests/p4_center_layout_test.c'),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)


def preview(path, category):
    from PIL import Image, ImageDraw, ImageFont
    image = Image.new("RGB", (640, 480))
    draw = ImageDraw.Draw(image)
    font_path = "/System/Library/Fonts/STHeiti Medium.ttc"
    color = lambda n: ((n >> 11)*255//31, ((n >> 5)&63)*255//63, (n&31)*255//31)
    import re
    import struct
    import zlib
    asset_root=ROOT/'assets/component-center'
    thumbnail_ids=re.findall(r'^  \{"([^"]+)",', (asset_root/'catalog.h').read_text(), re.MULTILINE)
    thumbs=zlib.decompress((asset_root/'center_thumbs.deflate').read_bytes())
    for line in run_renderer(category).splitlines():
        op, *a = json.loads(line)
        if op == "background":
            image.paste(Image.open(ROOT / 'assets/component-center/center_background.jpg').convert('RGB'))
        elif op == "thumbnail":
            ident,x,y=a
            size=32+64*48//2
            start=thumbnail_ids.index(ident)*size
            palette=struct.unpack("<16H",thumbs[start:start+32])
            indices=thumbs[start+32:start+size]
            thumb=Image.frombytes("RGB",(64,48),bytes(c for pair in indices
                for index in (pair>>4,pair&15) for c in color(palette[index])))
            mask=Image.new("L",(64,48))
            ImageDraw.Draw(mask).rounded_rectangle((0,0,63,47),7,fill=255)
            image.paste(thumb,(x,y),mask)
        elif op == "rect":
            x,y,w,h,r,c=a;draw.rounded_rectangle((x,y,x+w-1,y+h-1),r,fill=color(c))
        elif op == "line":
            x,y,x1,y1,c=a;draw.line((x,y,x1,y1),fill=color(c))
        elif op == "ellipse":
            x,y,rx,ry,c=a;draw.ellipse((x-rx,y-ry,x+rx,y+ry),fill=color(c))
        elif op == "alpha":
            x,y,w,h,clip,c,hex_runs=a
            coverage=bytes(alpha for run in bytes.fromhex(hex_runs)
                for alpha in [17*(run>>4)]*((run&15)+1))
            mask=Image.frombytes("L",(w,h),coverage)
            if w>clip: mask=mask.crop((0,0,clip,h))
            image.paste(color(c),(x,y,x+mask.width,y+h),mask)
        elif op == "text":
            x,y,w,s,c,anchor,t=a; font=ImageFont.truetype(font_path,16*s)
            while font.getlength(t)>w: t=t[:-1]
            width=font.getlength(t)
            if anchor=="right": x-=width
            if anchor=="center": x-=width/2
            draw.text((x,y),t,font=font,fill=color(c),anchor="lt")
    image.save(path)


if __name__ == "__main__":
    import argparse
    parser=argparse.ArgumentParser();parser.add_argument("--preview")
    parser.add_argument("--category",choices=("all","media"),default="all")
    args=parser.parse_args()
    if args.preview: preview(args.preview,args.category)
    else: run_renderer()
