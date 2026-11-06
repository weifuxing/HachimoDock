"""FDM audit and local fit trials from the current enclosure STEP.

Does not change the master enclosure. All print poses are explicit and all
mesh results are checked for closedness. Coupons are trimmed test pieces,
not replacements for the complete front shell.
"""
from pathlib import Path
import json, math, hashlib
import FreeCAD as App
import Part,MeshPart
import numpy as np

ROOT=Path(__file__).resolve().parent
MASTER=ROOT.parent/"HD28091-front-body.step"
design=json.loads((ROOT.parent/"adaptation-report.json").read_text(encoding="utf-8"))
CY=design["design_allowances"]["screen_center_local"][1]

def load():
    s=Part.Shape();s.read(str(MASTER));return s

def box(x0,y0,z0,x1,y1,z1):
    return Part.makeBox(x1-x0,y1-y0,z1-z0,App.Vector(x0,y0,z0))

def export_mesh(shape,name):
    assert shape.isValid() and len(shape.Solids)==1
    mesh=MeshPart.meshFromShape(Shape=shape,LinearDeflection=.06,AngularDeflection=.15,Relative=False)
    assert mesh.isSolid()
    mesh.write(str(ROOT/name))
    return mesh

def on_bed(s):
    s=s.copy()
    b=s.optimalBoundingBox(False,False)
    s.translate(App.Vector(-(b.XMin+b.XMax)/2,-(b.YMin+b.YMax)/2,-b.ZMin))
    return s

def rounded(width,height,radius,z0,z1):
    s=box(-width/2,CY-height/2,z0,width/2,CY+height/2,z1)
    edges=[e for e in s.Edges if e.BoundBox.XLength<1e-7 and e.BoundBox.YLength<1e-7]
    return s.makeFillet(radius,edges)

report={"source":str(MASTER),"sha256":hashlib.sha256(MASTER.read_bytes()).hexdigest(),
    "trial_machine":"Bambu Lab A1","nozzle_mm":.4,"material":"ABS, experimental per user request",
    "poses":[],"coupons":[],"limitations":[
        "Downward-face areas are geometric screening only, not support or toolpath simulation.",
        "No material shrink compensation is applied; fit trials are required.",
        "Coupons trim away the controls and rear body; they do not prove full-shell warping or strength."]}

for name,angle,label in [("mating_down",90,"后壳接合面朝下"),
                         ("screen_up",45,"屏幕面朝上"),
                         ("screen_down",225,"屏幕面朝下")]:
    s=load();s.rotate(App.Vector(),App.Vector(1,0,0),angle);s=on_bed(s)
    mesh=export_mesh(s,f"front-{name}-audit.stl")
    verts,tris=mesh.Topology
    t=np.array([[tuple(verts[i]) for i in tri] for tri in tris])
    cross=np.cross(t[:,1]-t[:,0],t[:,2]-t[:,0]);length=np.linalg.norm(cross,axis=1)
    area=length/2
    normal=cross/np.maximum(length[:,None],1e-12)
    on_floor=t[:,:,2].max(axis=1)<.05
    down=(normal[:,2]<-math.sqrt(.5))&(~on_floor)
    b=s.optimalBoundingBox(False,False)
    report["poses"].append({"name":name,"label_zh":label,"rotation_x_from_master_deg":angle,
        "size_mm":[b.XLength,b.YLength,b.ZLength],"horizontal_bed_contact_mm2":float(area[on_floor].sum()),
        "downward_surface_under_45deg_mm2":float(area[down].sum()),
        "downward_projected_area_mm2":float((area[down]*-normal[down,2]).sum()),
        "fits_a1_256_bed":max(b.XLength,b.YLength,b.ZLength)<256})

local=load();local.rotate(App.Vector(),App.Vector(1,0,0),45)
# Functional screen region: two-level seats, fillets, and both FPC exits remain.
# A planar cut makes a test base; the full shell's exterior is deliberately absent.
trim=box(-40,5.5,-9.0,40,60.1,0.0)
coupon=local.common(trim).removeSplitter()
assert coupon.isValid() and len(coupon.Solids)==1
for gap in [.30,.40,.50]:
    variant=coupon.copy()
    if gap>.30:
        variant=variant.cut(rounded(73.06+2*gap,50.54+2*gap,2+gap,-.95,1)).removeSplitter()
        # Match the LCD perimeter allowance as well as the glass: otherwise a
        # tight backlight pocket could mask the result of a larger glass opening.
        # Stop above the rear adhesive seat so the four bonding lands survive.
        variant=variant.cut(box(-69.21/2-gap,CY-25-gap,-3.78,
                                69.21/2+gap,CY+25+gap,-.65)).removeSplitter()
    # Rear side on bed: adhesive planes face upwards for inspection/fit testing.
    variant=on_bed(variant)
    name=f"screen-fit-gap-{gap:.2f}-screen-up.stl"
    export_mesh(variant,name)
    variant.exportStep(str(ROOT/name.replace(".stl",".step")))
    b=variant.optimalBoundingBox(False,False)
    report["coupons"].append({"file":name,"clearance_per_side_mm":gap,
        "glass_opening_mm":[73.06+2*gap,50.54+2*gap],"opening_corner_radius_mm":2+gap,
        "lcd_perimeter_pocket_mm":[69.21+2*gap,50+2*gap],
        "size_mm":[b.XLength,b.YLength,b.ZLength],"volume_mm3":variant.Volume,
        "glass_seat_depth_mm":.95,"lcd_seat_depth_mm":4.13,
        "material_shrink_scale":1.0,"is_valid":variant.isValid(),"solids":len(variant.Solids)})

(ROOT/"fdm-audit.json").write_text(json.dumps(report,indent=2,ensure_ascii=False),encoding="utf-8")
print(json.dumps(report,indent=2,ensure_ascii=False))
