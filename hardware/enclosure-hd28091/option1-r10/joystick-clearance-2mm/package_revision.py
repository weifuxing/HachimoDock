"""Export the unsliced A1 project and an actual-mesh Chinese section view."""
from pathlib import Path
import subprocess, zipfile, json, shutil, sys
ROOT=Path(__file__).resolve().parent
PREV=ROOT.parent
NAME='HD28091-R10-joystick-plus2mm'
for file in ['a1-machine.json','a1-process.json','generic-abs.json']:
    shutil.copy2(PREV/file, ROOT/file)
exe=r'C:\Program Files\Bambu Studio\bambu-studio.exe'
args=[exe,'--load-settings',str(ROOT/'a1-machine.json')+';'+str(ROOT/'a1-process.json'),
      '--load-filaments',str(ROOT/'generic-abs.json'),'--orient','0','--arrange','1','--ensure-on-bed',
      '--export-3mf',NAME+'-screen-up.3mf','--outputdir',str(ROOT),str(ROOT/(NAME+'-screen-up.stl'))]
run=subprocess.run(args,capture_output=True,text=True,encoding='utf8',errors='replace',timeout=120)
(ROOT/'project-export.log').write_text(run.stdout+'\n'+run.stderr,encoding='utf8')
assert run.returncode==0
with zipfile.ZipFile(ROOT/(NAME+'-screen-up.3mf')) as z:
    assert z.testzip() is None and not any(n.endswith('.gcode') for n in z.namelist())
    assert json.loads(z.read('Metadata/project_settings.config'))['printer_model']=='Bambu Lab A1'
    (ROOT/'native-preview.png').write_bytes(z.read('Metadata/plate_1.png'))

sys.path.insert(0,str(PREV/'.cadlibs'))
import manifold3d as mf
import numpy as np
import FreeCAD, Mesh
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib import font_manager
font_manager.fontManager.addfont(r'C:\Windows\Fonts\msyh.ttc')
plt.rcParams.update({'font.family':'Microsoft YaHei','axes.unicode_minus':False})

def section(file):
    v,t=Mesh.Mesh(str(file)).Topology
    s=mf.Manifold(mf.Mesh64(np.array([tuple(p) for p in v]),np.array(t,dtype=np.uint64)))
    return s.slice(15.75).to_polygons()
fig,axes=plt.subplots(1,2,figsize=(13,6.5),gridspec_kw={'width_ratios':[1,1.15]})
axes[0].imshow(plt.imread(ROOT/'native-preview.png'))
axes[0].set_axis_off();axes[0].set_title('新版前壳：摇杆圆环向壳外抬高',fontsize=14)
ax=axes[1]
for file,color,label,style in [(PREV/'HD28091-option1-R10.stl','#8c959d','修改前','--'),
                              (ROOT/(NAME+'.stl'),'#008c70','修改后','-')]:
    for i,p in enumerate(section(file)):
        p=np.vstack([p,p[0]])
        if style=='-':ax.fill(p[:,0]-29.5,p[:,1],color=color,alpha=.12)
        ax.plot(p[:,0]-29.5,p[:,1],color=color,ls=style,lw=1.5,label=label if i==0 else None)
ax.axvline(0,color='#a6abb0',ls=':',lw=1)
ax.annotate('',xy=(6,1.3926),xytext=(6,-.6074),arrowprops={'arrowstyle':'<->','color':'#d95524','lw':2})
ax.text(5.5,.35,'净空增加\n2.00 mm',ha='right',color='#ba4820',fontsize=12)
ax.annotate('连接侧壁厚 0.8 mm',xy=(8.6,.5),xytext=(9.7,2.8),ha='right',
            arrowprops={'arrowstyle':'->','color':'#444'},fontsize=10)
ax.text(0,3.5,'壳外 ↑',ha='center',fontsize=11)
ax.text(0,-1.7,'壳内 ↓\n摇杆孔中心不动',ha='center',fontsize=10)
ax.set(xlim=(-10.5,10.5),ylim=(-2.2,4),xlabel='距摇杆孔中心 / mm',ylabel='沿孔轴方向 / mm')
ax.set_aspect('equal');ax.grid(alpha=.18);ax.legend(loc='upper left');ax.set_title('穿过孔中心的实际 STL 剖面',fontsize=14)
fig.suptitle('摇杆避让修改：圆环及原孔口整体外移 2 mm',fontsize=17,y=.97)
fig.text(.5,.04,'屏幕安装结构、R10 外观和固定孔保留；新版尚未实物试装。',ha='center',fontsize=11,color='#555')
fig.tight_layout(rect=(0,.09,1,.92))
fig.savefig(ROOT/'joystick-change-preview.png',dpi=160)
plt.close(fig)
print('Unsliced 3MF and section preview exported',flush=True)
