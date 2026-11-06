"""Raise the coaxial joystick surround outward by 2 mm; retain the R10 shell."""
from pathlib import Path
import sys, json, hashlib, math
ROOT = Path(__file__).resolve().parent
PREV = ROOT.parent
sys.path.insert(0, str(PREV / '.cadlibs'))
import manifold3d as mf
import numpy as np
import FreeCAD as App
import Mesh, Part

NAME = 'HD28091-R10-joystick-plus2mm'
CENTER_X, CENTER_Z = 29.5, 15.75
R_OUT, R_IN = 9.0, 8.2
SHIFT = 2.0
SOURCE = PREV / 'HD28091-option1-R10.stl'

def read_solid(path):
    vertices, triangles = Mesh.Mesh(str(path)).Topology
    return mf.Manifold(mf.Mesh64(np.array([tuple(v) for v in vertices]), np.array(triangles, dtype=np.uint64)))

def cylinder(radius, low, high):
    return mf.Manifold.cylinder(high-low, radius, radius, 192).rotate([-90, 0, 0]).translate([CENTER_X, low, CENTER_Z])

old = read_solid(SOURCE)
assert str(old.status()) == 'Error.NoError'
# Only the front skin and annular crown are detached. Deep side walls stay put.
zone = cylinder(R_OUT, -1.0, 5.0)
cap = old ^ zone
raised = cap.translate([0, SHIFT, 0])
# A 0.8 mm radial wall connects the shifted skin to the retained panel.
collar = cylinder(R_OUT, -1.05, 1.5) - cylinder(R_IN, -1.1, 1.6)
result = mf.Manifold.batch_boolean([old-zone, raised, collar], mf.OpType.Add)
assert str(result.status()) == 'Error.NoError', result.status()
assert len(result.decompose()) == 1
changed_zone = cylinder(R_OUT+.001, -1.051, 5.001)
removed, added = old-result, result-old
assert abs((removed-changed_zone).volume()) < 1e-6
assert abs((added-changed_zone).volume()) < 1e-6
# The moved crown, taper, and aperture are exactly preserved before serialization.
inner = cylinder(7.8, 1.01, 5.0)
assert abs(((result ^ inner)-(raised ^ inner)).volume()) < 1e-6
assert abs(((raised ^ inner)-(result ^ inner)).volume()) < 1e-6

out = result.to_mesh64()
vertices, remap = np.unique(np.round(out.vert_properties[:, :3], 4), axis=0, return_inverse=True)
triangles = remap[np.array(out.tri_verts)]
normal = mf.Manifold(mf.Mesh(vertices.astype(np.float32), triangles.astype(np.uint32), tolerance=.0001)).simplify(.0001)
assert str(normal.status()) == 'Error.NoError' and len(normal.decompose()) == 1
out = normal.to_mesh()
vertices, triangles = np.array(out.vert_properties[:, :3], dtype=np.float64), np.array(out.tri_verts)

def write_mesh(vertices, suffix):
    m = Mesh.Mesh(vertices[triangles].tolist())
    assert m.isSolid() and not m.hasNonManifolds() and not m.hasSelfIntersections()
    m.write(str(ROOT / (NAME + suffix + '.stl')))
    return m

mesh = write_mesh(vertices, '')
c = math.sqrt(.5)
print_vertices = vertices @ np.array([[1,0,0],[0,c,-c],[0,c,c]]).T
print_vertices[:, :2] -= (print_vertices[:, :2].min(0)+print_vertices[:, :2].max(0))/2
print_vertices[:, 2] -= print_vertices[:, 2].min()
write_mesh(print_vertices, '-screen-up')

# Cross-section measurements in original assembly coordinates.
def lower_surface(solid, x):
    hits = solid.ray_cast([x, -10, CENTER_Z], [x, 6, CENTER_Z])
    ys = sorted(h.position[1] for h in hits)
    return ys
measurements = []
for radius in [-6, 6, -7, 7]:
    before = lower_surface(old, CENTER_X+radius)
    after = lower_surface(result, CENTER_X+radius)
    assert before and after, (radius, before, after)
    gain = after[0]-before[0]
    assert abs(gain-SHIFT) < 1e-5, (radius, gain)
    measurements.append({'offset_x_mm':radius, 'old_inner_y_mm':before[0], 'new_inner_y_mm':after[0], 'clearance_gain_mm':gain})

report = {'source':str(SOURCE), 'source_sha256':hashlib.sha256(SOURCE.read_bytes()).hexdigest(),
    'axis_world':'+Y, outward normal of button panel', 'translation_mm':SHIFT,
    'hole_center_world_xz_mm':[CENTER_X,CENTER_Z], 'hole_center_moved':False,
    'modified_radius_mm':R_OUT, 'collar_inner_radius_mm':R_IN, 'collar_radial_wall_mm':R_OUT-R_IN,
    'crown_top_y_mm':{'before':1.0,'after':3.0},
    'section_clearance_measurements':measurements,
    'old_volume_mm3':old.volume(), 'new_volume_mm3':normal.volume(),
    'removed_volume_mm3':removed.volume(), 'added_volume_mm3':added.volume(),
    'changes_outside_local_zone_mm3':abs((removed-changed_zone).volume())+abs((added-changed_zone).volume()),
    'mesh':{'connected_solids':1,'triangles':len(triangles),'closed':True,'nonmanifold':False,'self_intersections':False,
            'normalization_grid_mm':.0001},
    'STEP_note':'Mesh-derived faceted solid, not a smooth parametric model.'}
(ROOT/'revision-report.json').write_text(json.dumps(report,indent=2),encoding='utf8')
print(json.dumps(report,indent=2),flush=True)

faceted=Part.Shape(); faceted.makeShapeFromMesh(mesh.Topology,.000001)
faceted=Part.makeSolid(faceted)
if faceted.Volume<0: faceted.reverse()
assert faceted.isValid() and len(faceted.Solids)==1
faceted.exportStep(str(ROOT/(NAME+'-faceted.step')))
check=Part.Shape(); check.read(str(ROOT/(NAME+'-faceted.step')))
assert check.isValid() and len(check.Solids)==1
report['step_readback']={'valid':True,'solids':1,'volume_mm3':check.Volume}
(ROOT/'revision-report.json').write_text(json.dumps(report,indent=2),encoding='utf8')
print('STEP readback passed',flush=True)
