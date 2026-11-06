"""Create the A1 project, Chinese geometry preview, and a local slicing check."""
from pathlib import Path
import subprocess,zipfile,json,shutil,sys
ROOT=Path(__file__).resolve().parent
PREV=ROOT.parent
NAME='HD28091-R10-joystick-plus2mm-rootR04'
for file in ['a1-machine.json','a1-process.json','generic-abs.json']:
    shutil.copy2(PREV/file,ROOT/file)
common=[r'C:\Program Files\Bambu Studio\bambu-studio.exe',
        '--load-settings',str(ROOT/'a1-machine.json')+';'+str(ROOT/'a1-process.json'),
        '--load-filaments',str(ROOT/'generic-abs.json'),'--orient','0','--arrange','1','--ensure-on-bed']
model=str(ROOT/(NAME+'-screen-up.stl'))
run=subprocess.run(common+['--export-3mf',NAME+'-screen-up.3mf','--outputdir',str(ROOT),model],
                   capture_output=True,text=True,encoding='utf8',errors='replace',timeout=120)
(ROOT/'project-export.log').write_text(run.stdout+'\n'+run.stderr,encoding='utf8')
assert run.returncode==0
with zipfile.ZipFile(ROOT/(NAME+'-screen-up.3mf')) as z:
    assert z.testzip() is None and not any(n.endswith('.gcode') for n in z.namelist())
    assert json.loads(z.read('Metadata/project_settings.config'))['printer_model']=='Bambu Lab A1'
    (ROOT/'native-preview.png').write_bytes(z.read('Metadata/plate_1.png'))

sys.path.insert(0,str(PREV.parent/'.cadlibs'))
import manifold3d as mf
import numpy as np
import FreeCAD,Mesh
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib import font_manager
font_manager.fontManager.addfont(r'C:\Windows\Fonts\msyh.ttc')
plt.rcParams.update({'font.family':'Microsoft YaHei','axes.unicode_minus':False})

def section(path):
    v,t=Mesh.Mesh(str(path)).Topology
    s=mf.Manifold(mf.Mesh64(np.array([tuple(p) for p in v]),np.array(t,dtype=np.uint64)))
    return s.slice(15.75).to_polygons()
old=section(PREV/'HD28091-R10-joystick-plus2mm.stl')
new=section(ROOT/(NAME+'.stl'))
fig=plt.figure(figsize=(12,8))
grid=fig.add_gridspec(2,2,width_ratios=[1,1.15],height_ratios=[1,1.4])
native=fig.add_subplot(grid[:,0]);native.imshow(plt.imread(ROOT/'native-preview.png'));native.set_axis_off()
native.set_title('整件前壳：外侧根部增加 R0.4 过渡',fontsize=13,pad=5)
for ax in [fig.add_subplot(grid[0,1]),fig.add_subplot(grid[1,1])]:
    for polys,color,label,style in [(old,'#8e969e','原直壁','--'),(new,'#008c70','圆角过渡','-')]:
        for i,p in enumerate(polys):
            p=np.vstack([p,p[0]]);x=p[:,0]-29.5;y=p[:,1]
            if style=='-':ax.fill(x,y,color=color,alpha=.14)
            ax.plot(x,y,color=color,ls=style,lw=1.8,label=label if i==0 else None)
    ax.grid(alpha=.18);ax.set_aspect('equal');ax.set_xlabel('距摇杆孔中心 / mm');ax.set_ylabel('向壳外高度 / mm')
top,bottom=fig.axes[1:]
top.set(xlim=(-10,10),ylim=(-1.3,3.5),title='穿过孔中心的实际 STL 剖面')
top.text(0,.2,'孔口及内侧净空保留',ha='center',fontsize=10)
top.legend(loc='upper center',ncol=2,fontsize=9)
bottom.set(xlim=(8.8,9.65),ylim=(-.12,.6),title='右侧根部放大：随原壳面衔接')
bottom.annotate('R0.4 mm',xy=(9.12,.12),xytext=(9.35,.45),fontsize=12,
                arrowprops={'arrowstyle':'->','color':'#444'})
fig.suptitle('摇杆凸台根部圆角：外侧增料，内部避让空间保留',fontsize=16,y=.97)
fig.text(.5,.025,'新增过渡最大外扩 0.4 mm；靠按键开口侧名义间隔约 0.4 mm。模型尚未实物试装。',ha='center',fontsize=10,color='#555')
fig.tight_layout(rect=(0,.06,1,.93),h_pad=2)
fig.savefig(ROOT/'root-fillet-preview.png',dpi=160)
plt.close(fig)
print('Unsliced project and preview complete',flush=True)

out=ROOT/'slice-check';out.mkdir(exist_ok=True)
run=subprocess.run(common+['--slice','0','--export-3mf','slice-check.3mf','--outputdir',str(out),model],
                   capture_output=True,text=True,encoding='utf8',errors='replace',timeout=180)
log=run.stdout+'\n'+run.stderr
(out/'slicer.log').write_text(log,encoding='utf8')
info={'project_unsliced':True,'printer_model':'Bambu Lab A1','slice_return_code':run.returncode,
      'internal_error_log':'[error]' in log}
if (out/'result.json').exists():
    result=json.loads((out/'result.json').read_text())
    if result.get('sliced_plates'):
        p=result['sliced_plates'][0]
        info.update({'estimated_minutes':round(p['total_predication']/60),
                     'estimated_grams':p['filaments'][0]['total_used_g'],'model_count':len(p['objects'])})
(ROOT/'delivery-check.json').write_text(json.dumps(info,indent=2),encoding='utf8')
assert run.returncode==0 and not info['internal_error_log'],info
for file in out.iterdir():
    if file.suffix in ['.gcode','.3mf']:file.unlink()
print(json.dumps(info,indent=2),flush=True)
