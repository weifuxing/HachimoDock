"""Verify and describe the generated, locally sliced fit-trial projects."""
from pathlib import Path
import json,zipfile,xml.etree.ElementTree as ET
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib import font_manager
from PIL import Image
import io

ROOT=Path(__file__).resolve().parent
font_manager.fontManager.addfont(r"C:\Windows\Fonts\msyh.ttc")
plt.rcParams.update({"font.family":"Microsoft YaHei","axes.unicode_minus":False})

summary=[]
fig,axes=plt.subplots(1,3,figsize=(12,5),facecolor="#101418")
for ax,gap in zip(axes,[.30,.40,.50]):
    name=f"HD28091-A1-ABS-gap{gap:.2f}-trial"
    out=ROOT/name
    data=json.loads((out/"result.json").read_text())
    assert data["return_code"]==0 and len(data["sliced_plates"])==1
    plate=data["sliced_plates"][0]
    assert len(plate["objects"])==1
    assert abs(plate["objects"][0]["bbox"]["height"]-9)<1e-4
    log=(out/"slicer.log").read_text()
    assert "[error]" not in log and "ZFiller" not in log
    project=out/(name+".3mf")
    with zipfile.ZipFile(project) as archive:
        assert archive.testzip() is None
        settings=json.loads(archive.read("Metadata/project_settings.config"))
        for key,value in [("printer_model","Bambu Lab A1"),("layer_height","0.16"),
                          ("wall_loops","4"),("enable_support","1"),("curr_bed_type","Textured PEI Plate")]:
            assert settings[key]==value,(key,settings[key])
        assert settings["filament_type"]==["ABS"]
        assert settings["filament_shrink"]==["100%"]
        xml=ET.fromstring(archive.read("Metadata/slice_info.config"))
        meta={e.get("key"):e.get("value") for e in xml.iter("metadata")}
        assert meta["outside"]=="false"
        assert len(archive.read("Metadata/plate_1.gcode"))>100000
        native=Image.open(io.BytesIO(archive.read("Metadata/plate_1.png")))
        warnings=[e.attrib for e in xml.iter("warning")]
        native.load()
    minutes=round(plate["total_predication"]/60)
    grams=plate["filaments"][0]["total_used_g"]
    summary.append({"clearance_per_side_mm":gap,"project":str(project),
        "estimated_minutes":minutes,"estimated_filament_g":grams,
        "slicer_return_code":0,"slicer_internal_error_log":False,"within_build_volume":True,
        "warnings":warnings,"profile_source":"Installed Bambu Studio Generic ABS @BBL A1",
        "nozzle_temp_first_layer_C":settings["nozzle_temperature_initial_layer"],
        "nozzle_temp_C":settings["nozzle_temperature"],"bed_C":settings["textured_plate_temp"]})
    ax.imshow(native)
    ax.set_axis_off()
    ax.set_title(f"单边间隙 {gap:.2f} mm",fontsize=15,color="white")
    ax.text(.5,.02,f"预计 {minutes} 分钟 / {grams:.1f} g",transform=ax.transAxes,
            ha="center",va="bottom",fontsize=11,color="#d0d7df")
fig.suptitle("A1＋0.4 mm＋ABS 局部配合试样：先打印 0.30 mm，再按结果选更大间隙",fontsize=15,color="white",y=.98)
fig.text(.5,.025,"图片来自 Bambu Studio 切片工程；每个试样约 80 × 54.6 × 9 mm，包含两级台阶和 LCD / TP 出线口。",ha="center",fontsize=10,color="#d0d7df")
fig.tight_layout(rect=(0,.08,1,.91))
fig.savefig(ROOT/"A1-ABS-fit-trials-preview.png",dpi=160,facecolor=fig.get_facecolor(),bbox_inches="tight")
plt.close(fig)
(ROOT/"verified-trial-projects.json").write_text(json.dumps(summary,indent=2,ensure_ascii=False),encoding="utf-8")

notes=f"""A1＋0.4 mm 喷嘴＋ABS 局部试打说明

先打印：HD28091-A1-ABS-gap0.30-trial 文件夹中的同名 3mf。
预计耗时 {summary[0]['estimated_minutes']} 分钟，耗材约 {summary[0]['estimated_filament_g']:.1f} g；实际以打印机为准。
0.30、0.40、0.50 表示玻璃及 LCD 周边的单边设计间隙，不是双面胶厚度。

测试件保留屏幕安装区域：玻璃圆角、两级台阶、四段背贴支撑和 LCD/TP 出线避让。
测试件切掉了按键区域及较深壳体，并增加平整试打底面；它不是完整前壳。
本轮未改变整壳尺寸或加入 ABS 收缩倍率。全尺寸前壳仍需根据试打结果继续优化。

项目设置
拓竹 A1，0.4 mm 喷嘴，纹理 PEI 板，Generic ABS @BBL A1 起始预设。
0.16 mm 层高、4 圈墙、5 层顶底壳、20% Gyroid 填充。
8 mm 外沿；自动普通支撑，仅从热床生成；支撑接触间距 0.20 mm。
玻璃面朝上，支撑主要位于背部，使功能贴胶面朝上成形。
首层喷嘴 {summary[0]['nozzle_temp_first_layer_C'][0]}℃，后续 {summary[0]['nozzle_temp_C'][0]}℃，热床 {summary[0]['bed_C'][0]}℃，来自本机 Generic ABS 预设。
请按实际 ABS 线材标签和实际板材核对温度，然后在 Bambu Studio 中重新切片。
项目保留切片器的“热床温度相对耗材偏高”和延时摄影兼容提示；未将其隐藏或视作已消除。

怎么试
1. 先用 0.30 mm 项目打印一件；冷却到室温，清理外沿和支撑后检查是否翘曲。
2. 先不贴胶，轻放屏幕，记录玻璃及 LCD 槽是否都能放入、四周间隙是否均匀、排线能否自然通过。
3. 若配合偏紧，依次试 0.40、0.50 mm 项目。若样件翘曲，先处理打印条件，再比较间隙。
4. 合适后再试胶层：玻璃背贴名义 0.25 mm，LCD 背贴名义工作间隙 0.60 mm。
   LCD 实际工作间隙受模组厚度公差影响为约 0.40～0.80 mm，胶材与可贴区域仍需实物确认。
5. 记录选中的间隙、实测孔宽高、台阶平整度、排线照片，再据此定整壳版本。

三档同时打印的工程在 HD28091-A1-ABS-fit-trials 文件夹。
同时打多档时，取下热床前按对象名称分别标记，避免混淆。

检查结果与范围
三档 STL 均为单个有效实体及封闭网格；独立 3mf 已切片成功、在 A1 成型范围内，且无内部几何错误日志。
整壳在经典墙设置下的三种姿态曾出现 ZFiller 内部几何日志；后续 Arachne 完整前壳工程已消除该日志，但排线槽薄顶仍待优化。详见完整前壳审查说明.txt。
试样只验证局部工艺和配合，不证明整壳收缩、翘曲、粘接强度或长期可靠性。
"""
(ROOT/"试打说明.txt").write_text(notes,encoding="utf-8")
print(json.dumps([{k:v for k,v in row.items() if k in ['clearance_per_side_mm','estimated_minutes','estimated_filament_g','project']} for row in summary],indent=2))
