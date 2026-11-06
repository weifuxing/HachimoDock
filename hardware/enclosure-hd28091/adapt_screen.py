"""Local STEP modification using FreeCAD 1.1's bundled Python.

Original coordinate system is retained in all delivered manufacturing files.
Screen coordinates: X = horizontal, Y = up the sloping face, Z = outward.
Run with C:/Program Files/FreeCAD 1.1/bin/python.exe.
"""
from pathlib import Path
import hashlib
import json
import math

import FreeCAD as App
import Part
import MeshPart

ROOT = Path(__file__).resolve().parent
SOURCE = Path(r"C:\Users\fuxing\Downloads\桌搭-整机外壳3d打印版.STEP")
DRAWING = Path(r"C:\Users\fuxing\Downloads\HD28091C40-V1总成(2).jpg")

# Nominal dimensions from the user's HD28091C40-V1 assembly drawing, millimetres.
TP_W, TP_H, TP_T = 73.06, 50.54, 1.05  # landscape
GLASS_T = 0.70  # cover glass only; the complete touch stack is 1.05 mm
GLASS_R = 2.00  # verified from the original DWG; see glass-corner-evidence.json
BL_W, BL_H = 69.21, 50.00
TOTAL_T = 3.53
AA_W, AA_H = 57.60, 43.20
VA_W, VA_H = 58.20, 43.80
CY = 32.70  # same centre as the original surface opening
# BL is centred within TP. AA starts 2.83 mm below BL top in portrait.
# Counterclockwise rotation of the PORTRAIT FRONT VIEW places FPCs on the
# right, AA towards the left, and the off-centre TP tail towards lower right.
AA_CX = -TP_W/2 + (TP_W-BL_W)/2 + 2.83 + AA_W/2
SIDE_CLEARANCE = 0.30
PANEL_T = 3.00
ADHESIVE_T = 0.25  # design allowance; adhesive is not part of the screen drawing
GLASS_FRONT = 0.00  # original planar panel datum: flush installation
GLASS_BACK = GLASS_FRONT-GLASS_T
SEAT_DEPTH = GLASS_BACK-ADHESIVE_T
MODULE_BACK = GLASS_FRONT-TOTAL_T
OPENING_W, OPENING_H = TP_W+2*SIDE_CLEARANCE,TP_H+2*SIDE_CLEARANCE
OPENING_R = GLASS_R+SIDE_CLEARANCE  # true outward contour offset
REAR_ADHESIVE_T = 0.60  # nominal installed gap; actual soft adhesive not selected
REAR_SEAT_DEPTH = MODULE_BACK-REAR_ADHESIVE_T
REAR_SUPPORT_T = 2.00
REAR_GUSSET_ROOT_DEPTH = -8.50  # existing shell floor; avoid a hanging lower toe
REAR_GUSSET_ROOT_FILLET_R = 0.80
MOUNT_EAR_TRIM_DEPTH = -15.50  # make the two ears flush with the existing rear frame
MOUNT_EAR_X_RANGES = [(-30.0,-20.0),(20.0,30.0)]
# Four separated bonding strips along the rear module perimeter. Their actual
# suitability for bonding must be verified on the physical LCD rear structure.
REAR_PADS = [
    {"name":"upper_left","xy":[-27,CY+22,-11,CY+24.5]},
    {"name":"upper_right","xy":[0,CY+22,16,CY+24.5]},
    {"name":"lower_left","xy":[-27,CY-24.5,-11,CY-22]},
    {"name":"lower_right","xy":[0,CY-24.5,16,CY-22]},
]

def box(x0, y0, z0, x1, y1, z1):
    return Part.makeBox(x1-x0, y1-y0, z1-z0, App.Vector(x0,y0,z0))

def rectangle(width, height, z0, z1, cx=0, cy=CY):
    return box(cx-width/2,cy-height/2,z0,cx+width/2,cy+height/2,z1)

def rounded_rectangle(width,height,radius,z0,z1,cx=0,cy=CY):
    shape=rectangle(width,height,z0,z1,cx,cy)
    vertical=[e for e in shape.Edges
              if e.BoundBox.XLength<1e-7 and e.BoundBox.YLength<1e-7]
    result=shape.makeFillet(radius,vertical)
    assert result.isValid() and len(result.Solids)==1
    return result

def world(shape):
    result = shape.copy()
    result.rotate(App.Vector(), App.Vector(1,0,0), -45)
    return result

def bb(shape):
    # Default B-spline bounding boxes can overestimate the original by 0.29 mm.
    # Ignore cached display triangulations as well as loose curve bounds.
    b = shape.optimalBoundingBox(False,False)
    return [b.XMin,b.YMin,b.ZMin,b.XMax,b.YMax,b.ZMax]

original = Part.Shape()
original.read(str(SOURCE))
assert original.isValid() and len(original.Solids)==8
base = original.Solids[0].copy()
base.rotate(App.Vector(),App.Vector(1,0,0),45)

# Fill only the old screen opening, tied to the existing front-face outline.
# Face index is bound to the original source and guarded by geometric checks.
rim = base.Faces[42]
assert abs(rim.CenterOfMass.z)<1e-5 and len(rim.Wires)==2
assert abs(rim.BoundBox.XLength-77.5)<1e-5
panel = Part.Face(rim.OuterWire).extrude(App.Vector(0,0,-PANEL_T))
filled = base.fuse(panel).removeSplitter()

# Front insertion: large, shallow glass recess and smaller opening below it.
# The two side ledges bear on the exposed REAR OF THE 0.70 mm COVER GLASS,
# outside the sensor/backlight envelope; adhesive thickness sets the flush datum.
# The original touch-stack thickness must not be mistaken for glass thickness.
tp_pocket = rounded_rectangle(OPENING_W,OPENING_H,OPENING_R,SEAT_DEPTH,1.0)
backlight_pocket = rectangle(BL_W+2*SIDE_CLEARANCE,BL_H+2*SIDE_CLEARANCE,
                            -40.0,GLASS_BACK+0.05)
pocketed = filled.cut(tp_pocket.fuse(backlight_pocket)).removeSplitter()

# Integral second-level shelves extend from the upper and lower shell rails.
# The shelves stop below the LCD, leaving the nominal flexible adhesive gap.
rear_shelves=[]
rear_pads=[]
rear_gussets=[]
rear_root_bridges=[]
gusset_specs=[]
for pad in REAR_PADS:
    x0,y0,x1,y1=pad["xy"]
    shelf_y0,shelf_y1=(CY+21.5,59.8) if "upper" in pad["name"] else (5.9,CY-21.5)
    rear_shelves.append(box(x0-1,shelf_y0,REAR_SEAT_DEPTH-REAR_SUPPORT_T,
                            x1+1,shelf_y1,REAR_SEAT_DEPTH))
    upper="upper" in pad["name"]
    # Meet the edge of the EXISTING rear floor exactly. Extending a 45-degree
    # triangle downwards past that floor produced a vertical notch and lower toe.
    # The bearing shelf still extends outwards into the shell rail as before.
    tip_y=CY+21.5 if upper else CY-21.5
    root_y=CY+(BL_H/2+SIDE_CLEARANCE) if upper else CY-(BL_H/2+SIDE_CLEARANCE)
    run=abs(root_y-tip_y)
    back_z=REAR_SEAT_DEPTH-REAR_SUPPORT_T
    drop=back_z-REAR_GUSSET_ROOT_DEPTH
    angle=math.degrees(math.atan2(drop,run))
    verts=[App.Vector(x0-1,tip_y,back_z),App.Vector(x0-1,root_y,back_z),
           App.Vector(x0-1,root_y,back_z-drop),App.Vector(x0-1,tip_y,back_z)]
    gusset=Part.Face(Part.makePolygon(verts)).extrude(App.Vector(x1-x0+2,0,0))
    assert gusset.isValid()
    rear_gussets.append(gusset)
    bridge_y0,bridge_y1=(root_y,shelf_y1) if upper else (shelf_y0,root_y)
    rear_root_bridges.append(box(x0-1,bridge_y0,REAR_GUSSET_ROOT_DEPTH,
                                  x1+1,bridge_y1,back_z))
    gusset_specs.append({"name":pad["name"],"x_range_mm":[x0-1,x1+1],
        "tip_y_mm":tip_y,"root_y_mm":root_y,"slope_angle_deg":angle,
        "run_mm":run,"drop_mm":drop,"tip_thickness_mm":REAR_SUPPORT_T,
        "root_design_thickness_mm":REAR_SUPPORT_T+drop,
        "root_back_depth_mm":back_z-drop,"root_fillet_radius_mm":REAR_GUSSET_ROOT_FILLET_R})
    rear_pads.append(box(x0,y0,REAR_SEAT_DEPTH,x1,y1,MODULE_BACK))
flat_supported=pocketed.multiFuse(rear_shelves).removeSplitter()
supported=flat_supported.multiFuse(rear_gussets+rear_root_bridges).removeSplitter()
gusset_added_volume=supported.cut(flat_supported).Volume
gusset_removed_volume=flat_supported.cut(supported).Volume
assert gusset_added_volume>0 and gusset_removed_volume<1e-6
# Blend each horizontal floor / sloping underside junction. Select by geometry,
# not unstable edge numbers. These edges are away from all bonding planes.
root_edges=[]
for edge in supported.Edges:
    if len(edge.Vertexes)!=2 or edge.Length<1e-5:
        continue
    pts=[v.Point for v in edge.Vertexes]
    if abs(edge.Length-abs(pts[0].x-pts[1].x))>1e-5:
        continue
    if any(abs(p.z-REAR_GUSSET_ROOT_DEPTH)>1e-5 for p in pts):
        continue
    if any(all(abs(p.y-spec["root_y_mm"])<1e-5 for p in pts)
           and min(p.x for p in pts)>=spec["x_range_mm"][0]-1e-5
           and max(p.x for p in pts)<=spec["x_range_mm"][1]+1e-5
           for spec in gusset_specs):
        root_edges.append(edge)
for spec in gusset_specs:
    covered=sum(edge.Length for edge in root_edges
        if all(abs(v.Point.y-spec["root_y_mm"])<1e-5 for v in edge.Vertexes)
        and min(v.Point.x for v in edge.Vertexes)>=spec["x_range_mm"][0]-1e-5
        and max(v.Point.x for v in edge.Vertexes)<=spec["x_range_mm"][1]+1e-5)
    assert abs(covered-(spec["x_range_mm"][1]-spec["x_range_mm"][0]))<1e-5
before_root_blend=supported
# Build the tiny tangent arc cuts explicitly in the longitudinal section.
# A global fillet on the inherited split floor edges is topologically unstable.
root_cutters=[]
for spec in gusset_specs:
    theta=math.radians(spec["slope_angle_deg"])
    radius=REAR_GUSSET_ROOT_FILLET_R
    tangent=radius*math.tan(theta/2)
    direction=1 if spec["tip_y_mm"]>spec["root_y_mm"] else -1
    x=spec["x_range_mm"][0]
    def profile_point(u,z):
        return App.Vector(x,spec["root_y_mm"]+direction*u,z)
    a=profile_point(-tangent,REAR_GUSSET_ROOT_DEPTH)
    corner=profile_point(0,REAR_GUSSET_ROOT_DEPTH)
    b=profile_point(tangent*math.cos(theta),REAR_GUSSET_ROOT_DEPTH+tangent*math.sin(theta))
    mid=profile_point(-tangent+radius*math.sin(theta/2),
        REAR_GUSSET_ROOT_DEPTH+radius*(1-math.cos(theta/2)))
    wire=Part.Wire([Part.makeLine(a,corner),Part.makeLine(corner,b),Part.Arc(b,mid,a).toShape()])
    cutter=Part.Face(wire).extrude(App.Vector(spec["x_range_mm"][1]-x,0,0))
    assert cutter.isValid() and cutter.Volume>0
    root_cutters.append(cutter)
supported=supported.cut(Part.makeCompound(root_cutters)).removeSplitter()
assert supported.isValid() and len(supported.Solids)==1
root_blend_removed_volume=before_root_blend.cut(supported).Volume
assert root_blend_removed_volume>0

# Separate conservative LCD and TP routing volumes, calibrated from the user's
# JPG FRONT view, not the mirrored rear view. Both pass through the right wall.
# The LCD root includes its broad fan-out and component branch, not only its
# 20.5 mm connector tail. The TP root is offset below the LCD centreline.
# Routes merge where they overlap. The slot radii ease edges but do not certify
# an FPC bend radius; the complete flexible paths need physical verification.
lcd_slot=rounded_rectangle(10.5,35.0,0.8,-8.6,-0.65,cx=33.75,cy=CY)
tp_slot=rounded_rectangle(10.5,10.5,0.8,-8.6,-0.55,cx=33.75,cy=CY-12.25)
lcd_keepout=box(29.5,CY-16,-8,38.3,CY+16,-0.75)
tp_keepout=box(29.5,CY-16,-8,38.3,CY-8.5,-0.65)
fpc_path=lcd_slot.fuse(tp_slot)
final_local = supported.cut(fpc_path).removeSplitter()
assert final_local.isValid() and len(final_local.Solids)==1

# User-requested flat trim of ONLY the two original mounting-ear projections.
# Hole axes and cylindrical faces lie above this plane and remain untouched.
before_ear_trim=final_local.copy()
ear_cutters=Part.makeCompound([box(x0,-10,-40,x1,10,MOUNT_EAR_TRIM_DEPTH)
                              for x0,x1 in MOUNT_EAR_X_RANGES])
removed_ear_caps=before_ear_trim.common(ear_cutters)
assert removed_ear_caps.Volume>0 and len(removed_ear_caps.Solids)==2
final_local=before_ear_trim.cut(ear_cutters).removeSplitter()
assert final_local.isValid() and len(final_local.Solids)==1
assert final_local.optimalBoundingBox(False,False).ZMin>=MOUNT_EAR_TRIM_DEPTH-1e-5

def ear_holes(shape):
    holes=[]
    for face in shape.Faces:
        if not isinstance(face.Surface,Part.Cylinder):
            continue
        cyl=face.Surface
        if abs(cyl.Radius-1)<1e-6 and abs(abs(cyl.Center.x)-25)<1e-5:
            holes.append(face)
    return holes

before_holes,after_holes=ear_holes(before_ear_trim),ear_holes(final_local)
assert len(before_holes)==4 and len(after_holes)==4
assert abs(sum(f.Area for f in before_holes)-sum(f.Area for f in after_holes))<1e-5
for face in before_holes:
    assert any(face.cut(other).Area<1e-5 for other in after_holes)
ear_hole_margin=min(f.optimalBoundingBox(False,False).ZMin for f in after_holes)-MOUNT_EAR_TRIM_DEPTH
ear_original_bottom=before_ear_trim.optimalBoundingBox(False,False).ZMin

glass = rounded_rectangle(TP_W,TP_H,GLASS_R,GLASS_BACK,GLASS_FRONT)
# Conservative backing envelope includes the sensor and backlight. Both fit
# inside the BL outline; the bare-glass side strips remain available for bonding.
backlight = rectangle(BL_W,BL_H,MODULE_BACK,GLASS_BACK)
module = glass.fuse(backlight).removeSplitter()
final_body = world(final_local)
assembly_parts = [final_body]+[s.copy() for s in original.Solids[1:]]
assembly = Part.makeCompound(assembly_parts)

collision = final_local.common(module).Volume
other_collisions = []
for i, solid in enumerate(original.Solids[1:],2):
    other_collisions.append({"solid":i,"volume_mm3":world(module).common(solid).Volume})
added = final_local.cut(base)
removed = base.cut(final_local)
screen_change_region = box(-39.01,5.89,-40.01,39.01,60.0,0.01)
change_region = screen_change_region.fuse(ear_cutters)
outside_changes = added.cut(change_region).Volume+removed.cut(change_region).Volume
# Continuous axial FRONT insertion: glass and backing follow different outlines.
insertion_sweep = rounded_rectangle(TP_W,TP_H,GLASS_R,GLASS_BACK,40.0).fuse(
    rectangle(BL_W,BL_H,MODULE_BACK,40.0))
insertion_collision = final_local.common(insertion_sweep).Volume
# Drawing tolerances: TP outline +/-0.05, BL outline +/-0.2, cover glass
# +/-0.05 and total thickness +/-0.2. Use maximum material envelopes.
max_glass_back=GLASS_BACK-0.05
max_module_back=MODULE_BACK-0.20
max_module=rounded_rectangle(TP_W+0.05,TP_H+0.05,GLASS_R,max_glass_back,GLASS_FRONT).fuse(
    rectangle(BL_W+0.20,BL_H+0.20,max_module_back,max_glass_back))
max_screen_collision=final_local.common(max_module).Volume
max_insertion_collision=final_local.common(
    rounded_rectangle(TP_W+0.05,TP_H+0.05,GLASS_R,max_glass_back,40.0).fuse(
        rectangle(BL_W+0.20,BL_H+0.20,max_module_back,40.0))).Volume
# Verify bonding lands exist on BOTH exposed glass ends, after the FPC notch.
contact_slab=final_local.common(rounded_rectangle(TP_W,TP_H,GLASS_R,SEAT_DEPTH-0.01,SEAT_DEPTH))
left_contact=contact_slab.common(box(-40,CY-30,-2,0,CY+30,0)).Volume/0.01
right_contact=contact_slab.common(box(0,CY-30,-2,40,CY+30,0)).Volume/0.01
assert left_contact>20 and right_contact>20

rear_contact=[]
active_prism=rectangle(AA_W,AA_H,-10,1,AA_CX)
for pad in REAR_PADS:
    x0,y0,x1,y1=pad["xy"]
    area=final_local.common(box(x0,y0,REAR_SEAT_DEPTH-.01,x1,y1,REAR_SEAT_DEPTH)).Volume/.01
    assert abs(area-(x1-x0)*(y1-y0))<1e-4
    rear_contact.append({"name":pad["name"],"xy_mm":pad["xy"],"contact_area_mm2":area})
pad_active_overlap=sum(p.common(active_prism).Volume for p in rear_pads)/REAR_ADHESIVE_T
assert pad_active_overlap<1e-6
fpc_checks=[]
for name,keepout in [("LCD",lcd_keepout),("TP",tp_keepout)]:
    hard_collision=final_local.common(keepout).Volume
    pad_collision=sum(p.common(keepout).Volume for p in rear_pads)
    other_collision=[world(keepout).common(s).Volume for s in original.Solids[1:]]
    assert hard_collision<1e-6 and pad_collision<1e-6
    assert all(v<1e-6 for v in other_collision)
    fpc_checks.append({"name":name,"keepout_bounds_mm":bb(keepout),
        "shell_collision_mm3":hard_collision,"pad_collision_mm3":pad_collision,
        "other_parts_collision_mm3":other_collision})
new_material_collisions=[world(added).common(s).Volume for s in original.Solids[1:]]
assert all(v<1e-6 for v in new_material_collisions)
assert collision<1e-6 and all(v["volume_mm3"]<1e-6 for v in other_collisions)
assert insertion_collision<1e-6
assert max_screen_collision<1e-6 and max_insertion_collision<1e-6
assert outside_changes<1e-5
original_bounds,new_bounds=bb(original),bb(assembly)
assert max(abs(a-b) for a,b in zip(original_bounds,new_bounds))<1e-5

# Solid difference checks establish that the other seven source parts survive.
unchanged=[]
for i,(before,after) in enumerate(zip(original.Solids[1:],assembly_parts[1:]),2):
    difference=before.cut(after).Volume+after.cut(before).Volume
    assert difference<1e-6
    unchanged.append({"solid":i,"symmetric_difference_mm3":difference})

final_body.exportStep(str(ROOT/"HD28091-front-body.step"))
assembly.exportStep(str(ROOT/"HD28091-enclosure-assembly.step"))
world(module).exportStep(str(ROOT/"HD28091-screen-envelope-reference.step"))
world(removed_ear_caps).exportStep(str(ROOT/"HD28091-ear-trim-removed-reference.step"))
Part.makeCompound([world(p) for p in rear_pads]).exportStep(str(ROOT/"HD28091-flexible-pads-reference.step"))
Part.makeCompound([world(lcd_keepout),world(tp_keepout)]).exportStep(str(ROOT/"HD28091-fpc-keepouts-reference.step"))
mesh=MeshPart.meshFromShape(Shape=final_body,LinearDeflection=0.08,
                           AngularDeflection=0.20,Relative=False)
assert mesh.isSolid()
mesh.write(str(ROOT/"HD28091-front-body.stl"))

# Persist a native CAD document with separate manufacturing parts and references.
doc=App.newDocument("HD28091Enclosure")
src=doc.addObject("Part::Feature","OriginalFront")
src.Label="Original front body (reference)"
src.Shape=original.Solids[0]
src.Visibility=False
group=doc.addObject("App::DocumentObjectGroup","ManufacturingParts")
group.Label="Manufacturing parts - 8 solids"
for i,solid in enumerate(assembly_parts,1):
    item=doc.addObject("Part::Feature",f"Part{i:02d}")
    item.Label="Modified front body" if i==1 else f"Original part {i:02d} - unchanged"
    item.Shape=solid
    group.addObject(item)
    if i==1:
        for name,value in [("GlassOpeningWidth",OPENING_W),("GlassOpeningHeight",OPENING_H),
                           ("CoverGlassCornerRadius",GLASS_R),("GlassOpeningCornerRadius",OPENING_R),
                           ("LateralClearance",SIDE_CLEARANCE),("PanelThickness",PANEL_T),
                           ("CoverGlassThickness",GLASS_T),("BondingSeatDepth",-SEAT_DEPTH),
                           ("LcdRearSeatDepth",-REAR_SEAT_DEPTH),("LcdRearAdhesiveGap",REAR_ADHESIVE_T),
                           ("SupportTipThickness",REAR_SUPPORT_T),
                           ("MountEarTrimDepth",-MOUNT_EAR_TRIM_DEPTH),
                           ("AdhesiveAllowance",ADHESIVE_T),("ScreenTotalThickness",TOTAL_T)]:
            item.addProperty("App::PropertyLength",name,"Design inputs")
            setattr(item,name,value)
        item.addProperty("App::PropertyString","RebuildInstructions","Design inputs")
        item.RebuildInstructions="Edit adapt_screen.py parameters and rerun; properties are documentation."
        item.addProperty("App::PropertyAngle","SupportUndersideSlope","Design inputs")
        item.SupportUndersideSlope=gusset_specs[0]["slope_angle_deg"]
        item.addProperty("App::PropertyLength","SupportRootFilletRadius","Design inputs")
        item.SupportRootFilletRadius=REAR_GUSSET_ROOT_FILLET_R
ref=doc.addObject("Part::Feature","ScreenEnvelopeReference")
ref.Label="HD28091 conservative screen envelope - reference only"
ref.Shape=world(module)
ref.Visibility=False
for name,label,ref_shape in [
    ("RearAdhesiveReference","LCD rear flexible pads - provisional reference",Part.makeCompound([world(p) for p in rear_pads])),
    ("LcdFpcKeepout","LCD FPC clearance - reference",world(lcd_keepout)),
    ("TpFpcKeepout","TP FPC clearance - reference",world(tp_keepout))]:
    ref=doc.addObject("Part::Feature",name)
    ref.Label=label
    ref.Shape=ref_shape
    ref.Visibility=False
ref=doc.addObject("Part::Feature","RemovedEarCapsReference")
ref.Label="Removed mounting-ear tips - reference only"
ref.Shape=world(removed_ear_caps)
ref.Visibility=False
doc.recompute()
doc.saveAs(str(ROOT/"HD28091-enclosure.FCStd"))

# Re-import the exported assembly and front body, not only the in-memory shapes.
readback=[]
for name,count in [("HD28091-front-body.step",1),("HD28091-enclosure-assembly.step",8)]:
    s=Part.Shape()
    s.read(str(ROOT/name))
    assert s.isValid() and len(s.Solids)==count
    readback.append({"file":name,"valid":s.isValid(),"solids":len(s.Solids),"volume_mm3":s.Volume})

report={"source":str(SOURCE),"source_sha256":hashlib.sha256(SOURCE.read_bytes()).hexdigest(),
    "units":"mm","orientation":"landscape, FPC right (counterclockwise from portrait FRONT drawing); TP tail below LCD centre",
    "installation":"front insertion; first-level glass bonding lands and second-level LCD rear shelves reinforced with floor-aligned sloping gussets and filleted roots, with four provisional flexible adhesive strips; glass front flush with panel",
    "fpc_drawing":{"source":str(DRAWING),"sha256":hashlib.sha256(DRAWING.read_bytes()).hexdigest(),
        "view":"portrait FRONT drawing rotated counterclockwise; do not mirror from rear view",
        "method":"Conservative allowances calibrated from raster drawing; full LCD root fan-out and offset TP root included.",
        "lcd_connector_pins":40,"lcd_connector_width_mm":20.5,"tp_connector_pins":6},
    "source_dimensions":{"tp_landscape":[TP_W,TP_H],"tp_thickness":TP_T,
        "glass_corner_radius":GLASS_R,"glass_corner_evidence":"glass-corner-evidence.json",
        "cover_glass_thickness":GLASS_T,"backlight_landscape":[BL_W,BL_H],"total_thickness":TOTAL_T,
        "active_area_landscape":[AA_W,AA_H],"view_area_landscape":[VA_W,VA_H]},
    "design_allowances":{"glass_opening":[OPENING_W,OPENING_H],"side_clearance_each":SIDE_CLEARANCE,
        "mount_ear_trim_depth":MOUNT_EAR_TRIM_DEPTH,
        "glass_opening_corner_radius":OPENING_R,
        "panel_thickness":PANEL_T,"adhesive_thickness":ADHESIVE_T,"bonding_seat_depth":SEAT_DEPTH,
        "lcd_rear_adhesive_nominal_gap":REAR_ADHESIVE_T,"lcd_rear_seat_depth":REAR_SEAT_DEPTH,
        "lcd_rear_adhesive_gap_for_total_thickness_tolerance":[REAR_ADHESIVE_T-.2,REAR_ADHESIVE_T+.2],
        "lcd_rear_pads":REAR_PADS,"lcd_rear_support_thickness":REAR_SUPPORT_T,
        "lcd_support_underside_gussets":gusset_specs,
        "support_root_floor_depth":REAR_GUSSET_ROOT_DEPTH,"support_root_fillet_radius":REAR_GUSSET_ROOT_FILLET_R,
        "fpc_slot_corner_radius":0.8,
        "screen_center_local":[0,CY],"active_center_local":[AA_CX,CY],
        "glass_front_depth":GLASS_FRONT,"module_back_depth":MODULE_BACK},
    "checks":{"screen_body_collision_mm3":collision,"screen_other_parts_collisions":other_collisions,
        "mount_ear_trim":{"removed_volume_mm3":removed_ear_caps.Volume,"removed_solids":len(removed_ear_caps.Solids),
            "trim_plane_local_depth_mm":MOUNT_EAR_TRIM_DEPTH,
            "maximum_tip_removed_height_mm":MOUNT_EAR_TRIM_DEPTH-ear_original_bottom,
            "hole_cylindrical_faces_unchanged":True,"hole_diameter_mm":2.0,
            "minimum_vertical_hole_margin_to_trim_plane_mm":ear_hole_margin,
            "rear_frame_and_trimmed_tips_coplanar":True},
        "continuous_front_insertion_collision_mm3":insertion_collision,
        "left_glass_bonding_land_area_mm2":left_contact,"right_glass_bonding_land_area_mm2":right_contact,
        "lcd_rear_bonding_lands":rear_contact,
        "gusset_added_volume_mm3":gusset_added_volume,"gusset_removed_existing_material_mm3":gusset_removed_volume,
        "root_transition_fillet_edges":len(root_edges),"root_fillet_removed_volume_mm3":root_blend_removed_volume,
        "root_transition_blends":len(root_cutters),
        "rear_pad_projection_over_active_area_mm2":pad_active_overlap,
        "fpc_clearances":fpc_checks,"new_material_vs_original_other_parts_mm3":new_material_collisions,
        "glass_to_panel_flush_offset_mm":GLASS_FRONT,
        "adhesive_gap_range_for_glass_tolerance_mm":[ADHESIVE_T-0.05,ADHESIVE_T+0.05],
        "max_tolerance_screen_collision_mm3":max_screen_collision,
        "max_tolerance_insertion_collision_mm3":max_insertion_collision,
        "changes_outside_screen_region_mm3":added.cut(screen_change_region).Volume+removed.cut(screen_change_region).Volume,
        "changes_outside_authorized_regions_mm3":outside_changes,"unchanged_parts":unchanged,
        "original_assembly_bounds":original_bounds,"new_assembly_bounds":new_bounds,
        "added_material_mm3":added.Volume,"removed_material_mm3":removed.Volume,
        "final_body_valid":final_body.isValid(),"stl_closed":mesh.isSolid(),"step_readback":readback},
    "limitations":["Glass outline uses DWG-verified R2 corners; sensor and backlight remain conservative rectangular envelopes, not a complete vendor 3D model.",
        "Mounting-ear tips were trimmed flush per user request. The approximately 0.94 mm minimum vertical hole-edge margin is a geometric measurement, not a fastening-strength certification.",
        "Glass corner radius tolerance is not specified in the supplied drawing; dimensions-at-tolerance checks hold nominal R2 constant.",
        "LCD/TP routing volumes are conservative allowances from the user's JPG front view, not exact flexible-circuit CAD. Root clearances do not certify full tail routing, fold radius or connector insertion.",
        "Rear pad regions are provisional: the physical LCD rear material and permitted bonding/load areas require confirmation. No adhesion, stress or lifetime test has been performed.",
        "Rear flexible adhesive material is not selected: its installed thickness must accommodate the 0.40-0.80 mm rear gap across module thickness tolerance without lifting the flush glass or stressing the LCD.",
        "Adhesive attaches the exposed rear cover-glass ends to side lands; nominal glue thickness 0.25 mm.",
        "Print tolerances and adhesive clearance require a test fit.",
        "FCStd properties document inputs; rebuild by editing and executing this script."]}
(ROOT/"adaptation-report.json").write_text(json.dumps(report,indent=2),encoding="utf-8")
print(json.dumps(report["checks"],indent=2),flush=True)
