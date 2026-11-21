"""Actual production scale2 C preview; shows the retained 199-glyph limitation."""
import hashlib
from pathlib import Path
import subprocess
from p4_font_test import font
from p4_font_coverage_test import PREAMBLE, ROOT
from p4_render_trace_test import function

def render(tmp, source, label):
    phrase='系统设置 蓝牙键盘 每日新鲜事 龘鬱'
    _,g16,ids16,b16=font('pet_p4_font_cn.c')
    _,g32,ids32,b32=font('pet_p4_font_cn_32.c')
    code=PREAMBLE.replace('static const uint8_t *data;', 'static const uint8_t *data,*native_data;')
    code=code.replace('return data;', 'return g->format==2?native_data:data;')
    code=code.replace('static lv_font_glyph_dsc_t current;', 'static const lv_font_t pet_p4_font_cn_32={36,6,bitmap};\nstatic lv_font_glyph_dsc_t current,native;\nstatic bool pet_p4_font_native32_has_glyph(uint32_t cp){(void)cp;return native.adv_w>0;}')
    code=code.replace('*g=current;return true;', '*g=f==&pet_p4_font_cn_32?native:current;return true;')
    # All title characters in this fixture exist locally, including the 16px fallbacks.
    code += '\nstatic bool draw_bubble_cell(uint32_t cp,int x,int y,uint16_t color,bool medium){abort();}\n'
    for n in ('blend_rgb565','app_clip_contains','blend_px','glyph_alpha_at','scaled_glyph_dsc','scaled_glyph_baseline','draw_lvgl_glyph','glyph_visual_bounds','glyph_width'):
        code+=function(source,n)
    code+='\nint main(int argc,char**argv){assert(argc==4);uint8_t raw[554086],big[30579];FILE*f=fopen(argv[1],"rb");assert(f&&fread(raw,1,sizeof raw,f)==sizeof raw);fclose(f);f=fopen(argv[2],"rb");assert(f&&fread(big,1,sizeof big,f)==sizeof big);fclose(f);for(int i=0;i<480*180;i++)pixels[i]=0xffff;int x=12,y=8;\n'
    for cp in map(ord,phrase):
        o,a,w,h,ox,oy=g16[ids16[cp]]
        code+=f'data=raw+{o};current=(lv_font_glyph_dsc_t){{.resolved_font=&pet_p4_font_cn_16,.box_w={w},.box_h={h},.ofs_x={ox},.ofs_y={oy},.adv_w={(a+8)//16},.format=1}};\n'
        if cp in ids32:
            o,a,w,h,ox,oy=g32[ids32[cp]]
            code+=f'native_data=big+{o};native=(lv_font_glyph_dsc_t){{.resolved_font=&pet_p4_font_cn_32,.box_w={w},.box_h={h},.ofs_x={ox},.ofs_y={oy},.adv_w={(a+8)//16},.format=2}};\n'
        else:
            code+='memset(&native,0,sizeof native);\n'
        code+="{int adv=glyph_width('A',0)*2+2;if(x+adv>468){x=12;y+=48;}int t,b;(void)glyph_visual_bounds('A',0,2,&t,&b);draw_lvgl_glyph('A',0,x,y,0x18e3,2);x+=adv;}\n"
    code+='f=fopen(argv[3],"wb");assert(f&&fwrite(pixels,1,sizeof pixels,f)==sizeof pixels);fclose(f);return 0;}'
    c=tmp/f'title-{label}.c';c.write_text(code);exe=tmp/f'title-{label}'
    for name,bits in [('font16',b16),('font32',b32)]:
        (tmp/name).write_bytes(bytes(bits))
    subprocess.run(['cc','-O2','-std=c11','-fsanitize=address,undefined','-DPET_FONT_NATIVE_32=1','-I',str(ROOT/'main'),str(c),'-o',str(exe)],check=True)
    out=tmp/f'title-{label}.rgb565'
    subprocess.run([str(exe),str(tmp/'font16'),str(tmp/'font32'),str(out)],check=True)
    return out.read_bytes()

def test_mixed_scale2_pixels_preserved(tmp_path):
    current=(ROOT/'main/pet_p4_renderer.c').read_text()
    # RGB565 reference from the renderer before remote bubble glyph support.
    expected = '749baea66eb84df74cc534a19bd49b11f1ea5bde4754ae03525650e8e1d95854'
    assert hashlib.sha256(render(tmp_path,current,'after')).hexdigest() == expected

if __name__=='__main__':
    import argparse, tempfile, zipfile, json
    from PIL import Image,ImageDraw,ImageFont
    p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True);p.add_argument('--official-zip',type=Path,required=True);a=p.parse_args()
    a.output.mkdir(exist_ok=True,parents=True)
    with tempfile.TemporaryDirectory() as d:
        tmp=Path(d)
        with zipfile.ZipFile(a.official_zip) as z:
            otf=z.read('MiSans/otf/MiSans-Medium.otf') # Extract exactly one member.
        (tmp/'medium.otf').write_bytes(otf)
        current=(ROOT/'main/pet_p4_renderer.c').read_text()
        raw=render(tmp,current,'after');rgb=[]
        for i in range(0,len(raw),2):
            v=int.from_bytes(raw[i:i+2],'little');rgb.append(((v>>11)*255//31,((v>>5)&63)*255//63,(v&31)*255//31))
        actual=Image.new('RGB',(480,180));actual.putdata(rgb);actual.save(a.output/'title-device-unchanged.png')
        comparison=Image.new('RGB',(960,420),'white');draw=ImageDraw.Draw(comparison)
        draw.text((12,4),'DEVICE 32px: native199 + 16px x2 (unchanged)',fill='black')
        comparison.paste(actual.crop((0,0,480,90)).resize((960,180),Image.Resampling.NEAREST),(0,22))
        draw.text((12,210),'OFFLINE OTF 32px reference: not firmware / not layout-equivalent',fill='black')
        reference=Image.new('RGB',(480,90),'white');rd=ImageDraw.Draw(reference)
        ff=ImageFont.truetype(str(tmp/'medium.otf'),32)
        rd.text((12,4),'系统设置 蓝牙键盘',font=ff,fill='#181c18')
        rd.text((12,45),'每日新鲜事 龘鬱',font=ff,fill='#181c18')
        comparison.paste(reference.resize((960,180),Image.Resampling.NEAREST),(0,232))
        comparison.save(a.output/'title-device-vs-official.png')
        (a.output/'official-reference.json').write_text(json.dumps({'zip_member':'MiSans/otf/MiSans-Medium.otf','sha256':hashlib.sha256(otf).hexdigest(),'reference_only':True},indent=2)+'\n')
