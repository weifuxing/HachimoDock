"""Exterior-only corner patches, retaining the proven functional geometry.

Each patch lies outside the old analytic exterior envelope.  The slight
0.20 mm overlap is buried in the existing outer skin for robust boolean union.
"""
import math
import FreeCAD as App
import Part

SCREEN_OLD_R=13.0
BUTTON_OLD_R=12.0
NEW_R=10.0
EDGE_ROLL_R=5.5
SCREEN_TOP=65.34671709

def rounded_envelope(radius,low,high,inset=0.0):
    width=88.5-2*inset
    shape=Part.makeBox(width,high-low-2*inset,15.5,
                       App.Vector(-44.25+inset,low+inset,-15.5))
    vertical=[e for e in shape.Edges if e.BoundBox.XLength<1e-7 and e.BoundBox.YLength<1e-7]
    shape=shape.makeFillet(radius-inset,vertical)
    rims=[e for e in shape.Edges if e.BoundBox.ZLength<1e-7]
    shape=shape.makeFillet(EDGE_ROLL_R,rims)
    assert shape.isValid() and len(shape.Solids)==1
    return shape

def make_patches():
    patches=[]
    zones=[]
    evidence=[]
    for name,old_r,low,high,side in [
        ("screen",SCREEN_OLD_R,SCREEN_TOP-120,SCREEN_TOP,"upper"),
        ("buttons",BUTTON_OLD_R,-32.5,87.5,"lower")]:
        new=rounded_envelope(NEW_R,low,high)
        old=rounded_envelope(old_r,low,high)
        overlap=rounded_envelope(old_r,low,high,.20)
        if side=="upper":
            cutoff=high-old_r
            zone=Part.makeBox(100,old_r+1,18,App.Vector(-50,cutoff,-16))
        else:
            cutoff=low+old_r
            zone=Part.makeBox(100,old_r+1,18,App.Vector(-50,low-1,-16))
        band=new.cut(overlap).common(zone)
        for side_x in [-1,1]:
            start_x=-50 if side_x<0 else 44.25-old_r
            stop_x=-(44.25-old_r) if side_x<0 else 50
            xclip=Part.makeBox(stop_x-start_x,200,30,App.Vector(start_x,-80,-20))
            patch=band.common(xclip)
            corner_zone=zone.common(xclip)
            assert patch.isValid() and len(patch.Solids)==1
            if name=="buttons":
                # Button coordinates: x, v=-original Z, d=original Y.
                for obj in [patch,corner_zone]:
                    obj.rotate(App.Vector(),App.Vector(1,0,0),-90)
                    obj.rotate(App.Vector(),App.Vector(1,0,0),45)
            patches.append(patch)
            zones.append(corner_zone)
            evidence.append({"region":name,"side":"left" if side_x<0 else "right","old_outer_radius_mm":old_r,
                         "new_outer_radius_mm":NEW_R,
                         "front_flat_corner_radius_mm":NEW_R-EDGE_ROLL_R,
                         "edge_roll_radius_mm":EDGE_ROLL_R,"patch_volume_mm3":patch.Volume})
    return patches,zones,evidence

# Final union is performed by option1-r10/build_option1.py. The old STEP's
# curve-on-surface parameterizations do not support a reliable native OCCT fuse.
