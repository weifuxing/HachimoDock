"""Build a local Bambu Studio trial project; never send a print job."""
from pathlib import Path
import json,subprocess,sys

ROOT=Path(__file__).resolve().parent
PROFILES=Path(r"C:\Program Files\Bambu Studio\resources\profiles\BBL")
SLICER=Path(r"C:\Program Files\Bambu Studio\bambu-studio.exe")

def resolve(category,name,seen=None):
    seen=set() if seen is None else seen
    assert name not in seen,(category,name)
    seen=seen|{name}
    path=PROFILES/category/(name+".json")
    data=json.loads(path.read_text(encoding="utf-8"))
    parent=data.get("inherits")
    result=resolve(category,parent,seen) if parent else {}
    for include in data.get("include",[]):
        result.update(resolve(category,include,seen))
    result.update(data)
    result.pop("inherits",None);result.pop("include",None)
    return result

machine=resolve("machine","Bambu Lab A1 0.4 nozzle")
process=resolve("process","0.16mm High Quality @BBL A1")
filament=resolve("filament","Generic ABS @BBL A1")
process.update({"name":"HD28091 ABS fit trials - A1 0.4","from":"user","curr_bed_type":"Textured PEI Plate",
    "wall_loops":"4","top_shell_layers":"5","bottom_shell_layers":"5",
    "sparse_infill_density":"20%","sparse_infill_pattern":"gyroid",
    "brim_type":"outer_only","brim_width":"8","brim_object_gap":"0.15",
    "enable_support":"1","support_type":"normal(auto)","support_on_build_plate_only":"1",
    "support_threshold_angle":"45","support_top_z_distance":"0.20",
    "support_bottom_z_distance":"0.20","support_interface_top_layers":"3",
    "support_object_xy_distance":"0.35","elefant_foot_compensation":"0.15",
    "outer_wall_speed":["40"],"inner_wall_speed":["80"],"top_surface_speed":["40"],
    "internal_solid_infill_speed":["80"],"initial_layer_speed":["20"],
    "initial_layer_infill_speed":["30"],"travel_speed":["300"],"default_acceleration":["3000"],
    "xy_hole_compensation":"0","xy_contour_compensation":"0"})
for name,settings in [("a1-machine.json",machine),("trial-process.json",process),("generic-abs.json",filament)]:
    (ROOT/name).write_text(json.dumps(settings,indent=2,ensure_ascii=False),encoding="utf-8")

def run_case(name,files,slice_model=True):
    out=ROOT/name
    out.mkdir(exist_ok=True)
    args=[str(SLICER),"--load-settings",str(ROOT/"a1-machine.json")+";"+str(ROOT/"trial-process.json"),
        "--load-filaments",str(ROOT/"generic-abs.json"),"--orient","0","--arrange","1",
        "--ensure-on-bed","--export-3mf",name+".3mf",
        "--outputdir",str(out)]+[str(f) for f in files]
    if slice_model:
        args[1:1]=["--slice","0"]
    result=subprocess.run(args,capture_output=True,text=True,encoding="utf-8",errors="replace",timeout=300)
    log=result.stdout+"\n"+result.stderr
    (out/"slicer.log").write_text(log,encoding="utf-8")
    print(json.dumps({"name":name,"returncode":result.returncode,"log_tail":log[-4500:]},ensure_ascii=False),flush=True)
    return result.returncode

if __name__=="__main__":
    mode=sys.argv[1] if len(sys.argv)>1 else "coupons"
    if mode in ["full-arachne","full-screen-up","full-screen-up-raft","full-screen-up-trimmed","full-screen-up-trimmed-project"]:
        process["name"]="HD28091 complete front shell - A1 ABS experimental"
        process["wall_generator"]="arachne"
        if mode=="full-screen-up-raft":
            process["raft_layers"]="3"
        (ROOT/"trial-process.json").write_text(json.dumps(process,indent=2,ensure_ascii=False),encoding="utf-8")
        name="HD28091-A1-ABS-full-review" if mode=="full-arachne" else "HD28091-A1-ABS-screen-up-review"
        if mode=="full-screen-up-raft":
            name="HD28091-A1-ABS-screen-up-raft-review"
        if mode=="full-screen-up-trimmed":
            name="HD28091-A1-ABS-screen-up-trimmed"
        if mode=="full-screen-up-trimmed-project":
            name="HD28091-A1-ABS-screen-up-trimmed-project"
        mesh="front-mating_down-audit.stl" if mode=="full-arachne" else "front-screen_up-audit.stl"
        if mode.startswith("full-screen-up-trimmed"):
            (ROOT/"HD28091-front-screen-up-trimmed.stl").write_bytes((ROOT/mesh).read_bytes())
        sys.exit(run_case(name,[ROOT/mesh],slice_model=mode!="full-screen-up-trimmed-project"))
    if mode=="coupons":
        files=[ROOT/f"screen-fit-gap-{g:.2f}-screen-up.stl" for g in [.30,.40,.50]]
        sys.exit(run_case("HD28091-A1-ABS-fit-trials",files))
    if mode in ["gap0.30","gap0.40","gap0.50"]:
        gap=float(mode[3:])
        name=f"HD28091-A1-ABS-gap{gap:.2f}-trial"
        sys.exit(run_case(name,[ROOT/f"screen-fit-gap-{gap:.2f}-screen-up.stl"]))
    assert mode in ["mating_down","screen_up","screen_down"]
    sys.exit(run_case("audit-"+mode,[ROOT/f"front-{mode}-audit.stl"]))
