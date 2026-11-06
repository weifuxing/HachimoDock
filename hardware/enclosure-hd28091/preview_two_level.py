"""Chinese two-level support and dual-FPC review views from exported CAD."""
from pathlib import Path
import json
import FreeCAD as App
import Part
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib import font_manager
from matplotlib.patches import Rectangle,FancyBboxPatch
import numpy as np

ROOT=Path(__file__).resolve().parent
report=json.loads((ROOT/"adaptation-report.json").read_text(encoding="utf-8"))
params=report["design_allowances"]
cx,cy=params["active_center_local"]
font_manager.fontManager.addfont(r"C:\Windows\Fonts\msyh.ttc")
plt.rcParams.update({"font.family":"Microsoft YaHei","axes.unicode_minus":False})

def load(file):
    shape=Part.Shape()
    shape.read(str(ROOT/file))
    shape.rotate(App.Vector(),App.Vector(1,0,0),45)
    return shape

body=load("HD28091-front-body.step")
screen=load("HD28091-screen-envelope-reference.step")

def lines(ax,shape,plane,color,axes=(0,1),lw=1.3):
    section=shape.section(plane)
    for edge in section.Edges:
        pts=np.array([tuple(p) for p in edge.discretize(Deflection=.015)])
        ax.plot(pts[:,axes[0]],pts[:,axes[1]],color=color,lw=lw)

fig,axs=plt.subplots(2,2,figsize=(14,10),facecolor="#f5f7fa",
                     gridspec_kw={"height_ratios":[1.4,1]})
ax=axs[0,0]
z=params["lcd_rear_seat_depth"]-.02
lines(ax,body,Part.makePlane(100,100,App.Vector(-50,0,z)),"#6c849d")
gw,gh=report["source_dimensions"]["tp_landscape"]
ax.add_patch(FancyBboxPatch((-gw/2,cy-gh/2),gw,gh,boxstyle="round,pad=0,rounding_size=2",fill=False,ec="#333333",lw=1.5))
ax.add_patch(Rectangle((-69.21/2,cy-25),69.21,50,fill=False,ec="#777777",ls="--",lw=1.3,label="LCD 模组外形"))
ax.add_patch(Rectangle((cx-28.8,cy-21.6),57.6,43.2,fill=False,ec="#3d9d8a",ls=":",lw=1.0,label="显示区投影（背贴避开）"))
for i,pad in enumerate(params["lcd_rear_pads"],1):
    x0,y0,x1,y1=pad["xy"]
    ax.add_patch(Rectangle((x0,y0),x1-x0,y1-y0,fc="#d06a8b",ec="#ad4667",alpha=.85))
    ax.text((x0+x1)/2,(y0+y1)/2,str(i),ha="center",va="center",fontsize=8,color="white")
for check,color in zip(report["checks"]["fpc_clearances"],["#e5a03a","#8b68c2"]):
    x0,y0,z0,x1,y1,z1=check["keepout_bounds_mm"]
    ax.add_patch(Rectangle((x0,y0),x1-x0,y1-y0,fc=color,ec=color,alpha=.25,lw=1.5))
    ax.annotate(check["name"]+" 排线避让",(36,(y0+y1)/2),(42,(y0+y1)/2+1),fontsize=9,arrowprops={"arrowstyle":"->","color":color})
ax.set(xlim=(-42,64),ylim=(4,62),xlabel="屏幕横向 / mm",ylabel="屏幕纵向 / mm")
ax.set_aspect("equal")
ax.grid(alpha=.15)
ax.legend(loc="center",bbox_to_anchor=(.40,.51),fontsize=8)
ax.set_title("内部布局（从正面透视）：4 段背贴避开两条排线")

plane=Part.makePlane(120,60,App.Vector(-60,cy+23.25,20),App.Vector(0,1,0),App.Vector(1,0,0))
def section(ax):
    lines(ax,body,plane,"#45627f",(0,2),1.6)
    lines(ax,screen,plane,"#d09036",(0,2),1.7)
    seat=params["lcd_rear_seat_depth"]
    for pad in params["lcd_rear_pads"]:
        x0,y0,x1,y1=pad["xy"]
        if y0<=cy+23.25<=y1:
            ax.add_patch(Rectangle((x0,seat),x1-x0,params["lcd_rear_adhesive_nominal_gap"],fc="#d06a8b",alpha=.75))
    ax.add_patch(Rectangle((-36.53,-.95),1.625,.25,fc="#d06a8b",alpha=.75))
    ax.axhline(0,color="#a0a0a0",ls="--",lw=.8)
    ax.grid(alpha=.15)

ax=axs[0,1]
section(ax)
ax.annotate("玻璃与面板齐平",(0,0),(0,5.5),ha="center",arrowprops={"arrowstyle":"->"},fontsize=10)
ax.annotate("第一级：玻璃背贴",(-35.7,-.95),(-26,-11),ha="center",arrowprops={"arrowstyle":"->"},fontsize=10)
ax.annotate("第二级：LCD 柔性背贴",(8,-4.13),(15,-11),ha="center",arrowprops={"arrowstyle":"->"},fontsize=10)
ax.set(xlim=(-46,46),ylim=(-17,9),xlabel="屏幕横向 / mm",ylabel="相对面板深度 / mm")
ax.set_aspect("equal")
ax.set_title("两级台阶剖面：外壳蓝色、屏幕橙色、胶层粉色")

ax=axs[1,0]
section(ax)
ax.annotate("玻璃厚 0.70 mm",(-35.8,-.35),(-33,.6),fontsize=10,arrowprops={"arrowstyle":"->"})
ax.annotate("玻璃胶层 0.25 mm",(-35.8,-.825),(-39.5,-2),fontsize=10,arrowprops={"arrowstyle":"->"})
ax.annotate("第一级台阶深度 0.95 mm",(-35.8,-.95),(-35.5,-4),fontsize=10,arrowprops={"arrowstyle":"->"})
ax.set(xlim=(-40,-30),ylim=(-5,1.4),xlabel="屏幕横向 / mm",ylabel="相对面板深度 / mm")
ax.set_aspect("equal")
ax.set_title("玻璃端部局部放大")

ax=axs[1,1]
section(ax)
ax.annotate("LCD 背面名义深度 3.53 mm",(-28.5,-3.53),(-30,-1.2),fontsize=10,arrowprops={"arrowstyle":"->"})
ax.annotate("柔性背贴名义间隙 0.60 mm",(-18,-3.83),(-23,.5),fontsize=10,arrowprops={"arrowstyle":"->"})
ax.annotate("第二级台阶深度 4.13 mm",(-24,-4.13),(-30,-6),fontsize=10,arrowprops={"arrowstyle":"->"})
ax.set(xlim=(-31,-8),ylim=(-7,1.3),xlabel="屏幕横向 / mm",ylabel="相对面板深度 / mm")
ax.set_aspect("equal")
ax.set_title("LCD 背贴局部放大：前端厚 2 mm，背面斜撑加厚")

fig.suptitle("两级台阶＋分段柔性背贴：斜撑与壳体圆角顺接",fontsize=17,y=.98)
fig.text(.5,.015,"背贴建议区：4 ×（16 × 2.5 mm）；背面允许贴装区域及胶材须实物核实。排线色块为避让空间，并非完整排线模型。",ha="center",fontsize=10,color="#555555")
fig.tight_layout(rect=(0,.04,1,.95),h_pad=2.0,w_pad=2.0)
fig.savefig(ROOT/"two-level-mount-preview.png",dpi=160,facecolor=fig.get_facecolor(),bbox_inches="tight")
plt.close(fig)
print(ROOT/"two-level-mount-preview.png")
