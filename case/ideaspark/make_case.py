#!/usr/bin/env python3
"""Build the Glucoled ideaspark case STLs from the original design in source/.

Original: "BOX for Ideaspark ESP32 DEV BOARD" by PS_Diegooz, CC BY-SA
https://www.thingiverse.com/thing:6940341

    python3 -m venv .venv && .venv/bin/pip install manifold3d fonttools numpy
    .venv/bin/python make_case.py

Changes from the original:
  - inches -> millimetres; body and end plug split into separate files
  - inside width +0.5 mm (side walls out 0.25 mm each) for printing slack; plug widened to match
  - depth unchanged (15.2 mm inside: right for the board in its acrylic plates, keeps USB aligned)
  - "GlucoLED" raised 0.6 mm in Courier New Bold on the strip under the screen
"""
import os, struct
import numpy as np
from manifold3d import Manifold, Mesh, CrossSection, FillRule
from fontTools.ttLib import TTFont
from fontTools.pens.basePen import BasePen

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "source")

DEPTH_ADD = 0.0      # mm added at the back (tried 1.4: board sat too far back, USB misaligned)
WIDTH_ADD = 0.25     # mm each side
TEXT, EM, RAISE = "GlucoLED", 6.0, 0.6
FONT = "/System/Library/Fonts/Supplemental/Courier New Bold.ttf"
FACE_Y = 9.7                     # front face of the body
WINDOW_Z = (18.0, 61.0)          # screen window extent along the length
STRIP_X = (11.65, 18.25)         # strip between window edge and the long edge (text side)


def read_stl_inches(path):
    d = open(path, "rb").read()
    n = struct.unpack("<I", d[80:84])[0]
    return [[tuple(c * 25.4 for c in struct.unpack("<3f", d[84 + i * 50 + 12 + k * 12:84 + i * 50 + 24 + k * 12]))
             for k in range(3)] for i in range(n)]


def read_obj(path):
    V, T = [], []
    for line in open(path):
        if line.startswith("v "):
            V.append(tuple(map(float, line.split()[1:4])))
        elif line.startswith("f "):
            idx = [int(x.split("/")[0]) - 1 for x in line.split()[1:]]
            T += [[V[idx[0]], V[idx[k]], V[idx[k + 1]]] for k in range(1, len(idx) - 1)]
    return T


def to_manifold(T):
    raw = np.array(T, dtype=np.float64).reshape(-1, 3)
    uniq, inv = np.unique(np.round(raw, 4), axis=0, return_inverse=True)
    return Manifold(Mesh(vert_properties=uniq.astype(np.float32), tri_verts=inv.reshape(-1, 3).astype(np.uint32)))


def write_stl(path, m, name):
    mesh = m.to_mesh()
    V = np.asarray(mesh.vert_properties)[:, :3]
    F = np.asarray(mesh.tri_verts)
    lo = V.min(axis=0)
    V = V - [(lo[0] + V[:, 0].max()) / 2, (lo[1] + V[:, 1].max()) / 2, lo[2]]   # centred, on the bed
    with open(path, "wb") as f:
        f.write(name.encode()[:80].ljust(80, b" "))
        f.write(struct.pack("<I", len(F)))
        for t in F:
            a, b, c = V[t[0]], V[t[1]], V[t[2]]
            n = np.cross(b - a, c - a)
            n = n / (np.linalg.norm(n) or 1)
            f.write(struct.pack("<12fH", *n, *a, *b, *c, 0))
    size = V.max(axis=0) - V.min(axis=0)
    print(f"{os.path.basename(path)}: {len(F)} triangles, {size[0]:.1f} x {size[1]:.1f} x {size[2]:.1f} mm")


class FlatPen(BasePen):
    def __init__(self, gs):
        super().__init__(gs)
        self.contours = []

    def _moveTo(self, p):
        self.contours.append([p])

    def _lineTo(self, p):
        self.contours[-1].append(p)

    def _qCurveToOne(self, p1, p2):
        p0 = self.contours[-1][-1]
        for i in range(1, 9):
            t = i / 8
            self.contours[-1].append(tuple((1 - t) ** 2 * p0[j] + 2 * (1 - t) * t * p1[j] + t * t * p2[j] for j in (0, 1)))

    def _curveToOne(self, p1, p2, p3):
        p0 = self.contours[-1][-1]
        for i in range(1, 11):
            t = i / 10
            m = 1 - t
            self.contours[-1].append(tuple(m ** 3 * p0[j] + 3 * m * m * t * p1[j] + 3 * m * t * t * p2[j] + t ** 3 * p3[j]
                                           for j in (0, 1)))


def text_solid():
    font = TTFont(FONT)
    gs, cmap, upm = font.getGlyphSet(), font.getBestCmap(), font["head"].unitsPerEm
    polys, x0 = [], 0
    for ch in TEXT:
        g = cmap[ord(ch)]
        pen = FlatPen(gs)
        gs[g].draw(pen)
        for c in pen.contours:
            pts = [((x + x0) * EM / upm, y * EM / upm) for x, y in c]
            if len(pts) > 2 and pts[0] == pts[-1]:
                pts = pts[:-1]
            polys.append(pts)
        x0 += gs[g].width
    cs = CrossSection(polys, FillRule.NonZero)
    b = cs.bounds()
    cs = cs.translate((-(b[0] + b[2]) / 2, -(b[1] + b[3]) / 2))
    # 2D u (reading direction) -> -z (USB end on the right), v (up) -> -x, extrusion -> +y (out of the front)
    m = [[0, -1, 0, sum(STRIP_X) / 2],
         [0, 0, 1, FACE_Y - 0.3],
         [-1, 0, 0, sum(WINDOW_Z) / 2]]
    return cs.extrude(RAISE + 0.3).transform(m)   # 0.3 mm sunk into the face for a solid join


def main():
    tris = read_stl_inches(os.path.join(SRC, "BOX_NERDMINER_body.stl"))
    body = [t for t in tris if min(v[0] for v in t) > -30]
    plug = [t for t in tris if min(v[0] for v in t) <= -30]

    def fix_body(v):
        x, y, z = v
        if y < -5:
            y -= DEPTH_ADD
        if x > 13:
            x += WIDTH_ADD
        elif x < -13:
            x -= WIDTH_ADD
        return (x, y, z)

    body = [[fix_body(v) for v in t] for t in body]
    # plug: x = depth, y = width, z = thickness; scale about its own centre
    px = [v[0] for t in plug for v in t]
    py = [v[1] for t in plug for v in t]
    cx, cy = (min(px) + max(px)) / 2, (min(py) + max(py)) / 2
    kx = (max(px) - min(px) + DEPTH_ADD) / (max(px) - min(px))
    ky = (max(py) - min(py) + 2 * WIDTH_ADD) / (max(py) - min(py))
    plug = [[(cx + (v[0] - cx) * kx, cy + (v[1] - cy) * ky, v[2]) for v in t] for t in plug]

    body_m = to_manifold(body)
    write_stl(os.path.join(HERE, "glucoled-ideaspark-body.stl"), body_m + text_solid(),
              "Glucoled ideaspark body, GlucoLED text (from PS_Diegooz thing:6940341, CC BY-SA)")
    write_stl(os.path.join(HERE, "glucoled-ideaspark-body-plain.stl"), body_m,
              "Glucoled ideaspark body, no text (from PS_Diegooz thing:6940341, CC BY-SA)")
    write_stl(os.path.join(HERE, "glucoled-ideaspark-plug.stl"), to_manifold(plug),
              "Glucoled ideaspark end plug (from PS_Diegooz thing:6940341, CC BY-SA)")
    write_stl(os.path.join(HERE, "glucoled-ideaspark-stand.stl"), to_manifold(read_obj(os.path.join(SRC, "BOX_NERDMINER_PIEDISTALLO.obj"))),
              "Glucoled ideaspark stand (from PS_Diegooz thing:6940341, CC BY-SA)")


if __name__ == "__main__":
    main()
