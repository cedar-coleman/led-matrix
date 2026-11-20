"""Drilling jigs for the 2020 frame (docs/assembly-guide.md, section 6).

Each fixed junction mount takes an M5 screw through a 6 mm hole drilled straight through a
seam rail or an end stile, front face to back face, on the profile's centreline. Every jig
is a saddle that fits over the 2020: it centres the drill on the slot and holds it square.

  drill_guide   one guide hole, with sight notches on both sides and the top: square a
                pencil line across the extrusion at the hole's position and line the notches
                up with it. For the seam rails' quad holes
  stile_jig     an end stop and two guide holes, STILE_HOLES from the stop. Butted against
                either end of an end stile it drills that end's two holes, so the four holes
                need no measuring
  mid_jig       a guide hole and an index hole MID_PITCH from it, for the optional mid-edge
                mounts: a 6 mm drill shank through the index hole into a drilled quad hole
                fixes the spacing
  pilot_sleeve  drops into any guide hole for a 3 mm pilot drill

Every jig prints top face down, as the STL is oriented, with the channel open upwards: no
supports. Printed plastic shrinks a little, so measure the stile jig's hole spacing before
use and set SHRINK if it's off.

    .tools/cad-venv/bin/python hardware/jigs.py

Writes hardware/stl/jig_*.stl and docs/diagrams/drilling-jigs.png.
"""
from pathlib import Path

import numpy as np
from manifold3d import Manifold

from mounts import SEGMENTS, write_stl

# The extrusion and the saddle (mm)
RAIL = 20.0           # 2020
CLEAR = 0.2           # channel clearance each side: a sliding fit
WALL = 4.0            # side legs
LEG = 12.0            # how far the legs reach down the extrusion's sides
TOP = 16.0            # top plate thickness: the length of the guide, which keeps the drill square
GUIDE_D = 6.3         # for a 6 mm drill (printed holes come out a little small)
NOTCH = 1.0           # sight notches' width and depth
SHRINK = 1.0          # scale along the jig to make up for shrinkage, e.g. 1.003 for 0.3%

# Hole positions on the frame (mm), from docs/assembly-guide.md section 6
STILE_HOLES = (12.0, 155.0)   # from either end of a 470 mm end stile (12, 155, 315, 458)
STOP = 5.0                    # end stop thickness
MID_PITCH = 151.0             # a quad hole to its neighbouring mid-edge hole
END = 12.0                    # jig body beyond its outermost hole

ROOT = Path(__file__).resolve().parent


def saddle(length, guides, stop=False, pins=()):
    """A jig in its working position: the extrusion along x, its top face at z = 0, centred on
    y = 0. Guides and pins are x positions; with a stop, the extrusion's end butts x = 0."""
    half = RAIL / 2 + CLEAR + WALL
    body = Manifold.cube((length, 2 * half, LEG + TOP)).translate((0, -half, -LEG))
    channel = Manifold.cube((length + 2, RAIL + 2 * CLEAR, LEG + 1)).translate((-1, -(RAIL / 2 + CLEAR), -LEG - 1))
    body -= channel
    if stop:
        body += Manifold.cube((STOP, 2 * half, LEG + TOP)).translate((-STOP, -half, -LEG))
    for x in list(guides) + list(pins):
        x *= SHRINK
        body -= Manifold.cylinder(TOP + 2, GUIDE_D / 2, circular_segments=SEGMENTS).translate((x, 0, -1))
    for x in guides:
        x *= SHRINK
        # sight notches: down both sides and across the top, through the hole's centre
        for side in (-1, 1):
            body -= Manifold.cube((NOTCH, NOTCH * 2, LEG + TOP + 2)).translate(
                (x - NOTCH / 2, side * half - NOTCH, -LEG - 1))
        body -= Manifold.cube((NOTCH, 2 * half + 2, NOTCH)).translate((x - NOTCH / 2, -half - 1, TOP - NOTCH))
    return body


def drill_guide():
    return saddle(2 * END + 16, [END + 8])


def stile_jig():
    return saddle(STILE_HOLES[1] + END, STILE_HOLES, stop=True)


def mid_jig():
    return saddle(MID_PITCH + 2 * END, [END], pins=[END + MID_PITCH])


def pilot_sleeve():
    """A sleeve for a 3 mm pilot drill: slides into a guide hole, flange on top."""
    od, flange = GUIDE_D - 0.15, 2.0
    body = Manifold.cylinder(TOP + flange, od / 2, circular_segments=SEGMENTS)
    body += Manifold.cylinder(flange, od / 2 + 2.5, circular_segments=SEGMENTS)
    return body - Manifold.cylinder(TOP + flange + 2, 3.4 / 2, circular_segments=SEGMENTS).translate((0, 0, -1))


def print_pose(m):
    """Top face down, channel open upwards (a rotation, not a mirror)."""
    return m.rotate((180, 0, 0))


def preview(path):
    """Top views of the three saddles with their hole positions, and the sleeve."""
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from matplotlib.patches import Circle, Rectangle

    half = RAIL / 2 + CLEAR + WALL
    jigs = [("drill_guide: square a pencil line across the extrusion, line up the notches",
             2 * END + 16, [END + 8], [], False),
            ("stile_jig: butt the stop against either end of an end stile; drill both holes",
             STILE_HOLES[1] + END, list(STILE_HOLES), [], True),
            ("mid_jig: 6 mm drill shank through the index hole into a quad hole; drill the guide",
             MID_PITCH + 2 * END, [END], [END + MID_PITCH], False)]
    fig, axes = plt.subplots(len(jigs), 1, figsize=(11, 7))
    for ax, (title, length, guides, pins, stop) in zip(axes, jigs):
        ax.add_patch(Rectangle((0, -half), length, 2 * half, fill=False, lw=1.5))
        if stop:
            ax.add_patch(Rectangle((-STOP, -half), STOP, 2 * half, color="#7a5520"))
            ax.annotate("stop", (-STOP / 2, half), xytext=(0, 6), textcoords="offset points", ha="center", fontsize=8)
        for x in guides:
            ax.add_patch(Circle((x, 0), GUIDE_D / 2, color="#d08c2e"))
            ax.annotate(f"{x:g}", (x, -half), xytext=(0, -12), textcoords="offset points", ha="center", fontsize=8)
            for side in (-1, 1):
                ax.plot([x, x], [side * half, side * (half - NOTCH)], color="k", lw=2)
        for x in pins:
            ax.add_patch(Circle((x, 0), GUIDE_D / 2, fill=False, lw=1.5, color="#333"))
            ax.annotate(f"index {x - guides[0]:g} from the guide", (x, -half), xytext=(0, -12),
                        textcoords="offset points", ha="center", fontsize=8)
        ax.set_xlim(-12, 200); ax.set_ylim(-half - 10, half + 10); ax.set_aspect("equal")
        ax.set_title(title, fontsize=9, loc="left")
        ax.tick_params(labelsize=7)
    fig.suptitle("Drilling jigs from above (mm): saddles over the 2020 that centre and square a 6 mm drill. "
                 "Orange: guide holes. Black ticks: sight notches.", fontsize=10)
    fig.tight_layout()
    fig.savefig(path, dpi=100)


def main():
    parts = {"drill_guide": drill_guide(), "stile_jig": stile_jig(), "mid_jig": mid_jig(),
             "pilot_sleeve": pilot_sleeve()}
    holes = {"drill_guide": 1, "stile_jig": 2, "mid_jig": 2, "pilot_sleeve": 1}
    for name, m in parts.items():
        assert m.status().name == "NoError", (name, m.status())
        assert m.genus() == holes[name], (name, m.genus())
        lo, hi = np.asarray(m.bounding_box()[:3]), np.asarray(m.bounding_box()[3:])
        assert (hi - lo)[0] <= 200, f"{name} is too long for a 200 mm print bed"
        write_stl(m if name == "pilot_sleeve" else print_pose(m), ROOT / "stl" / f"jig_{name}.stl")
        print(f"jig_{name:12s} {hi[0] - lo[0]:.0f} x {hi[1] - lo[1]:.0f} x {hi[2] - lo[2]:.0f} mm")
    preview(ROOT.parent / "docs" / "diagrams" / "drilling-jigs.png")


if __name__ == "__main__":
    main()
