"""Exterior R10 appearance revision based on the physically fitted STL.

Manifold performs the final solid union because the old STEP contains invalid
curve-on-surface parameterizations for OCCT booleans.  Any STEP derived here is
explicitly a faceted solid; analytic corner construction is retained separately.
"""
from pathlib import Path
import sys,json,hashlib,math
ROOT=Path(__file__).resolve().parent
BASE=ROOT.parent
sys.path.insert(0,str(ROOT/".cadlibs"))
sys.path.insert(0,str(BASE))
import manifold3d as mf
import numpy as np
import FreeCAD as App
import Part,Mesh,MeshPart
import outer_corner_option1 as corners

def rot(points,degrees):
    a=math.radians(degrees);c,s=math.cos(a),math.sin(a)
    return np.asarray(points)@np.array([[1,0,0],[0,c,-s],[0,s,c]]).T

def manifold(mesh,degrees=0):
    v,t=mesh.Topology
    points=rot([tuple(p) for p in v],degrees)
    solid=mf.Manifold(mf.Mesh64(np.ascontiguousarray(points,dtype=np.float64),np.array(t,dtype=np.uint64)))
    assert str(solid.status())=="Error.NoError",str(solid.status())
    return solid

def mesh_shape(shape):
    m=MeshPart.meshFromShape(Shape=shape,LinearDeflection=.02,AngularDeflection=.12,Relative=False)
    assert m.isSolid()
    return manifold(m)

source=BASE/"HD28091-front-body.stl"
original=manifold(Mesh.Mesh(str(source)),45)
patches,zones,specs=corners.make_patches()
tools=[mesh_shape(p) for p in patches]
result=mf.Manifold.batch_boolean([original]+tools,mf.OpType.Add)
assert str(result.status())=="Error.NoError"
pieces=result.decompose()
tiny=[v for v in pieces if abs(v.volume())<1e-6]
main=[v for v in pieces if abs(v.volume())>=1e-6]
assert len(main)==1
result=main[0]
removed=original-result
added=result-original
assert abs(removed.volume())<1e-6

# Independent witness points in the four new exterior corner regions.
probes=[[-42,61,-7.5],[42,61,-7.5]]
for x in [-41.5,41.5]:
    probes.append([x,(-7.5-29)/math.sqrt(2),(-7.5+29)/math.sqrt(2)])
witness=[]
for p in probes:
    sphere=mf.Manifold.sphere(.05,16).translate(p)
    fraction=(result^sphere).volume()/sphere.volume()
    assert fraction>.999,fraction
    witness.append({"point_screen_local_mm":p,"fraction_inside":fraction})

def box(x0,y0,z0,x1,y1,z1):
    return Part.makeBox(x1-x0,y1-y0,z1-z0,App.Vector(x0,y0,z0))

# Added material must not enter the proven screen, adhesive, and cable spaces.
glass_mask=box(-36.83,7.13,-15.5,36.83,58.27,.05)
glass_mask=glass_mask.makeFillet(2.3,[e for e in glass_mask.Edges if e.BoundBox.XLength<1e-7 and e.BoundBox.YLength<1e-7])
protected=[
    ("glass_and_screen",glass_mask),
    ("LCD_FPC",box(29.5,16.7,-8,38.3,48.7,-.75)),
    ("TP_FPC",box(29.5,16.7,-8,38.3,24.2,-.65)),
    ("rear_adhesive_upper",box(-27,54.7,-4.13,16,57.2,-3.53)),
    ("rear_adhesive_lower",box(-27,8.2,-4.13,16,10.7,-3.53)),
]
protection=[]
for name,shape in protected:
    overlap=(added^mesh_shape(shape)).volume()
    assert abs(overlap)<1e-5,(name,overlap)
    protection.append({"region":name,"new_material_mm3":overlap})

# Preserve all seven unchanged assembly parts, including the original rear lid.
assembly=Part.Shape();assembly.read(str(BASE/"HD28091-enclosure-assembly.step"))
assembly_collisions=[]
for i,part in enumerate(assembly.Solids[1:],2):
    m=MeshPart.meshFromShape(Shape=part,LinearDeflection=.06,AngularDeflection=.15,Relative=False)
    intersection=(added^manifold(m,45)).volume()
    assert abs(intersection)<.01,(i,intersection)
    assembly_collisions.append({"part":i,"new_material_intersection_mm3":intersection})

out=result.to_mesh64()
local=np.array(out.vert_properties[:,:3]);tri=np.array(out.tri_verts)
world=rot(local,-45)
# Normalize to a 0.0001 mm grid before STL's float32 serialization. This removes
# a numerical edge crossing only a few millionths of a millimetre long.
world,indices=np.unique(np.round(world,4),axis=0,return_inverse=True)
normalized=mf.Manifold(mf.Mesh(np.asarray(world,dtype=np.float32),
    np.asarray(indices[tri],dtype=np.uint32),tolerance=.0001)).simplify(.0001)
assert str(normalized.status())=="Error.NoError" and len(normalized.decompose())==1
normalized_mesh=normalized.to_mesh()
world=np.array(normalized_mesh.vert_properties[:,:3],dtype=np.float64)
tri=np.array(normalized_mesh.tri_verts)
local=rot(world,45)

def write_stl(vertices,name):
    m=Mesh.Mesh(vertices[tri].tolist())
    assert m.isSolid() and not m.hasNonManifolds() and not m.hasSelfIntersections()
    m.write(str(ROOT/name))
    return m

world_mesh=write_stl(world,"HD28091-option1-R10.stl")
print_vertices=local.copy()
print_vertices[:,:2]-=(print_vertices[:,:2].min(axis=0)+print_vertices[:,:2].max(axis=0))/2
print_vertices[:,2]-=print_vertices[:,2].min()
write_stl(print_vertices,"HD28091-option1-R10-screen-up.stl")
corner_refs=[]
for patch in patches:
    ref=patch.copy();ref.rotate(App.Vector(),App.Vector(1,0,0),-45);corner_refs.append(ref)
Part.makeCompound(corner_refs).exportStep(str(ROOT/"exterior-corner-patches-analytic-reference.step"))

report={"source":str(source),"source_sha256":hashlib.sha256(source.read_bytes()).hexdigest(),
    "operation":"Exterior union only, against the user-tested STL; no original material removed",
    "corners":specs,"old_volume_mm3":original.volume(),"new_volume_mm3":result.volume(),
    "discarded_numerical_component_volumes_mm3":[v.volume() for v in tiny],
    "added_volume_mm3":added.volume(),"removed_volume_mm3":removed.volume(),
    "four_corner_witnesses":witness,"protected_spaces":protection,"assembly_intersections":assembly_collisions,
    "connected_solids":len(result.decompose()),"triangle_count":len(tri),
    "baseline_tessellation":"Existing user-tested STL; new analytic patches meshed at 0.02 mm linear deflection",
    "STL_numeric_normalization":{"coordinate_grid_mm":.0001,"simplification_tolerance_mm":.0001,
        "maximum_combined_surface_deviation_bound_mm":.0002,"closed":True,"self_intersections":False},
    "STEP_note":"Analytic patches are a reference construction; final print solid uses robust mesh union. Any final STEP is a faceted mesh-derived solid."}
(ROOT/"option1-report.json").write_text(json.dumps(report,indent=2),encoding="utf-8")
print(json.dumps(report,indent=2),flush=True)

# Optional exchange CAD, intentionally labelled as faceted geometry.
faceted=Part.Shape();faceted.makeShapeFromMesh(world_mesh.Topology,.000001)
faceted=Part.makeSolid(faceted)
if faceted.Volume<0:faceted.reverse()
assert faceted.isValid() and len(faceted.Solids)==1
faceted.exportStep(str(ROOT/"HD28091-option1-R10-faceted.step"))
doc=App.newDocument("HD28091Option1R10")
obj=doc.addObject("Part::Feature","PrintSolid")
obj.Label="Option 1 R10 - mesh-derived faceted print solid"
obj.Shape=faceted
doc.recompute();doc.saveAs(str(ROOT/"HD28091-option1-R10.FCStd"))
print("Faceted STEP and FCStd saved",flush=True)
