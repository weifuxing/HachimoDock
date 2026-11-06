"""Compare original and current upper-right corner contours without edits."""
from pathlib import Path
import FreeCAD as App
import Part
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib import font_manager

ROOT=Path(__file__).resolve().parent
font_manager.fontManager.addfont(r"C:\Windows\Fonts\msyh.ttc")
plt.rcParams.update({"font.family":"Microsoft YaHei","axes.unicode_minus":False})
old=Part.Shape();old.read(r"C:\Users\fuxing\Downloads\桌搭-整机外壳3d打印版.STEP")
old=old.Solids[0].copy();old.rotate(App.Vector(),App.Vector(1,0,0),45)
face=old.Faces[42]
outer=face.OuterWire
old_hole=next(w for w in face.Wires if not w.isSame(outer))
glass=Part.Shape();glass.read(str(ROOT/"HD28091-screen-envelope-reference.step"))
glass.rotate(App.Vector(),App.Vector(1,0,0),45)
glass_face=next(f for f in glass.Faces if isinstance(f.Surface,Part.Plane)
    and abs(f.CenterOfMass.z)<1e-5 and f.normalAt(0,0).z>.99)
p=Part.makeBox(73.66,51.14,.1,App.Vector(-36.83,7.13,0))
p=p.makeFillet(2.3,[e for e in p.Edges if e.BoundBox.XLength<1e-7 and e.BoundBox.YLength<1e-7])
opening=next(f for f in p.Faces if isinstance(f.Surface,Part.Plane) and abs(f.CenterOfMass.z)<1e-6)

def contour(ax,wire,color,label):
    for edge in wire.Edges:
        pts=np.array([tuple(p) for p in edge.discretize(Deflection=.008)])
        ax.plot(pts[:,0],pts[:,1],color=color,lw=2)
    ax.plot([],[],color=color,lw=2,label=label)

fig,axes=plt.subplots(1,2,figsize=(12,6),facecolor="#f5f7fa")
for ax in axes:
    contour(ax,outer,"#77879a","原前面框外缘（上角约 R7.5）")
    ax.set(xlim=(28,40),ylim=(49.5,61),xlabel="横向 / mm",ylabel="纵向 / mm")
    ax.set_aspect("equal");ax.grid(alpha=.15)
contour(axes[0],old_hole,"#bd873c","原屏幕开口 R7.1")
axes[0].set_title("原设计：开口与面框均为较大的圆角")
contour(axes[1],opening.OuterWire,"#2687b8","新开口 R2.3")
contour(axes[1],glass_face.OuterWire,"#37a17a","新玻璃 R2.0")
axes[1].set_title("换屏后：开口已改，面框外缘沿用原形")
axes[1].annotate("最窄平面边框约 0.31 mm",(36.18,57.78),(28.5,60.2),
                 fontsize=10,arrowprops={"arrowstyle":"->"})
for ax in axes:ax.legend(loc="lower left",fontsize=9)
fig.suptitle("圆角核对：当前配合间隙正确，但前面框四角的比例需要协调",fontsize=16,y=.98)
fig.text(.5,.025,"图示为上侧右角的真实 CAD 轮廓。原屏幕玻璃未包含在原 STEP 中，原开口 R7.1 不能直接当作原玻璃的 R 值。",ha="center",fontsize=10,color="#555555")
fig.tight_layout(rect=(0,.07,1,.93),w_pad=2)
fig.savefig(ROOT/"radius-review-comparison.png",dpi=160,facecolor=fig.get_facecolor(),bbox_inches="tight")
plt.close(fig)
print(ROOT/"radius-review-comparison.png")
