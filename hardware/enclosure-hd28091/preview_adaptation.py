"""Generate an engineering preview from the actual exported STEP geometry."""
from pathlib import Path
import json
import FreeCAD as App
import Part
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib import font_manager
from matplotlib.patches import Rectangle,FancyBboxPatch
from matplotlib.colors import to_rgb
import numpy as np

font_manager.fontManager.addfont(r"C:\Windows\Fonts\msyh.ttc")
plt.rcParams["font.family"]="Microsoft YaHei"
plt.rcParams["axes.unicode_minus"]=False

ROOT=Path(__file__).resolve().parent
report=json.loads((ROOT/"adaptation-report.json").read_text(encoding="utf-8"))

def load(path):
    shape=Part.Shape()
    shape.read(str(path))
    shape.rotate(App.Vector(),App.Vector(1,0,0),45)
    return shape

original=load(report["source"])
modified=load(ROOT/"HD28091-enclosure-assembly.step")
screen=load(ROOT/"HD28091-screen-envelope-reference.step")

def render(ax, shape, include_screen=False):
    triangles=[]
    colors=[]
    solids=list(shape.Solids)
    palette=["#a7b9d2","#7a899d"]+["#e6ae71"]*3+["#8cae9d","#dfa7a7","#8f7ba5"]
    if include_screen:
        solids+=list(screen.Solids)
        palette += ["#172332"]*len(screen.Solids)
        cx,cy=report["design_allowances"]["active_center_local"]
        z=report["design_allowances"]["glass_front_depth"]+0.002
        solids.append(Part.makeBox(57.6,43.2,0.001,App.Vector(cx-28.8,cy-21.6,z)))
        palette.append("#426986")
    light=np.array([-0.3,-0.4,1.0]);light/=np.linalg.norm(light)
    for solid,color in zip(solids,palette):
        vertices,faces=solid.tessellate(0.20)
        tri=np.array([[tuple(vertices[j]) for j in f] for f in faces])
        normal=np.cross(tri[:,1]-tri[:,0],tri[:,2]-tri[:,0])
        normal/=np.maximum(np.linalg.norm(normal,axis=1)[:,None],1e-12)
        brightness=0.65+0.35*np.maximum(normal@light,0)
        triangles.extend(tri)
        colors.extend(np.clip(np.array(to_rgb(color))[None,:]*brightness[:,None],0,1))
    # Pixel depth testing avoids painter-sort artefacts at the bezel aperture.
    camera=np.array([.25,-.45,1.0]);camera/=np.linalg.norm(camera)
    right=np.cross([0,1,0],camera);right/=np.linalg.norm(right)
    up=np.cross(camera,right)
    basis=np.array([right,up,camera])
    triangles=np.array(triangles)
    projected=triangles@basis.T
    n=1000
    projected[:,:,0]=(projected[:,:,0]+62)*n/124
    projected[:,:,1]=(76-projected[:,:,1])*n/124
    pixels=np.full((n,n,3),[245,247,250],dtype=np.uint8)
    depth=np.full((n,n),-np.inf)
    for triangle,color in zip(projected,colors):
        lo=np.maximum(np.floor(triangle[:,:2].min(axis=0)).astype(int),0)
        hi=np.minimum(np.ceil(triangle[:,:2].max(axis=0)).astype(int),n-1)
        if np.any(lo>hi): continue
        xx,yy=np.meshgrid(np.arange(lo[0],hi[0]+1)+.5,np.arange(lo[1],hi[1]+1)+.5)
        a,b,c=triangle
        den=(b[1]-c[1])*(a[0]-c[0])+(c[0]-b[0])*(a[1]-c[1])
        if abs(den)<1e-10: continue
        w0=((b[1]-c[1])*(xx-c[0])+(c[0]-b[0])*(yy-c[1]))/den
        w1=((c[1]-a[1])*(xx-c[0])+(a[0]-c[0])*(yy-c[1]))/den
        w2=1-w0-w1
        zz=w0*a[2]+w1*b[2]+w2*c[2]
        block=depth[lo[1]:hi[1]+1,lo[0]:hi[0]+1]
        mask=(w0>=-1e-8)&(w1>=-1e-8)&(w2>=-1e-8)&(zz>block)
        block[mask]=zz[mask]
        pixels[lo[1]:hi[1]+1,lo[0]:hi[0]+1][mask]=np.array(color)*255
    ax.imshow(pixels)
    ax.set_axis_off()

fig=plt.figure(figsize=(14,9),facecolor="#f5f7fa")
ax=fig.add_subplot(221)
render(ax,original)
ax.set_title("原始外壳",pad=-10)
ax=fig.add_subplot(222)
render(ax,modified,True)
ax.set_title("修改后：正面装入，玻璃与面板齐平",pad=-10)

ax=fig.add_subplot(223)
before=original.Solids[0].Faces[42]
for wire in before.Wires:
    for edge in wire.Edges:
        p=np.array([tuple(v) for v in edge.discretize(Deflection=0.04)])
        ax.plot(p[:,0],p[:,1],color="#9aa5b5",lw=1)
cx,cy=report["design_allowances"]["active_center_local"]
ow,oh=report["design_allowances"]["glass_opening"]
gr=report["source_dimensions"]["glass_corner_radius"]
pr=report["design_allowances"]["glass_opening_corner_radius"]
for x,y,w,h,r,c,label in [(-ow/2,cy-oh/2,ow,oh,pr,"#2582ac",f"安装口 {ow:.2f} × {oh:.2f} mm，R{pr:.2f}"),
                        (-73.06/2,cy-50.54/2,73.06,50.54,gr,"#888888",f"触摸玻璃 73.06 × 50.54 mm，R{gr:.2f}"),
                        (cx-57.6/2,cy-43.2/2,57.6,43.2,0,"#35a476","显示区 57.60 × 43.20 mm")]:
    patch=FancyBboxPatch((x,y),w,h,boxstyle=f"round,pad=0,rounding_size={r}",fill=False,ec=c,lw=1.5,label=label) if r else Rectangle((x,y),w,h,fill=False,ec=c,lw=1.5,label=label)
    ax.add_patch(patch)
ax.annotate("LCD 排线",(37,cy),(42,cy+3),arrowprops={"arrowstyle":"->"},fontsize=9)
ax.annotate("TP 排线",(37,cy-12.25),(42,cy-15.5),arrowprops={"arrowstyle":"->"},fontsize=9)
ax.set(xlim=(-42,57),ylim=(3,63),xlabel="屏幕横向 / mm",ylabel="屏幕纵向 / mm")
ax.set_aspect("equal")
ax.grid(alpha=.2)
ax.legend(loc="lower center",bbox_to_anchor=(.5,-.43),fontsize=8)
ax.set_title("正面尺寸：玻璃四周单边留隙 0.30 mm")

ax=fig.add_subplot(224)
body=modified.Solids[0]
section_plane=Part.makePlane(120,60,App.Vector(-60,cy+23.25,20),App.Vector(0,1,0),App.Vector(1,0,0))
section=body.section(section_plane)
for edge in section.Edges:
    p=np.array([tuple(v) for v in edge.discretize(Deflection=.03)])
    ax.plot(p[:,0],p[:,2],color="#45627f",lw=1.1)
for solid in screen.Solids:
    section=solid.section(section_plane)
    for edge in section.Edges:
        p=np.array([tuple(v) for v in edge.discretize(Deflection=.03)])
        ax.plot(p[:,0],p[:,2],color="#d09036",lw=1.6)
ax.axhline(0,color="#9aa5b5",ls="--",lw=.8)
ax.annotate("玻璃正面与面板齐平",(0,0),(0,6),ha="center",arrowprops={"arrowstyle":"->"},fontsize=9)
ax.annotate("玻璃背面粘接台阶",(-35.7,-.95),(-23,-10),ha="center",arrowprops={"arrowstyle":"->"},fontsize=9)
rear_seat=report["design_allowances"]["lcd_rear_seat_depth"]
rear_gap=report["design_allowances"]["lcd_rear_adhesive_nominal_gap"]
for pad in report["design_allowances"]["lcd_rear_pads"]:
    x0,y0,x1,y1=pad["xy"]
    if y0<=cy+23.25<=y1:
        ax.add_patch(Rectangle((x0,rear_seat),x1-x0,rear_gap,fc="#d06a8b",alpha=.7))
ax.annotate("LCD 背贴台阶＋柔性胶层",(8,rear_seat),(16,-11),ha="center",arrowprops={"arrowstyle":"->"},fontsize=9)
ax.annotate("从正面向内装入",(21,1),(21,10),ha="center",arrowprops={"arrowstyle":"->"},fontsize=9)
ax.set(xlim=(-47,47),ylim=(-17,13),xlabel="屏幕横向 / mm",ylabel="相对面板深度 / mm")
ax.set_aspect("equal")
ax.grid(alpha=.2)
ax.set_title("横向剖面：蓝色为外壳，橙色为屏幕")
fig.suptitle("HD28091C40-V1 横屏适配：两级台阶＋分段柔性背贴",fontsize=16,y=.98)
fig.subplots_adjust(top=.92,bottom=.18,hspace=.25,wspace=.15)
fig.savefig(ROOT/"adaptation-preview.png",dpi=160,facecolor=fig.get_facecolor(),bbox_inches="tight")
plt.close(fig)

# Close view of the actual bonding land, with a separately indicated adhesive.
fig,ax=plt.subplots(figsize=(10,5),facecolor="#f5f7fa")
body_section=body.section(section_plane)
for edge in body_section.Edges:
    p=np.array([tuple(v) for v in edge.discretize(Deflection=.02)])
    ax.plot(p[:,0],p[:,2],color="#45627f",lw=2)
for solid in screen.Solids:
    for edge in solid.section(section_plane).Edges:
        p=np.array([tuple(v) for v in edge.discretize(Deflection=.02)])
        ax.plot(p[:,0],p[:,2],color="#d09036",lw=2)
seat=report["design_allowances"]["bonding_seat_depth"]
ax.add_patch(Rectangle((-36.53,seat),1.625,.25,fc="#d06a8b",alpha=.7))
ax.axhline(0,color="#999999",lw=.8,ls="--")
ax.annotate("玻璃正面 = 面板基准面（0 mm）",(-36,0),(-35.5,1.1),ha="center",arrowprops={"arrowstyle":"->"},fontsize=11)
ax.annotate("触摸盖板玻璃厚 0.70 mm",(-35.8,-.35),(-33.6,.45),arrowprops={"arrowstyle":"->"},fontsize=10)
ax.annotate("玻璃背面胶层：预留 0.25 mm",(-35.8,-.825),(-39.4,-1.75),arrowprops={"arrowstyle":"->"},fontsize=10)
ax.annotate("粘接台阶：距面板 0.95 mm",(-35.8,-.95),(-34.5,-2.55),arrowprops={"arrowstyle":"->"},fontsize=10)
ax.annotate("传感层及显示模组",(-34.3,-2.0),(-33.6,-3),arrowprops={"arrowstyle":"->"},fontsize=10)
ax.annotate("从正面装入",(-36.5,.1),(-38,.9),arrowprops={"arrowstyle":"->"},fontsize=10)
ax.set(xlim=(-40,-30),ylim=(-5,1.7),xlabel="屏幕横向 / mm",ylabel="相对面板深度 / mm")
ax.set_title("左侧玻璃背面粘接台阶局部剖面（胶层示意为粉色）")
ax.set_aspect("equal")
ax.grid(alpha=.15)
fig.tight_layout()
fig.savefig(ROOT/"flush-mount-detail.png",dpi=160,facecolor=fig.get_facecolor(),bbox_inches="tight")
plt.close(fig)
print(ROOT/"adaptation-preview.png")

# The corner close-up comes from the exported CAD, not a cosmetic image edit.
fig,ax=plt.subplots(figsize=(8,6),facecolor="#f5f7fa")
glass_outline=screen.section(Part.makePlane(100,100,App.Vector(-50,0,-.10)))
opening_outline=body.section(Part.makePlane(100,100,App.Vector(-50,0,-.10)))
for outline,color,label in [(opening_outline,"#2582ac","外壳安装口：R2.30 mm"),
                            (glass_outline,"#444444","玻璃外形：R2.00 mm（DWG 核对）")]:
    for edge in outline.Edges:
        p=np.array([tuple(v) for v in edge.discretize(Deflection=.005)])
        ax.plot(p[:,0],p[:,1],color=color,lw=2)
    ax.plot([],[],color=color,lw=2,label=label)
corner_center=np.array([-73.06/2+gr,cy+50.54/2-gr])
arc_point=corner_center+gr*np.array([-1,1])/np.sqrt(2)
outer_point=corner_center+pr*np.array([-1,1])/np.sqrt(2)
ax.plot(*corner_center,"+",color="#666666")
ax.annotate("玻璃圆角 R2.00",arc_point,(-33.5,cy+24.5),arrowprops={"arrowstyle":"->"},fontsize=11)
ax.annotate("安装口圆角 R2.30",outer_point,(-38.3,cy+26.3),arrowprops={"arrowstyle":"->"},fontsize=11)
ax.annotate("沿轮廓留隙 0.30 mm",(arc_point+outer_point)/2,(-38.1,cy+21.8),arrowprops={"arrowstyle":"->"},fontsize=11)
ax.set(xlim=(-39,-31),ylim=(cy+20,cy+28),xlabel="屏幕横向 / mm",ylabel="屏幕纵向 / mm")
ax.set_aspect("equal")
ax.set_title("左上角放大：玻璃与安装口均为圆角")
ax.grid(alpha=.15)
ax.legend(loc="lower center",fontsize=10)
fig.tight_layout()
fig.savefig(ROOT/"glass-corner-detail.png",dpi=160,facecolor=fig.get_facecolor(),bbox_inches="tight")
plt.close(fig)
