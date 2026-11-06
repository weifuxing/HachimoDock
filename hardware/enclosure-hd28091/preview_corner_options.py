"""Concept-only upper-corner comparison; does not edit manufacturing geometry."""
from pathlib import Path
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib import font_manager
from matplotlib.patches import FancyBboxPatch

ROOT=Path(__file__).resolve().parent
font_manager.fontManager.addfont(r"C:\Windows\Fonts\msyh.ttc")
plt.rcParams.update({"font.family":"Microsoft YaHei","axes.unicode_minus":False})

def rr(ax,x,y,w,h,r,c,lw=2,ls="-",label=None):
    ax.add_patch(FancyBboxPatch((x,y),w,h,boxstyle=f"round,pad=0,rounding_size={r}",
        fill=False,edgecolor=c,linewidth=lw,linestyle=ls,label=label))

fig,axes=plt.subplots(1,2,figsize=(12,6),facecolor="#f5f7fa")
for ax in axes:
    # Relevant upper-corner envelopes, using measured existing extents.
    # Only upper corners are shown; this is not a complete lower-shell design.
    rr(ax,-44.25,-31.65,88.5,96.9967,13,"#a5adb7",1.5,"--","原机身外角轮廓")
    rr(ax,-38.75,6.2132,77.5,53.6335,7.5,"#c1c7ce",1.3,"--","原前面框边界")
    rr(ax,-36.83,7.13,73.66,51.14,2.3,"#37a17a",2,"-","已试装的屏幕开口（保留）")
    ax.set(xlim=(29,46),ylim=(50,68),xlabel="横向 / mm",ylabel="纵向 / mm")
    ax.set_aspect("equal");ax.grid(alpha=.15)

rr(axes[0],-44.25,-31.65,88.5,96.9967,10,"#287eae",2.4,"-","机身外角也适度减小")
rr(axes[0],-38.75,6.2132,77.5,53.6335,4.5,"#287eae",2.4)
axes[0].set_title("方案一：机身和面框一起协调",fontsize=14)
axes[0].annotate("机身外侧也变得更方",(41.32,62.42),(29.6,66.8),fontsize=10,arrowprops={"arrowstyle":"->"})
axes[0].annotate("面框四角同步协调",(37.4,58.5),(29.6,61.5),fontsize=10,arrowprops={"arrowstyle":"->"})

rr(axes[1],-44.25,-31.65,88.5,96.9967,13,"#77879a",2.4,"-","机身外轮廓保留")
rr(axes[1],-38.75,6.2132,77.5,53.6335,4.5,"#287eae",2.4,"-","局部面框／过渡调整")
axes[1].set_title("方案二：只调整屏幕周围的面框",fontsize=14)
axes[1].annotate("机身仍保留原来的圆润外形",(40.44,61.54),(29.4,66.8),fontsize=10,arrowprops={"arrowstyle":"->"})
axes[1].annotate("局部做更协调的面框边界",(37.4,58.5),(29.4,61.5),fontsize=10,arrowprops={"arrowstyle":"->"})
for ax in axes:ax.legend(loc="lower left",fontsize=8)
fig.suptitle("两种改法的差别：是否一起改变机身外侧的大圆角",fontsize=16,y=.98)
fig.text(.5,.018,"上侧右角的方案示意；蓝色为拟调整轮廓，灰色为原轮廓。半径仅用于说明差异，尚未修改 STEP 或已试装的配合结构。",ha="center",fontsize=10,color="#555555")
fig.tight_layout(rect=(0,.07,1,.93),w_pad=2)
fig.savefig(ROOT/"corner-options-comparison.png",dpi=160,facecolor=fig.get_facecolor(),bbox_inches="tight")
plt.close(fig)
print(ROOT/"corner-options-comparison.png")
