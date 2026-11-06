"""Add an external nominal R0.4 root blend to the raised joystick surround.

The radial quarter-circle follows the old outer skin. Its bottom overlaps the
skin by 0.06 mm, including the small existing roll at the right side. No inner
clearance surface is replaced or filled.
"""
from pathlib import Path
import sys, json, math, hashlib
ROOT=Path(__file__).resolve().parent
PREV=ROOT.parent
R10=PREV.parent
sys.path.insert(0,str(R10/'.cadlibs'))
import manifold3d as mf
import numpy as np
import FreeCAD as App
import Mesh, Part

NAME='HD28091-R10-joystick-plus2mm-rootR04'
SOURCE=PREV/'HD28091-R10-joystick-plus2mm.stl'
BASE=R10/'HD28091-option1-R10.stl'
CX,CZ=29.5,15.75
RADIUS,FILLET=9.0,.4
N_ANGLE,N_ARC=256,12

def read(path):
    v,t=Mesh.Mesh(str(path)).Topology
    solid=mf.Manifold(mf.Mesh64(np.array([tuple(p) for p in v]),np.array(t,dtype=np.uint64)))
    assert str(solid.status())=='Error.NoError'
    return solid

old,base=read(SOURCE),read(BASE)

def skin(radius,theta):
    x,z=CX+radius*math.cos(theta),CZ+radius*math.sin(theta)
    hits=base.ray_cast([x,-3,z],[x,1,z])
    assert hits,(radius,theta)
    return max(h.position[1] for h in hits)

# Closed profile in the radius/height plane. The small inward overlap stays
# inside the existing 0.8 mm annular connecting wall.
profile=[(8.96,FILLET)]
for t in np.linspace(0,math.pi/2,N_ARC+1):
    profile.append((RADIUS+FILLET*(1-math.cos(t)),FILLET*(1-math.sin(t))))
profile.extend([(RADIUS+FILLET,-.06),(8.96,-.06)])
vertices=[]
for a in range(N_ANGLE):
    theta=2*math.pi*a/N_ANGLE
    for radius,h in profile:
        vertices.append([CX+radius*math.cos(theta),skin(radius,theta)+h,CZ+radius*math.sin(theta)])
vertices=np.array(vertices)
triangles=[]
count=len(profile)
for a in range(N_ANGLE):
    b=(a+1)%N_ANGLE
    for j in range(count):
        k=(j+1)%count
        p,q,r,s=a*count+j,b*count+j,b*count+k,a*count+k
        triangles.extend([[p,q,r],[p,r,s]])
triangles=np.array(triangles,dtype=np.uint64)
patch=mf.Manifold(mf.Mesh64(vertices,triangles))
assert str(patch.status())=='Error.NoError',patch.status()
if patch.volume()<0:
    triangles=triangles[:,[0,2,1]].copy()
    patch=mf.Manifold(mf.Mesh64(vertices,triangles))
assert patch.volume()>0 and len(patch.decompose())==1
raw=old+patch
assert str(raw.status())=='Error.NoError' and len(raw.decompose())==1
added,removed=raw-old,old-raw
assert abs(removed.volume())<1e-6

def cylinder(radius,low,high):
    return mf.Manifold.cylinder(high-low,radius,radius,256).rotate([-90,0,0]).translate([CX,low,CZ])
inside=added^cylinder(8.2,-5,5)
assert abs(inside.volume())<1e-6
local=cylinder(9.401,-.1,.401)-cylinder(8.95,-.11,.41)
assert abs((added-local).volume())<1e-6

out=raw.to_mesh64()
vertices,indices=np.unique(np.round(out.vert_properties[:,:3],4),axis=0,return_inverse=True)
normalized=mf.Manifold(mf.Mesh(vertices.astype(np.float32),indices[np.array(out.tri_verts)].astype(np.uint32),tolerance=.0005)).simplify(.0005)
assert str(normalized.status())=='Error.NoError' and len(normalized.decompose())==1
out=normalized.to_mesh()
vertices=np.array(out.vert_properties[:,:3],dtype=np.float64)
triangles=np.array(out.tri_verts)

def write(v,suffix):
    mesh=Mesh.Mesh(v[triangles].tolist())
    checks={'closed':mesh.isSolid(),'nonmanifold':mesh.hasNonManifolds(),'self_intersections':mesh.hasSelfIntersections()}
    if checks['self_intersections']:
        pairs=mesh.getSelfIntersections()
        print('Intersecting triangle indices',pairs[:12],flush=True)
        for idx in list(pairs)[:4]:
            if isinstance(idx,tuple):idx=idx[0]
            print(idx, v[triangles[idx]].tolist(),flush=True)
    assert checks=={'closed':True,'nonmanifold':False,'self_intersections':False},checks
    mesh.write(str(ROOT/(NAME+suffix+'.stl')))
    return mesh
mesh=write(vertices,'')
c=math.sqrt(.5)
print_v=vertices@np.array([[1,0,0],[0,c,-c],[0,c,c]]).T
print_v[:,:2]-=(print_v[:,:2].min(0)+print_v[:,:2].max(0))/2
print_v[:,2]-=print_v[:,2].min()
write(print_v,'-screen-up')

# Post-serialization check of inner support surface and actual clearance gain.
final=read(ROOT/(NAME+'.stl'))
clearance=[]
for theta in np.linspace(0,2*math.pi,16,endpoint=False):
    x,z=CX+6*math.cos(theta),CZ+6*math.sin(theta)
    def inner(s):return min(h.position[1] for h in s.ray_cast([x,-3,z],[x,5,z]))
    before,raised,after=inner(base),inner(old),inner(final)
    assert abs(after-raised)<.0003
    assert abs(after-before-2)<.0003
    clearance.append({'angle_deg':math.degrees(theta),'clearance_gain_mm':after-before})
report={'source':str(SOURCE),'source_sha256':hashlib.sha256(SOURCE.read_bytes()).hexdigest(),
        'fillet_radius_nominal_mm':FILLET,'outer_foot_radius_mm':RADIUS+FILLET,
        'profile_arc_segments':N_ARC,'angular_segments':N_ANGLE,
        'construction':'External additive quarter-circle, conformed to original exterior skin; not an internal fillet.',
        'maximum_right_side_skin_drop_mm':-skin(9.4,0),
        'source_material_removed_raw_mm3':removed.volume(),'added_volume_raw_mm3':added.volume(),
        'new_material_inside_radius_8p2_mm3':inside.volume(),
        'new_material_outside_root_region_mm3':(added-local).volume(),
        'nominal_gap_to_neighbor_opening_at_center_section_mm':CX-(RADIUS+FILLET)-19.7,
        'hole_axis_world_xz_mm':[CX,CZ],'clearance_samples':clearance,
        'mesh':{'closed':True,'nonmanifold':False,'self_intersections':False,'solids':1,
                'triangles':len(triangles),'volume_mm3':final.volume(),'numeric_grid_mm':.0001,'simplification_tolerance_mm':.0005},
        'final_mesh_differences_mm3':{
            'added':(final-old).volume(),'removed':(old-final).volume(),
            'added_outside_root':((final-old)-local).volume(),
            'removed_outside_root':((old-final)-local).volume(),
            'added_inside_radius_8p2':((final-old)^cylinder(8.2,-5,5)).volume(),
            'removed_inside_radius_8p2':((old-final)^cylinder(8.2,-5,5)).volume()},
        'STEP_note':'Faceted mesh-derived solid; not a smooth parametric STEP.'}
(ROOT/'fillet-report.json').write_text(json.dumps(report,indent=2),encoding='utf8')
print(json.dumps({k:v for k,v in report.items() if k!='clearance_samples'},indent=2),flush=True)

faceted=Part.Shape();faceted.makeShapeFromMesh(mesh.Topology,.000001)
faceted=Part.makeSolid(faceted)
if faceted.Volume<0:faceted.reverse()
assert faceted.isValid() and len(faceted.Solids)==1
faceted.exportStep(str(ROOT/(NAME+'-faceted.step')))
check=Part.Shape();check.read(str(ROOT/(NAME+'-faceted.step')))
assert check.isValid() and len(check.Solids)==1
report['step_readback']={'valid':True,'solids':1,'volume_mm3':check.Volume}
(ROOT/'fillet-report.json').write_text(json.dumps(report,indent=2),encoding='utf8')
print('STEP readback passed',flush=True)
