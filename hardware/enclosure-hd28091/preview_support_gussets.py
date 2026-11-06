"""Render the actual longitudinal CAD sections of the reinforced supports."""
from pathlib import Path
import json
import math
import FreeCAD as App
import Part
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib import font_manager
from matplotlib.patches import Rectangle
import numpy as np

ROOT=Path(__file__).resolve().parent
report=json.loads((ROOT/"adaptation-report.json").read_text(encoding="utf-8"))
p=report["design_allowances"]
font_manager.fontManager.addfont(r"C:\Windows\Fonts\msyh.ttc")
plt.rcParams.update({"font.family":"Microsoft YaHei","axes.unicode_minus":False})

def load(file):
    shape=Part.Shape()
    shape.read(str(ROOT/file))
    shape.rotate(App.Vector(),App.Vector(1,0,0),45)
    return shape

body=load("HD28091-front-body.step")
screen=load("HD28091-screen-envelope-reference.step")
plane=Part.makePlane(80,60,App.Vector(-19,0,-40),App.Vector(1,0,0),App.Vector(0,1,0))

fig,axes=plt.subplots(1,2,figsize=(12,7),facecolor="#f5f7fa")
for ax,which in zip(axes,["upper_left","lower_left"]):
    spec=next(s for s in p["lcd_support_underside_gussets"] if s["name"]==which)
    for shape,color in [(body,"#45627f"),(screen,"#c58c30")]:
        for edge in shape.section(plane).Edges:
            pts=np.array([tuple(v) for v in edge.discretize(Deflection=.01)])
            ax.plot(pts[:,1],pts[:,2],color=color,lw=2)
    pad=next(s for s in p["lcd_rear_pads"] if s["name"]==which)
    x0,y0,x1,y1=pad["xy"]
    seat=p["lcd_rear_seat_depth"]
    ax.add_patch(Rectangle((y0,seat),y1-y0,p["lcd_rear_adhesive_nominal_gap"],fc="#d06a8b",alpha=.75))
    tip,root=spec["tip_y_mm"],spec["root_y_mm"]
    old_back=seat-spec["tip_thickness_mm"]
    ax.plot([tip,root],[old_back,old_back],ls="--",lw=1.2,color="#999999",label="原矩形块背面")
    upper=which.startswith("upper")
    point_y=(tip+root)/2
    point_z=old_back-abs(point_y-tip)*math.tan(math.radians(spec["slope_angle_deg"]))
    ax.annotate(f"连续斜撑约 {spec['slope_angle_deg']:.0f}°",(point_y,point_z),
        (tip+.2 if upper else root-.7,-12.5),fontsize=11,arrowprops={"arrowstyle":"->"})
    ax.annotate("R0.8 圆角顺接",(root,-8.47),
        (root+1 if upper else 2.5,-10.7),fontsize=10,arrowprops={"arrowstyle":"->"})
    if not upper:
        ax.plot([3.6480945,5.9,5.9,11.2],[-8.5,-8.5,-11.43,-6.13],
                ls="--",color="#ba8282",lw=1.1,label="上一版根部轮廓")
    ax.annotate("贴胶面保持水平",((y0+y1)/2,seat),
        (tip-1 if upper else root-.7,-1.5),fontsize=10,arrowprops={"arrowstyle":"->"})
    ax.annotate("前端保留 2 mm",(tip,seat-1),
        (tip-3.6 if upper else tip-.9,-7.5),fontsize=10,arrowprops={"arrowstyle":"->"})
    ax.axhline(0,color="#bbbbbb",ls=":",lw=.8)
    lo,hi=(50.0,64.0) if upper else (1.8,15.8)
    ax.set(xlim=(lo,hi),ylim=(-14,1.2),xlabel="屏幕纵向 / mm",ylabel="相对面板深度 / mm")
    ax.set_title(("上侧两处支撑" if upper else "下侧两处支撑")+f"：设计根部厚 {spec['root_design_thickness_mm']:.2f} mm")
    ax.set_aspect("equal")
    ax.grid(alpha=.15)
    ax.legend(loc="lower right",fontsize=9)

fig.suptitle("根部修正：斜撑在原底面收口，以 R0.8 圆角连续顺接",fontsize=16,y=.98)
fig.text(.5,.025,"蓝色：修正后的真实外壳剖面；橙色：屏幕包络；粉色：柔性胶层。右图暗红虚线为上一版多余尖角的位置。",ha="center",fontsize=10,color="#555555")
fig.tight_layout(rect=(0,.10,1,.95),w_pad=2)
fig.savefig(ROOT/"gusset-support-preview.png",dpi=160,facecolor=fig.get_facecolor(),bbox_inches="tight")
plt.close(fig)
print(ROOT/"gusset-support-preview.png")
