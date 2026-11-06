"""Measure the original STEP in screen coordinates; never change the input."""
from pathlib import Path
import json
import math
import sys

import FreeCAD as App
import Part
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib import font_manager
from mpl_toolkits.mplot3d.art3d import Poly3DCollection
import numpy as np

font_manager.fontManager.addfont(r"C:\Windows\Fonts\msyh.ttc")
plt.rcParams["font.family"]="Microsoft YaHei"
plt.rcParams["axes.unicode_minus"]=False

SOURCE = Path(r"C:\Users\fuxing\Downloads\桌搭-整机外壳3d打印版.STEP")
OUT = Path(__file__).resolve().parent
R = math.sqrt(2)

def coord(p):
    return [p.x, (p.y-p.z)/R, (p.y+p.z)/R]

def bounds(shape):
    pts = np.array([coord(v.Point) for v in shape.Vertexes])
    return {"min": pts.min(axis=0).tolist(), "max": pts.max(axis=0).tolist()}

shape = Part.Shape()
shape.read(str(SOURCE))
report = {"source": str(SOURCE), "valid": shape.isValid(),
          "coordinates": "u=x, v=(y-z)/sqrt(2), depth=(y+z)/sqrt(2), mm",
          "solids": [], "screen_planes": []}
for i, solid in enumerate(shape.Solids, 1):
    report["solids"].append({"id": i, "volume_mm3": solid.Volume,
                              "valid": solid.isValid(), "bounds": bounds(solid)})
    if i != 1:
        continue
    for j, face in enumerate(solid.Faces, 1):
        if not isinstance(face.Surface, Part.Plane):
            continue
        normal = face.normalAt(0, 0)
        if abs(abs((normal.y+normal.z)/R)-1) > 1e-5 or face.Area < 100:
            continue
        report["screen_planes"].append({"face": j, "area": face.Area,
            "bounds": bounds(face), "wires": [bounds(w) for w in face.Wires]})

fig = plt.figure(figsize=(14, 9))
ax = fig.add_subplot(121, projection="3d")
colors = ["#c0c9d9", "#8897b2", "#eab779", "#eab779", "#eab779", "#91bfa6", "#d89595", "#bca2cf"]
for i, solid in enumerate(shape.Solids):
    vertices, triangles = solid.tessellate(0.25)
    vertices = np.array([coord(p) for p in vertices])
    mesh = Poly3DCollection(vertices[np.array(triangles)], facecolor=colors[i],
                            edgecolor="none", alpha=1, linewidths=0)
    mesh.set_zsort("average")
    ax.add_collection3d(mesh)
ax.set(xlim=(-48,48), ylim=(-35,66), zlim=(-32,38), xlabel="横向 / mm", ylabel="纵向 / mm", zlabel="深度 / mm", title="原始外壳：8 个实体")
ax.set_box_aspect((96,101,70))
ax.view_init(elev=62, azim=-70)
ax2 = fig.add_subplot(122)
for j in (43, 180, 194):
    face = shape.Solids[0].Faces[j-1]
    for wire in face.Wires:
        for edge in wire.Edges:
            p = np.array([coord(p) for p in edge.discretize(Deflection=0.05)])
            ax2.plot(p[:,0],p[:,1],lw=1, color={43:"#333333",180:"#087ab0",194:"#da7d19"}[j])
    ax2.plot([],[],color={43:"#333333",180:"#087ab0",194:"#da7d19"}[j], label=f"面 {j}，深度 {coord(face.CenterOfMass)[2]:.2f} mm")
ax2.set_aspect("equal")
ax2.set(xlabel="横向 / mm",ylabel="纵向 / mm",title="原屏幕面板与内部支撑轮廓")
ax2.grid(alpha=0.3)
ax2.legend(fontsize=8)
fig.tight_layout()
fig.savefig(OUT / "original-inspection.png", dpi=150)
plt.close(fig)
(OUT / "original-inspection.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
print(json.dumps(report["screen_planes"], indent=2))
