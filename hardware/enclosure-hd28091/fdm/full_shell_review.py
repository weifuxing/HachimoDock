"""A1 full-shell FDM review with measured port-roof sections."""
from pathlib import Path
import json,zipfile,hashlib,xml.etree.ElementTree as ET
import FreeCAD as App
import Part,Mesh
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib import font_manager
from matplotlib.patches import Rectangle

ROOT=Path(__file__).resolve().parent
MASTER=ROOT.parent/"HD28091-front-body.step"
OUT=ROOT/"HD28091-A1-ABS-full-review"
PROJECT=OUT/"HD28091-A1-ABS-full-review.3mf"
font_manager.fontManager.addfont(r"C:\Windows\Fonts\msyh.ttc")
plt.rcParams.update({"font.family":"Microsoft YaHei","axes.unicode_minus":False})

s=Part.Shape();s.read(str(MASTER));assert s.isValid() and len(s.Solids)==1
s.rotate(App.Vector(),App.Vector(1,0,0),45)
m=Mesh.Mesh(str(ROOT.parent/"HD28091-front-body.stl"))
assert m.isSolid() and not m.hasNonManifolds() and not m.hasSelfIntersections()
result=json.loads((OUT/"result.json").read_text())
assert result["return_code"]==0
plate=result["sliced_plates"][0]
assert len(plate["objects"])==1
assert "[error]" not in (OUT/"slicer.log").read_text()
with zipfile.ZipFile(PROJECT) as archive:
    assert archive.testzip() is None
    cfg=json.loads(archive.read("Metadata/project_settings.config"))
    assert cfg["printer_model"]=="Bambu Lab A1" and cfg["wall_generator"]=="arachne"
    xml=ET.fromstring(archive.read("Metadata/slice_info.config"))
    warnings=[e.attrib for e in xml.iter("warning")]
    meta={e.get("key"):e.get("value") for e in xml.iter("metadata")}
    assert meta["outside"]=="false"
    (ROOT/"full-shell-native.png").write_bytes(archive.read("Metadata/plate_1.png"))

thin=json.loads((ROOT/"thin-planar-samples.json").read_text())
critical=[f for f in thin if f["thickness_mm"]<.8]
assert sorted(f["thickness_mm"] for f in critical)==[.55,.65]
summary={"source":str(MASTER),"source_sha256":hashlib.sha256(MASTER.read_bytes()).hexdigest(),
    "scope":"Complete front shell, Bambu Lab A1, 0.4 mm nozzle, ABS per user instruction",
    "geometry_changes_this_review":"None; material and FPC routing uncertainties are not resolved by blind thickness changes.",
    "geometry_checks":{"valid_solid":True,"closed_mesh":True,"non_manifold":False,"mesh_self_intersections":False},
    "sampled_thin_roofs":critical,
    "wall_sampling_scope":"Face-centre inward-normal samples on planar faces only; not a global minimum wall-thickness proof.",
    "print_pose":"Rear-shell mating face down; screen plane is tilted relative to the build plate.",
    "sliced_size_mm":plate["objects"][0]["bbox"],"estimated_minutes":round(plate["total_predication"]/60),
    "estimated_grams":plate["filaments"][0]["total_used_g"],
    "slicer":{"version":"Bambu Studio 2.6.0.51","wall_generator":"arachne","layer_height_mm":.16,
        "wall_loops":4,"infill":"20% gyroid","brim_mm":8,"automatic_support":True,
        "slicer_return_code":0,"internal_geometry_error_log":False,"warnings":warnings},
    "remaining_design_work":[
        "TP and LCD port ceilings have 0.55/0.65 mm local wall thickness. Reinforce or redesign the port opening after establishing actual cable fold envelopes.",
        "A 1.0-1.2 mm port-roof design target is provisional; it cannot be achieved by intruding into the current reserved FPC volumes without review.",
        "Glass fit, LCD rear adhesive regions, glue thickness and ABS shrink/warping still need physical confirmation."],
    "thermal_profile":{"source":"Installed Generic ABS @BBL A1, not a validated profile for the user's unknown ABS brand",
        "nozzle_first_layer_C":cfg["nozzle_temperature_initial_layer"],"nozzle_C":cfg["nozzle_temperature"],
        "bed_C":cfg["textured_plate_temp"]},
    "readiness":"Review/experimental slicing project; not manufacturing acceptance or verified ABS print success."}
(ROOT/"A1-full-shell-fdm-review.json").write_text(json.dumps(summary,indent=2,ensure_ascii=False),encoding="utf-8")

fig=plt.figure(figsize=(13,7),facecolor="#f5f7fa")
grid=fig.add_gridspec(2,2,width_ratios=[1.05,1.25])
ax=fig.add_subplot(grid[:,0]);ax.imshow(plt.imread(ROOT/"full-shell-native.png"));ax.set_axis_off()
ax.set_title("完整前壳：后壳接合面朝下",fontsize=14)
ax.text(.5,-.02,"Arachne＋自动支撑＋8 mm 外沿\n约 88.5 × 88.0 × 60.0 mm",transform=ax.transAxes,ha="center",fontsize=11)

plane=Part.makePlane(80,25,App.Vector(37.91,0,-20),App.Vector(1,0,0),App.Vector(0,1,0))
edges=s.section(plane).Edges
for row,(name,y0,y1,point,thickness,color) in enumerate([
    ("TP 出线槽",17,24,20.45,.55,"#ce6677"),
    ("LCD 出线槽",34,42,37.887,.65,"#dd9d3c")]):
    ax=fig.add_subplot(grid[row,1])
    for edge in edges:
        pts=np.array([tuple(v) for v in edge.discretize(Deflection=.01)])
        ax.plot(pts[:,1],pts[:,2],color="#45627f",lw=2)
    ax.add_patch(Rectangle((y0,-thickness),y1-y0,thickness,fc=color,alpha=.35))
    ax.annotate("",(point,0),(point,-thickness),arrowprops={"arrowstyle":"<->","color":"#333333","lw":1.3})
    ax.annotate(f"局部厚度 {thickness:.2f} mm",(point,-thickness/2),(point+1,-1.3),fontsize=11,
                arrowprops={"arrowstyle":"->"})
    ax.set(xlim=(y0,y1),ylim=(-1.8,.7),xlabel="屏幕纵向 / mm",ylabel="相对面板深度 / mm")
    ax.set_title(name+"上方薄顶：需要优先处理",fontsize=13)
    ax.grid(alpha=.15)
fig.suptitle("A1＋0.4 mm＋ABS 完整前壳审查：重点是两处排线槽薄顶",fontsize=16,y=.98)
fig.text(.5,.015,"剖面取自当前完整 STEP。厚度为指定位置的法向测量；加强薄顶时必须保持 LCD / TP 排线避让空间。",ha="center",fontsize=10,color="#555555")
fig.tight_layout(rect=(0,.06,1,.94),h_pad=1.8,w_pad=2)
fig.savefig(ROOT/"A1-full-shell-fdm-review.png",dpi=160,facecolor=fig.get_facecolor(),bbox_inches="tight")
plt.close(fig)

minutes=summary["estimated_minutes"]
notes=f"""A1＋0.4 mm＋ABS：完整前壳 FDM 审查

结论：仍需要局部结构优化。当前已解决切片算法日志问题，但不等于薄顶强度和 ABS 成型已验证。

优先处理
TP 出线槽顶约 0.55 mm；LCD 出线槽顶约 0.65 mm。
建议在不挤占实际排线折弯空间的前提下，将薄顶加固至约 1.0～1.2 mm，或改成便于从内侧走线的开口/盖片结构。
目前排线避让是基于图纸的保守包络，不能直接把薄顶向槽内加厚。
其余已采样的平面壁中，另有一处约 1.20 mm；采样不覆盖所有曲面或全部最薄位置。
四个背贴支撑的前端 2 mm、根部约 4.37 mm、R0.8 顺接仍保留，当前未发现它们与装配件/排线包络干涉。

完整前壳工程
文件：HD28091-A1-ABS-full-review/HD28091-A1-ABS-full-review.3mf。
包含一个完整前壳，不是局部试样；后壳接合面朝下。
摆放后约 88.5 × 88.0 × 60.0 mm，位于 A1 的成型范围内。
0.16 mm 层高、Arachne、4 圈墙、20% Gyroid、8 mm 外沿、自动支撑。
切片估计 {minutes//60} 小时 {minutes%60} 分钟，约 {summary['estimated_grams']:.1f} g（含辅助结构，实际以打印机为准）。
经典墙的三种姿态曾产生 ZFiller 内部日志；当前 Arachne 工程切片返回成功且无内部几何错误日志。
实体有效，STL 封闭、无非流形与自交；四个根部的几何连接检查保留在上级 adaptation-report.json。

材料与验证边界
工程使用本机 Generic ABS @BBL A1 起始预设：首层 260℃，后续 270℃，热床 100℃。
实际 ABS 品牌未确认，打印前请按线材及板材要求核对温度并重新切片。
原生工程保留“热床温度相对耗材偏高”和延时摄影兼容提示，未隐藏这些提示。
本轮未改变整壳 CAD 或盲加收缩倍率；尚无整壳 ABS 实测、强度或长期粘接验证。
"""
(ROOT/"完整前壳审查说明.txt").write_text(notes,encoding="utf-8")
print(json.dumps({k:summary[k] for k in ['estimated_minutes','estimated_grams','sampled_thin_roofs','readiness']},indent=2,ensure_ascii=False))
