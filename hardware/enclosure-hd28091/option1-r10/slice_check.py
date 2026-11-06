"""One local slicing check; the delivered 3MF remains unsliced."""
from pathlib import Path
import subprocess,json
ROOT=Path(__file__).resolve().parent
OUT=ROOT/"slice-check"
OUT.mkdir(exist_ok=True)
args=[r"C:\Program Files\Bambu Studio\bambu-studio.exe","--slice","0",
    "--load-settings",str(ROOT/"a1-machine.json")+";"+str(ROOT/"a1-process.json"),
    "--load-filaments",str(ROOT/"generic-abs.json"),"--orient","0","--arrange","1","--ensure-on-bed",
    "--export-3mf","option1-slice-check.3mf","--outputdir",str(OUT),str(ROOT/"HD28091-option1-R10-screen-up.stl")]
r=subprocess.run(args,capture_output=True,text=True,encoding="utf-8",errors="replace",timeout=180)
log=r.stdout+"\n"+r.stderr
(OUT/"slicer.log").write_text(log,encoding="utf-8")
info={"return_code":r.returncode,"internal_error_log":"[error]" in log}
if (OUT/"result.json").exists():
    result=json.loads((OUT/"result.json").read_text())
    if result.get("sliced_plates"):
        plate=result["sliced_plates"][0]
        info.update({"estimated_minutes":round(plate["total_predication"]/60),
            "estimated_grams":plate["filaments"][0]["total_used_g"],"model_count":len(plate["objects"])})
report=ROOT/"option1-report.json"
d=json.loads(report.read_text());d["local_slice_check"]=info;report.write_text(json.dumps(d,indent=2),encoding="utf-8")
print(json.dumps(info,indent=2),flush=True)
