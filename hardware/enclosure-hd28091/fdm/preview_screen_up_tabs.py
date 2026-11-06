"""Explain why the two existing mounting ears lift the screen-up body."""
from pathlib import Path
import json,zipfile
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
s=Part.Shape();s.read(str(ROOT.parent/"HD28091-front-body.step"))
removed_reference=ROOT.parent/"HD28091-ear-trim-removed-reference.step"
if removed_reference.exists():
    removed=Part.Shape();removed.read(str(removed_reference))
    s=s.fuse(removed).removeSplitter()
s.rotate(App.Vector(),App.Vector(1,0,0),45)
bottom=s.optimalBoundingBox(False,False).ZMin
s.translate(App.Vector(0,0,-bottom))
frame_height=-15.5-bottom
plane=Part.makePlane(100,70,App.Vector(25,-35,-1),App.Vector(1,0,0),App.Vector(0,1,0))
low=s.common(Part.makeBox(200,200,frame_height,App.Vector(-100,-70,0)))
native=ROOT/"HD28091-A1-ABS-screen-up-raft-review-native.png"

fig,axes=plt.subplots(1,2,figsize=(12,5),facecolor="#f5f7fa",gridspec_kw={"width_ratios":[1,1.3]})
ax=axes[0];ax.imshow(plt.imread(native));ax.set_axis_off()
ax.set_title("屏幕面水平朝上，按键面倾斜",fontsize=13)
ax.text(.5,-.02,"主体已有平底，两个固定耳先触床",transform=ax.transAxes,ha="center",fontsize=11)
ax=axes[1]
for shape,color,lw in [(s,"#45627f",1.8),(low,"#c65565",2.6)]:
    for e in shape.section(plane).Edges:
        pts=np.array([tuple(v) for v in e.discretize(Deflection=.01)])
        ax.plot(pts[:,1],pts[:,2],color=color,lw=lw)
ax.axhline(0,color="#777777",lw=2)
ax.axhline(frame_height,color="#6b91b4",ls="--",lw=1)
ax.annotate("主体背框平面",(14,frame_height),(11,7),fontsize=11,arrowprops={"arrowstyle":"->"})
ax.annotate("固定耳底端先碰床",(.417,0),(-7,4),fontsize=11,arrowprops={"arrowstyle":"->"})
ax.annotate("",(5,0),(5,frame_height),arrowprops={"arrowstyle":"<->"})
ax.text(5.5,frame_height*.25,f"抬高约 {frame_height:.2f} mm",fontsize=11)
ax.text(16,-.8,"热床",fontsize=11,color="#555555")
ax.set(xlim=(-8,22),ylim=(-1.5,14),xlabel="屏幕纵向 / mm",ylabel="相对热床高度 / mm")
ax.set_aspect("equal");ax.grid(alpha=.15)
ax.set_title("右侧固定耳剖面：红色是低于主体平底的部分",fontsize=12)
fig.suptitle("削平前的抬高原因：两个带孔固定耳，比主体背框低约 1.89 mm",fontsize=15,y=.98)
fig.text(.5,.015,"本图为削平前的历史状态，由当前 STEP 与削除参考体复原；当前削平结果见 trimmed-ears-before-after.png。",ha="center",fontsize=10,color="#555555")
fig.tight_layout(rect=(0,.06,1,.94),w_pad=2)
fig.savefig(ROOT/"screen-up-tabs-explanation.png",dpi=160,facecolor=fig.get_facecolor(),bbox_inches="tight")
plt.close(fig)

report={"screen_up_frame_height_above_ear_low_point_mm":frame_height,
    "ear_ranges_x_mm":[[-30,-20],[20,30]],"frame_plane_local_depth_mm":-15.5,
    "ear_lowest_local_depth_mm":bottom,"ear_hole_diameter_mm":2,
    "minimum_cylinder_point_height_after_ear_alignment_mm":2.82842712474619,
    "vertical_hole_margin_if_trimmed_to_frame_plane_mm":2.82842712474619-frame_height,
    "interpretation":"The body has a flat rear frame parallel to the screen. Two original mounting ears project about 1.89 mm below it in the screen-up pose. Earlier general explanation based only on the rear mating-plane angle was incomplete.",
    "limitations":"No ear trimming was performed. Hole edge thickness and fastening performance would need review before changing these ears."}
(ROOT/"screen-up-tabs-analysis.json").write_text(json.dumps(report,indent=2),encoding="utf-8")
print(json.dumps(report,indent=2))
