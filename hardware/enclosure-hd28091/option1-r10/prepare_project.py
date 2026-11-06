"""Export an unsliced A1 project and native preview for option 1."""
from pathlib import Path
import sys,json,subprocess,zipfile
ROOT=Path(__file__).resolve().parent
sys.path.insert(0,str(ROOT.parent/"fdm"))
import slice_trials as presets
settings=presets.process.copy()
settings.update({"name":"HD28091 Option 1 R10 - A1 ABS review","wall_generator":"arachne"})
for file,data in [("a1-machine.json",presets.machine),("a1-process.json",settings),("generic-abs.json",presets.filament)]:
    (ROOT/file).write_text(json.dumps(data,indent=2,ensure_ascii=False),encoding="utf-8")
name="HD28091-option1-R10-screen-up.3mf"
args=[str(presets.SLICER),"--load-settings",str(ROOT/"a1-machine.json")+";"+str(ROOT/"a1-process.json"),
    "--load-filaments",str(ROOT/"generic-abs.json"),"--orient","0","--arrange","1","--ensure-on-bed",
    "--export-3mf",name,"--outputdir",str(ROOT),str(ROOT/"HD28091-option1-R10-screen-up.stl")]
run=subprocess.run(args,capture_output=True,text=True,encoding="utf-8",errors="replace",timeout=120)
(ROOT/"project-export.log").write_text(run.stdout+"\n"+run.stderr,encoding="utf-8")
assert run.returncode==0,run.stderr
with zipfile.ZipFile(ROOT/name) as archive:
    assert archive.testzip() is None
    assert not any(n.endswith(".gcode") for n in archive.namelist())
    cfg=json.loads(archive.read("Metadata/project_settings.config"))
    assert cfg["printer_model"]=="Bambu Lab A1"
    (ROOT/"option1-native-preview.png").write_bytes(archive.read("Metadata/plate_1.png"))
print(ROOT/name)
