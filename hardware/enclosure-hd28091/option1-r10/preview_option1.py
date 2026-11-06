"""Actual STL and native project comparison for option 1."""
from pathlib import Path
import sys,json,math
ROOT=Path(__file__).resolve().parent
sys.path.insert(0,str(ROOT/".cadlibs"))
import manifold3d as mf
import numpy as np
import FreeCAD,Mesh
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib import font_manager
font_manager.fontManager.addfont(r"C:\Windows\Fonts\msyh.ttc")
plt.rcParams.update({"font.family":"Microsoft YaHei","axes.unicode_minus":False})

def section(file):
    v,t=Mesh.Mesh(str(file)).Topology
    angle=math.pi/4;c,s=math.cos(angle),math.sin(angle)
    matrix=np.array([[1,0,0],[0,c,-s],[0,s,c]])
    v=np.asarray([tuple(p) for p in v])@matrix.T
    m=mf.Manifold(mf.Mesh64(v,np.array(t,dtype=np.uint64)))
    return m.slice(-.001).to_polygons()

def area(p):
    return .5*np.sum(p[:,0]*np.roll(p[:,1],-1)-p[:,1]*np.roll(p[:,0],-1))

def region_segments(p):
    a=p;b=np.roll(p,-1,axis=0)
    mask=(a[:,0]>29)&(a[:,1]>51)&(b[:,0]>29)&(b[:,1]>51)
    return a[mask],b[mask]

def distance(points,a,b):
    ab=b-a
    t=np.clip(np.sum((points[:,None,:]-a[None,:,:])*ab[None,:,:],axis=2)/np.maximum(np.sum(ab*ab,axis=1),1e-15),0,1)
    d=np.linalg.norm(points[:,None,:]-(a[None,:,:]+t[:,:,None]*ab[None,:,:]),axis=2)
    return float(d.min())

old=section(ROOT.parent/"HD28091-front-body.stl")
new=section(ROOT/"HD28091-option1-R10.stl")
bands=[]
for polys in [old,new]:
    outer=max(polys,key=area);inner=min(polys,key=area)
    a,b=region_segments(outer);u,v=region_segments(inner)
    bands.append(min(distance(u,a,b),distance(a,u,v)))

fig,axes=plt.subplots(2,2,figsize=(12,10),facecolor="#f5f7fa")
for ax,img,title in [
    (axes[0,0],ROOT.parent/"fdm"/"trimmed-ears-native.png","改前：已试装通过的基准模型"),
    (axes[0,1],ROOT/"option1-native-preview.png","方案一：四处机身外角统一 R10")]:
    ax.imshow(plt.imread(img));ax.set_axis_off();ax.set_title(title,fontsize=14,pad=-5)
for ax,polys,title,width in [(axes[1,0],old,"改前：面框上角约 R7.5",bands[0]),
                            (axes[1,1],new,"改后：面框边缘约 R4.5",bands[1])]:
    outer=max(polys,key=area);inner=min(polys,key=area)
    for p,c,label in [(outer,"#647c95","前框外缘"),(inner,"#37a17a","已试装开口，保持原设计")]:
        p=np.vstack([p,p[0]]);ax.plot(p[:,0],p[:,1],color=c,lw=2,label=label)
    ax.set(xlim=(29,40),ylim=(50.5,61),xlabel="横向 / mm",ylabel="纵向 / mm")
    ax.set_aspect("equal");ax.grid(alpha=.15);ax.legend(loc="lower left",fontsize=9)
    ax.set_title(title+f"\n上角平面窄边约 {width:.2f} mm",fontsize=13)
fig.suptitle("方案一实模对比：整体适度变方，屏幕配合与安装结构保留",fontsize=16,y=.98)
fig.text(.5,.015,"下排轮廓来自实际 STL 在前表面后方 0.001 mm 的截面；宽度为该截面的近似测量。新版本单独输出，基准文件保留。",ha="center",fontsize=10,color="#555555")
fig.tight_layout(rect=(0,.06,1,.94),h_pad=2,w_pad=2)
fig.savefig(ROOT/"option1-before-after.png",dpi=160,facecolor=fig.get_facecolor(),bbox_inches="tight")
plt.close(fig)
p=ROOT/"option1-report.json";data=json.loads(p.read_text());data["front_corner_band_from_mesh_section_mm"]={"before":bands[0],"after":bands[1],"plane_depth_mm":-.001};p.write_text(json.dumps(data,indent=2),encoding="utf8")
print(bands)
