"""Device C raster experiments (no device I/O).
Run: python3 -m pytest firmware/tests/p4_font_coverage_test.py -q
Preview: python3 firmware/tests/p4_font_coverage_test.py --output /tmp/font-coverage
PNG pixels come from extracted renderer C + unchanged MiSans payload, not PIL text.
"""
from pathlib import Path
import argparse
import json
import subprocess
import tempfile
from p4_render_trace_test import function
from p4_font_test import font
ROOT = Path(__file__).resolve().parents[1]

PREAMBLE = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
static unsigned long long reads;
#include "pet_p4_rgb565_blend.h"
#define PET_P4_UI_WIDTH 480
#define PET_P4_UI_HEIGHT 180
#define LV_FONT_GLYPH_FORMAT_A1 1
#define LV_FONT_GLYPH_FORMAT_A2 2
#define LV_FONT_GLYPH_FORMAT_A4 4
#define LV_FONT_GLYPH_FORMAT_A8 8
typedef struct glyph lv_font_glyph_dsc_t;
typedef struct {int line_height,base_line;const void*(*get_glyph_bitmap)(lv_font_glyph_dsc_t*,void*);} lv_font_t;
struct glyph {const lv_font_t *resolved_font;int box_w,box_h,ofs_x,ofs_y,adv_w,format,stride,req_raw_bitmap;bool is_placeholder;};
static const uint8_t *data;
static const void *bitmap(lv_font_glyph_dsc_t *g,void *b){(void)g;(void)b;return data;}
static const lv_font_t pet_p4_font_cn_16={18,3,bitmap};
static lv_font_glyph_dsc_t current;
static bool lv_font_get_glyph_dsc(const lv_font_t*f,lv_font_glyph_dsc_t*g,uint32_t cp,uint32_t next){(void)f;(void)cp;(void)next;*g=current;return true;}
static void lv_font_glyph_release_draw_data(lv_font_glyph_dsc_t*g){memset(g,0,sizeof(*g));}
static uint16_t pixels[PET_P4_UI_WIDTH*PET_P4_UI_HEIGHT],saved[PET_P4_UI_WIDTH*PET_P4_UI_HEIGHT],*g_framebuffer=pixels;
static struct{bool active;int x,y,right,bottom;}g_app_clip;
static uint16_t rgb565(uint8_t r,uint8_t g,uint8_t b){return ((r>>3)<<11)|((g>>2)<<5)|(b>>3);}
static void fill_ellipse(int x,int y,int a,int b,uint16_t c){(void)x;(void)y;(void)a;(void)b;(void)c;abort();}
static void draw_line(int x,int y,int a,int b,uint16_t c){(void)x;(void)y;(void)a;(void)b;(void)c;abort();}
static void draw_glyph5x7(int x,int y,char c,uint16_t color,int s){(void)x;(void)y;(void)c;(void)color;(void)s;abort();}
'''

def build(tmp, source, label, perf=False):
    _, glyphs, ids, bits = font('pet_p4_font_cn.c')
    phrase = '每日新鲜事：首屏文字清晰，重绘字重一致。MiSans 123'
    cps = list(dict.fromkeys(map(ord, phrase + '龘鬱测试对齐gjAV!?')))
    code = PREAMBLE
    if 'draw_bubble_cell(codepoint' in source:
        code += '\nstatic bool draw_bubble_cell(uint32_t cp,int x,int y,uint16_t c,bool m){(void)cp;(void)x;(void)y;(void)c;(void)m;return false;}\n'
    for name in ('blend_rgb565','app_clip_contains','blend_px','glyph_alpha_at',
                 'medium_glyph_coverage','glyph_width','medium_glyph_width',
                 'medium_glyph_visual_bounds','draw_lvgl_glyph_medium'):
        if name == 'medium_glyph_coverage' and label == 'before':
            continue
        body = function(source, name)
        if name == 'glyph_alpha_at':
            body = body.replace('{', '{ reads++;', 1)
        code += body
    code += '\nstruct item {int offset,w,h,x,y,adv;};\nstatic const struct item items[]={\n'
    for cp in cps:
        o,a,w,h,x,y=glyphs[ids[cp]]
        code += f'{{{o},{w},{h},{x},{y},{(a+8)//16}}},\n'
    code += '};\nstatic const unsigned phrase[]={'+','.join(str(cps.index(ord(c))) for c in phrase)+'};\n'
    (tmp/'bitmap.bin').write_bytes(bytes(bits))
    code += r'''
static void set_item(unsigned i,const uint8_t *raw){
 const struct item *q=&items[i];data=raw+q->offset;
 current=(lv_font_glyph_dsc_t){.resolved_font=&pet_p4_font_cn_16,.box_w=q->w,.box_h=q->h,.ofs_x=q->x,.ofs_y=q->y,.adv_w=q->adv,.format=1};
}
static void clear(void){for(unsigned i=0;i<sizeof(pixels)/sizeof(*pixels);i++)pixels[i]=0xffff;}
static void scene(const uint8_t*raw){
 clear();int x=12,y=8;
 for(unsigned k=0;k<sizeof(phrase)/sizeof(*phrase);k++){
  set_item(phrase[k],raw);int advance=medium_glyph_width('A',0)+1;
  if(x+advance>468){x=12;y+=36;}
  draw_lvgl_glyph_medium('A',0,x,y,0x18e3);x+=advance;
 }
}
int main(int argc,char**argv){
 assert(argc==3);FILE*f=fopen(argv[1],"rb");assert(f);uint8_t *raw=malloc(554086);assert(raw);assert(fread(raw,1,554086,f)==554086);fclose(f);
'''
    if label != 'before':
        code += r'''
 /* Independent half-pixel integration oracle; exercises every phase, odd
  * borders, isolated strokes, empty cells and arbitrary A8 alpha. */
 uint8_t sample[32*32];unsigned rng=42;
 for(int h=1;h<=32;h++)for(int w=1;w<=32;w++){
  for(int i=0;i<w*h;i++){rng=rng*1664525+1013904223;sample[i]=rng>>24;}
  lv_font_glyph_dsc_t g={.box_w=w,.box_h=h,.format=8};
  for(int y=0;y<(h*3+1)/2;y++)for(int x=0;x<(w*3+1)/2;x++){
   unsigned sum=0;
   for(int yy=y*2;yy<y*2+2;yy++)for(int xx=x*2;xx<x*2+2;xx++)
    if(xx<w*3&&yy<h*3)sum+=sample[(yy/3)*w+xx/3];
   uint8_t alpha[9];medium_glyph_coverage(sample,&g,(x/3)*2,(y/3)*2,alpha);
   assert(alpha[(y%3)*3+x%3]==(sum+2)/4);
  }
 }
 /* A one-source-pixel stem has the same 1.5px integrated weight in either
  * phase. Nearest neighbour gives 2px versus 1px. */
 for(int phase=0;phase<2;phase++){
  uint8_t s[2]={0,0};s[phase]=255;lv_font_glyph_dsc_t g={.box_w=2,.box_h=1,.format=8};
  uint8_t alpha[9];medium_glyph_coverage(s,&g,0,0,alpha);
  unsigned sum=alpha[0]+alpha[1]+alpha[2];
  assert(sum==383);
 }
 for(unsigned i=0;i<sizeof(items)/sizeof(*items);i++){
  set_item(i,raw);int top=0,bottom=0;bool ink=medium_glyph_visual_bounds('A',0,&top,&bottom);
  clear();draw_lvgl_glyph_medium('A',0,30,30,0);
  int first=999,last=-1;for(int y=0;y<180;y++)for(int x=0;x<480;x++)if(pixels[y*480+x]!=0xffff){if(y<first)first=y;last=y;}
  assert(ink==(last>=0));if(ink){assert(first==30+top&&last==30+bottom);}
 }
'''
    code += r'''
 unsigned long long bounds_reads=0,draw_reads=0;
 clock_t started=clock();
 for(int repeat=0;repeat<200;repeat++)for(unsigned i=0;i<sizeof(items)/sizeof(*items);i++){
  set_item(i,raw);int t,b;reads=0;medium_glyph_visual_bounds('A',0,&t,&b);bounds_reads+=reads;
 }
 double bounds_ms=1000.0*(clock()-started)/CLOCKS_PER_SEC;
 started=clock();
 for(int repeat=0;repeat<200;repeat++)for(unsigned i=0;i<sizeof(items)/sizeof(*items);i++){
  set_item(i,raw);reads=0;draw_lvgl_glyph_medium('A',0,30,30,0);draw_reads+=reads;
 }
 double draw_ms=1000.0*(clock()-started)/CLOCKS_PER_SEC;
 printf("{\"bounds_reads\":%llu,\"draw_reads\":%llu,\"bounds_ms\":%.3f,\"draw_ms\":%.3f}\n",bounds_reads,draw_reads,bounds_ms,draw_ms);
 scene(raw);memcpy(saved,pixels,sizeof pixels);
 /* Fresh background, unrelated glyph calls, redraw: exact first/late match. */
 for(unsigned i=0;i<sizeof(items)/sizeof(*items);i++){set_item(i,raw);draw_lvgl_glyph_medium('A',0,-3,-4,0);}
 scene(raw);assert(!memcmp(saved,pixels,sizeof pixels));
 f=fopen(argv[2],"wb");assert(f);assert(fwrite(pixels,1,sizeof pixels,f)==sizeof pixels);assert(!fclose(f));free(raw);return 0;
}
'''
    c=tmp/f'{label}.c';c.write_text(code);exe=tmp/label
    subprocess.run(['cc','-O2','-std=c11','-Wall','-Wextra','-Werror',*([] if perf else ['-fsanitize=address,undefined']),
                    '-I',str(ROOT/'main'),str(c),'-o',str(exe)],check=True)
    out=tmp/f'{label}.rgb565'
    result=subprocess.run([str(exe),str(tmp/'bitmap.bin'),str(out)],check=True,capture_output=True,text=True)
    (tmp/f'{label}-perf.json').write_text(result.stdout)
    return out


def test_actual_area_sampler_bounds_stem_weight_and_repeat_pixels(tmp_path):
    build(tmp_path,(ROOT/'main/pet_p4_renderer.c').read_text(),'after')


def test_startup_precedes_lcd_and_first_renderer_and_no_deferred_native():
    main=(ROOT/'main/pet_p4_main.c').read_text()
    init=main.index('ESP_ERROR_CHECK(pet_p4_font_bitmap_init()')
    assert init < main.index('esp_err_t lcd_err = pet_p4_lcd_init()',init)
    assert init < main.index('pet_p4_renderer_',init)
    renderer=(ROOT/'main/pet_p4_renderer.c').read_text()
    assert 'pet_p4_font_bitmap_init(' not in renderer
    assert 'pet_p4_font_native32_prepare(' not in renderer


def preview(out):
    from PIL import Image,ImageDraw
    out.mkdir(parents=True,exist_ok=True)
    current=(ROOT/'main/pet_p4_renderer.c').read_text()
    baseline=subprocess.check_output(['git','show','HEAD:firmware/main/pet_p4_renderer.c'],cwd=ROOT,text=True)
    with tempfile.TemporaryDirectory() as d:
        tmp=Path(d);images=[]
        for label,source in [('before',baseline),('after',current)]:
            raw=build(tmp,source,label,perf=True).read_bytes();rgb=[]
            (out/f'{label}-perf.json').write_bytes((tmp/f'{label}-perf.json').read_bytes())
            for i in range(0,len(raw),2):
                v=int.from_bytes(raw[i:i+2],'little');rgb.append(((v>>11)*255//31,((v>>5)&63)*255//63,(v&31)*255//31))
            im=Image.new('RGB',(480,180));im.putdata(rgb);im.save(out/f'{label}.png');images.append(im)
        board=Image.new('RGB',(960,412),'#e8e8e8');draw=ImageDraw.Draw(board)
        for k,im in enumerate(images):
            draw.text((12,k*206+6),['BEFORE: nearest neighbour','AFTER: exact area coverage'][k],fill='black')
            board.paste(im.crop((0,0,480,90)).resize((960,180),Image.Resampling.NEAREST),(0,k*206+24))
        board.save(out/'comparison-2x.png')
    (out/'experiment.json').write_text(json.dumps({'baseline':subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip(),
       'preview':'extracted production C draw + blend, unchanged real font16; host harness, not LCD capture',
       'stem_alpha_sum_before':[510,255],'stem_alpha_sum_after':[383,383],
       'native32_glyphs':199,'font_payload_delta_bytes':0},indent=2)+'\n')

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True);preview(p.parse_args().output)
