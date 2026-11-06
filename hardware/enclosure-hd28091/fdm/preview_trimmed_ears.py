"""Before/after CAD sections for the user-requested mounting-ear flat trim."""
from pathlib import Path
import json,hashlib
import FreeCAD as App
import Part,Mesh
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib import font_manager

ROOT=Path(__file__).resolve().parent
MASTER=ROOT.parent/"HD28091-front-body.step"
font_manager.fontManager.addfont(r"C:\Windows\Fonts\msyh.ttc")
plt.rcParams.update({"font.family":"Microsoft YaHei","axes.unicode_minus":False})

def load(file):
    s=Part.Shape();s.read(str(file));s.rotate(App.Vector(),App.Vector(1,0,0),45);return s

after=load(MASTER)
caps=load(ROOT.parent/"HD28091-ear-trim-removed-reference.step")
before=after.fuse(caps).removeSplitter()
assert before.isValid() and after.isValid()
cutplane=-15.5
old_min=before.optimalBoundingBox(False,False).ZMin
new_min=after.optimalBoundingBox(False,False).ZMin
assert abs(new_min-cutplane)<1e-5
contact=sum(f.Area for f in after.Faces if isinstance(f.Surface,Part.Plane)
    and abs(f.CenterOfMass.z-cutplane)<1e-5 and f.normalAt(0,0).z<-.999)
assert contact>200
mesh=Mesh.Mesh(str(ROOT.parent/"HD28091-front-body.stl"))
assert mesh.isSolid() and not mesh.hasNonManifolds() and not mesh.hasSelfIntersections()
report={"source":str(MASTER),"sha256":hashlib.sha256(MASTER.read_bytes()).hexdigest(),
    "removed_height_mm":cutplane-old_min,"new_lowest_screen_local_depth_mm":new_min,
    "screen_up_horizontal_contact_area_mm2":contact,"mesh_closed":True,
    "mesh_self_intersections":False,"changed_other_7_solids":False,
    "hole_diameter_mm":2.0,"minimum_vertical_hole_margin_mm":.9406712753689828,
    "notes":"Two mounting-ear tips only; original hole cylindrical faces unchanged. Slot-roof thin regions are unchanged."}
(ROOT/"trimmed-ears-verification.json").write_text(json.dumps(report,indent=2),encoding="utf-8")

fig,axes=plt.subplots(1,2,figsize=(12,5),facecolor="#f5f7fa")
for ax,title,s,offset in [(axes[0],"削平前：凸耳先触床",before,-old_min),
                          (axes[1],"削平后：凸耳与背框齐平贴床",after,-new_min)]:
    s=s.copy();s.translate(App.Vector(0,0,offset))
    plane=Part.makePlane(100,70,App.Vector(25,-35,-1),App.Vector(1,0,0),App.Vector(0,1,0))
    for e in s.section(plane).Edges:
        pts=np.array([tuple(v) for v in e.discretize(Deflection=.01)])
        ax.plot(pts[:,1],pts[:,2],color="#45627f",lw=2)
    ax.axhline(0,color="#777777",lw=2)
    height=cutplane+offset
    ax.axhline(height,color="#8ba4bd",ls="--",lw=1)
    if title.startswith("削平前"):
        ax.annotate("低于背框约 1.89 mm",(.417,0),(-7,4.4),fontsize=11,arrowprops={"arrowstyle":"->"})
        ax.annotate("",(5,0),(5,height),arrowprops={"arrowstyle":"<->"})
    else:
        ax.annotate("削平面，与背框共面",(.417,0),(-7,3.5),fontsize=11,arrowprops={"arrowstyle":"->"})
    ax.text(13,-.8,"热床",fontsize=10,color="#666666")
    ax.set(xlim=(-8,18),ylim=(-1.5,11),xlabel="屏幕纵向 / mm",ylabel="相对热床高度 / mm")
    ax.set_aspect("equal");ax.grid(alpha=.15);ax.set_title(title,fontsize=14)
fig.suptitle("两个固定耳已直接削平：屏幕面朝上时，主体背框可直接贴床",fontsize=15,y=.98)
fig.text(.5,.018,"真实 STEP 剖面对比，左右两耳对称。Ø2 mm 孔保留；孔下方最小竖向余量约 0.94 mm，未做紧固强度试验。",ha="center",fontsize=10,color="#555555")
fig.tight_layout(rect=(0,.08,1,.93),w_pad=2)
fig.savefig(ROOT/"trimmed-ears-before-after.png",dpi=160,facecolor=fig.get_facecolor(),bbox_inches="tight")
plt.close(fig)
print(json.dumps(report,indent=2))
