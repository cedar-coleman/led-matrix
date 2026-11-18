"""Printed parts for the frame (docs/assembly-guide.md, section 6).

Junction mounts screw onto the panels' M3 posts from the back. Each carries an M5 heat-set
insert, which takes the M5 x 25 screw driven through the frame from behind. In wall terms,
x runs along the rows (the panels' long edges) and y up and down the wall:

  quad       where four panel corners meet: four posts, insert in the middle (on a seam
             rail). It has a direction: its pegs go along the rows
  pair_side  two corners across a row seam at the wall's left or right edge, with a lip down
             the panels' outer side; insert OFFSET in from the posts, on an end stile
  pair_top   two corners across a seam on the wall's top or bottom edge, with a lip; joins
             the panels only (no rail there unless you add one)
  pair_mid   two mid-edge posts across a row seam, no lip (optional); insert OFFSET along
             the seam, on a seam rail
  corner_a   one post at an outer corner, with lips down both outer sides; insert OFFSET in
  corner_b   on both axes, on an end stile. Mirror images (the peg points along the long
             edge): seen from behind, corner_a goes bottom-left and top-right, corner_b
             top-left and bottom-right

Measured on the panels: the plastic backs stop short of the LED board by about BACK_INSET
on every side, and each M3 post sits about EDGE from its own back's outer side. So across a
seam, with the LED faces butted, the posts are PITCH_X apart along the rows and PITCH_Y
apart across the row seams. A lip sits in the BACK_INSET step under the LED board's
overhang: it doesn't widen the wall or show. A lip only ever goes on the outside of the
wall: between two panels there's no room for one.

Every panel has a recessed magnet boss (a cup, WELL_D across) in line with each M3 post,
BOSS_ALONG from it along the long edge: towards the panel's middle at the corners, and to
one side at the mid-edge posts. Each mount has a short chamfered peg (PEG_D, PEG_H) under it
that drops into the cup, so the mount locates on the panel and can't twist. Across a row
seam the two mid-edge cups are on opposite sides; with the middle row upside down it comes
out the same at both seams: seen from behind, the upper panel's cup is right of its post and
the lower panel's left.

Every part prints back face down, as the STL is oriented: lips and pegs point up, no
supports.

Wall blocks hold the frame BLOCK_T off the wall, screwed into the end stiles' rear slots:

  hanger     top of each stile: a keyhole on the wall face hangs on a screw in the wall
  bumper     bottom of each stile: rests against the wall

Print one quad and one pair_side first and test-fit them (hardware/README.md). If anything
is off, change the measurements below and run again:

    .tools/cad-venv/bin/python hardware/mounts.py

Writes hardware/stl/*.stl, docs/diagrams/junction-mounts.png and docs/diagrams/wall-blocks.png.
Needs manifold3d, numpy and matplotlib (see hardware/README.md).
"""
from pathlib import Path

import numpy as np
from manifold3d import Manifold

# Panel posts, measured with a caliper (mm)
PITCH_X = 16.25       # post to post across a seam between panels side by side in a row
PITCH_Y = 16.0        # post to post across a row seam (long edges butted)
EDGE = 6.2            # post centre to its plastic back's outer side (measured 5.8 to 6.3)
BACK_INSET = min(PITCH_X, PITCH_Y) / 2 - EDGE   # the back's step in from the LED board's edge

# Plate and holes (mm)
PLATE_T = 12.0        # plate thickness: also the gap between the panel backs and the frame
M3_HOLE = 3.4         # M3 clearance
HEAD_D = 6.2          # counterbore for an M3 socket head (5.5 mm), so no washer is needed
HEAD_DEPTH = 6.5      # leaves 5.5 mm under the head: an M3 x 10 goes 4.5 mm into the post
INSERT_D = 6.4        # M5 heat-set insert hole: use the insert maker's figure
INSERT_DEPTH = 7.0    # blind, from the back face; leaves a 5 mm floor
OFFSET = 9.0          # pairs and corner: insert centre from the posts' line
MARGIN = 7.0          # plate edge beyond a hole centre, except on a wall edge
FAR = OFFSET + INSERT_D / 2 + 3.8   # plate edge beyond the posts' line, on the insert's side

# The panels' magnet bosses and the edge lips (mm)
BOSS_ALONG = 10.6     # magnet boss centre from its M3 post, along the panel's long edge
WELL_D = 12.0         # inside diameter of the boss's cup (measured)
WELL_DEPTH = 2.0      # the cup's depth (measured)
PEG_D = WELL_D - 0.8  # 0.4 mm clearance all round, for small errors in BOSS_ALONG
PEG_H = WELL_DEPTH - 0.4   # never bottoms out, so the plate sits on the rim
PEG_CHAMFER = 0.6     # lead-in on the peg's tip
PEG_REACH = BOSS_ALONG + PEG_D / 2 + 1.5   # plate edge beyond a post, on the side its peg is
LIP_T = 1.4           # lip thickness
LIP_DEPTH = 8.0       # how far the lip runs down the panel's side (the panel is about 15.5 mm deep)
LIP_GAP = 0.2         # clearance between the lip and the panel's side
LIP_OUT = EDGE + LIP_GAP + LIP_T   # plate edge to post line, on a lipped side
CORNER_R = 2.0        # plate corner radius
SEGMENTS = 64

ROOT = Path(__file__).resolve().parent


def plate(x0, y0, x1, y1):
    """Rounded rectangle from (x0, y0) to (x1, y1), PLATE_T thick, standing on z = 0."""
    r = CORNER_R
    posts = [Manifold.cylinder(PLATE_T, r, circular_segments=SEGMENTS).translate((x, y, 0))
             for x in (x0 + r, x1 - r) for y in (y0 + r, y1 - r)]
    return Manifold.batch_hull(posts)


def screw_hole(x, y):
    """M3 clearance through the plate, counterbored from the back face (z = PLATE_T)."""
    through = Manifold.cylinder(PLATE_T + 2, M3_HOLE / 2, circular_segments=SEGMENTS).translate((x, y, -1))
    head = Manifold.cylinder(HEAD_DEPTH + 1, HEAD_D / 2, circular_segments=SEGMENTS)
    return through + head.translate((x, y, PLATE_T - HEAD_DEPTH))


def insert_hole(x, y):
    hole = Manifold.cylinder(INSERT_DEPTH + 1, INSERT_D / 2, circular_segments=SEGMENTS)
    return hole.translate((x, y, PLATE_T - INSERT_DEPTH))


def part(outline, posts, insert, pegs=(), lips=()):
    """A mount in its working position: panel face at z = 0, back face at z = PLATE_T. Lips
    are (x0, y0, x1, y1) strips that run from the panel face down the panel's side."""
    body = plate(*outline)
    for x, y in posts:
        body -= screw_hole(x, y)
    body -= insert_hole(*insert)
    for x, y in pegs:
        r = PEG_D / 2
        body += Manifold.cylinder(PEG_H + 0.01, r - PEG_CHAMFER, r, circular_segments=SEGMENTS).translate(
            (x, y, -PEG_H))
    for x0, y0, x1, y1 in lips:
        body += Manifold.cube((x1 - x0, y1 - y0, LIP_DEPTH + 0.01)).translate((x0, y0, -LIP_DEPTH))
    return dict(m=body, posts=posts, insert=insert, pegs=list(pegs), lips=list(lips))


def quad():
    hx, hy = PITCH_X / 2, PITCH_Y / 2
    posts = [(sx * hx, sy * hy) for sx in (-1, 1) for sy in (-1, 1)]
    # Each panel's corner boss lies along the row, away from the crossing.
    pegs = [(sx * (hx + BOSS_ALONG), sy * hy) for sx in (-1, 1) for sy in (-1, 1)]
    rx = hx + PEG_REACH
    return part((-rx, -hy - MARGIN, rx, hy + MARGIN), posts, (0, 0), pegs)


def pair_side():
    # The wall's left or right side is at -x; the row seam runs along x through y = 0.
    hy = PITCH_Y / 2
    posts = [(0, -hy), (0, hy)]
    pegs = [(BOSS_ALONG, -hy), (BOSS_ALONG, hy)]
    lips = [(-LIP_OUT, -hy - MARGIN, -LIP_OUT + LIP_T, hy + MARGIN)]
    return part((-LIP_OUT, -hy - MARGIN, max(FAR, PEG_REACH), hy + MARGIN), posts, (OFFSET, 0), pegs, lips)


def pair_top():
    # The wall's top or bottom edge is at -y; the seam between the two panels runs along y.
    hx = PITCH_X / 2
    posts = [(-hx, 0), (hx, 0)]
    pegs = [(-hx - BOSS_ALONG, 0), (hx + BOSS_ALONG, 0)]
    reach = hx + PEG_REACH
    lips = [(-reach, -LIP_OUT, reach, -LIP_OUT + LIP_T)]
    return part((-reach, -LIP_OUT, reach, FAR), posts, (0, OFFSET), pegs, lips)


def pair_mid():
    # Mid-edge posts across a row seam (along x through y = 0), seen from behind with y up:
    # the upper panel's cup is right of its post, the lower panel's left. That holds at both
    # row seams because the middle row is upside down, and turning the part round keeps it.
    hy = PITCH_Y / 2
    posts = [(0, -hy), (0, hy)]
    pegs = [(BOSS_ALONG, hy), (-BOSS_ALONG, -hy)]
    return part((-PEG_REACH, -hy - MARGIN, PEG_REACH, hy + MARGIN), posts, (OFFSET, 0), pegs)


def _corner(peg):
    # The wall's outer corner is beyond (-EDGE, -EDGE); lips down both outer sides; the peg
    # points along the panel's long edge. Seen from behind with y up, this fits the
    # bottom-left corner as drawn and the top-right turned half round.
    far = max(FAR, PEG_REACH)
    lips = [(-LIP_OUT, -LIP_OUT, far, -LIP_OUT + LIP_T), (-LIP_OUT, -LIP_OUT, -LIP_OUT + LIP_T, far)]
    return part((-LIP_OUT, -LIP_OUT, far, far), [(0, 0)], (OFFSET, OFFSET), [peg], lips)


def corner_a():
    return _corner((BOSS_ALONG, 0))   # bottom-left and top-right, seen from behind


def corner_b():
    return _corner((0, BOSS_ALONG))   # top-left and bottom-right: the mirror image


def print_pose(m):
    """Back face down: turn the part over (a rotation, not a mirror) and stand it on z = 0."""
    return m.rotate((180, 0, 0)).translate((0, 0, PLATE_T))


# Wall blocks (mm). x across the stile, y up the wall, z from the wall face (z = 0, printed
# face down) to the stile's back (z = BLOCK_T).
BLOCK_T = 45.0        # the gap between the wall and the frame: a supply (30 mm) on its adapter, plus air
BLOCK_W = 20.0        # a 2020 stile's width
M5_HOLE = 5.5
M5_HEAD_D = 9.5       # counterbore for an M5 socket head, on the wall face
M5_HEAD_DEPTH = BLOCK_T - 24.0  # deep counterbore: 24 mm under the head, so an M5 x 30 goes about 6 mm into a T-nut
KEY_ENTRY_D = 10.0    # takes a #8 or #10 pan head (9.5 mm or less)
KEY_SLOT_W = 5.5      # takes the screw's shank
KEY_DROP = 18.0       # how far the block drops onto the screw
KEY_LIP = 3.0         # plastic in front of the head
KEY_POCKET = 4.0      # room for the head behind the lip (pan heads are about 3 mm tall)
HANGER_H, HANGER_HOLES, KEY_ENTRY_Y = 80.0, (10.0, 70.0), 30.0
BUMPER_H, BUMPER_HOLES = 40.0, (20.0,)


def block(height, holes):
    """A BLOCK_W x height x BLOCK_T block with M5 holes at heights `holes`, counterbored from
    the wall face, for screws into T-nuts in the stile's rear slot."""
    body = Manifold.cube((BLOCK_W, height, BLOCK_T))
    for y in holes:
        body -= Manifold.cylinder(BLOCK_T + 2, M5_HOLE / 2, circular_segments=SEGMENTS).translate((BLOCK_W / 2, y, -1))
        body -= Manifold.cylinder(M5_HEAD_DEPTH + 1, M5_HEAD_D / 2, circular_segments=SEGMENTS).translate(
            (BLOCK_W / 2, y, -1))
    return body


def hanger():
    """Top of a stile. The block drops KEY_DROP onto a screw head in the wall: the head goes
    in at the entry hole, the block slides down so the shank rides to the top of the slot,
    and the head sits behind the lip, so the block hangs from it."""
    entry_y = KEY_ENTRY_Y
    top_y = entry_y + KEY_DROP          # y runs up the wall: the screw ends above the entry
    body = block(HANGER_H, HANGER_HOLES)
    cx = BLOCK_W / 2
    body -= Manifold.cylinder(KEY_LIP + KEY_POCKET + 1, KEY_ENTRY_D / 2, circular_segments=SEGMENTS).translate(
        (cx, entry_y, -1))
    body -= Manifold.cube((KEY_SLOT_W, KEY_DROP, KEY_LIP + 2)).translate((cx - KEY_SLOT_W / 2, entry_y, -1))
    body -= Manifold.cylinder(KEY_LIP + 1, KEY_SLOT_W / 2, circular_segments=SEGMENTS).translate((cx, top_y, -1))
    pocket = Manifold.cube((KEY_ENTRY_D, KEY_DROP + KEY_ENTRY_D / 2 + 1, KEY_POCKET))
    body -= pocket.translate((cx - KEY_ENTRY_D / 2, entry_y, KEY_LIP))
    return body, 2


def bumper():
    """Bottom of an end stile: holds it BLOCK_T off the wall."""
    return block(BUMPER_H, BUMPER_HOLES), 1


def wall_thickness(posts, insert):
    """Thinnest material between holes, and between each hole and the plate's edge is
    checked by the genus and the bounding box below; this is the hole-to-hole web."""
    webs = [np.hypot(x - insert[0], y - insert[1]) - HEAD_D / 2 - INSERT_D / 2 for x, y in posts]
    webs += [np.hypot(a[0] - b[0], a[1] - b[1]) - HEAD_D for i, a in enumerate(posts) for b in posts[i + 1:]]
    return min(webs)


def write_stl(manifold, path):
    mesh = manifold.to_mesh()
    v = np.asarray(mesh.vert_properties)[:, :3]
    t = np.asarray(mesh.tri_verts)
    v = v - v.min(axis=0)   # every coordinate positive, the part's corner at the origin
    tri = v[t]
    n = np.cross(tri[:, 1] - tri[:, 0], tri[:, 2] - tri[:, 0])
    n /= np.linalg.norm(n, axis=1, keepdims=True)
    rec = np.zeros(len(t), dtype=[("n", "<f4", 3), ("v", "<f4", (3, 3)), ("a", "<u2")])
    rec["n"], rec["v"] = n, tri
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "wb") as f:
        f.write(b"binary STL, esp32-gym-matrix hardware/mounts.py, mm".ljust(80, b"\0"))
        f.write(np.uint32(len(t)).tobytes())
        f.write(rec.tobytes())


def preview(parts, path):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from matplotlib.patches import Circle
    from mpl_toolkits.mplot3d.art3d import Poly3DCollection

    fig = plt.figure(figsize=(3.6 * len(parts), 7.5))
    for col, (name, d) in enumerate(parts.items()):
        m, posts, insert = print_pose(d["m"]), d["posts"], d["insert"]
        mesh = m.to_mesh()
        v = np.asarray(mesh.vert_properties)[:, :3]
        tri = v[np.asarray(mesh.tri_verts)]
        ax = fig.add_subplot(2, len(parts), col + 1, projection="3d")
        ax.add_collection3d(Poly3DCollection(tri, facecolor="#6fb3b8", edgecolor="#2d5f63", linewidths=0.05))
        lo, hi = v.min(0), v.max(0)
        c, s = (lo + hi) / 2, (hi - lo).max() / 2
        ax.set_xlim(c[0] - s, c[0] + s); ax.set_ylim(c[1] - s, c[1] + s); ax.set_zlim(0, 2 * s)
        ax.view_init(elev=35, azim=-60)
        ax.set_axis_off()
        ax.set_title(f"{name}: as printed, back face down", fontsize=11)

        ax = fig.add_subplot(2, len(parts), col + 1 + len(parts))
        blo, bhi = d["m"].bounding_box()[:3], d["m"].bounding_box()[3:]
        x0, y0, x1, y1 = blo[0], blo[1], bhi[0], bhi[1]
        for x, y in d["pegs"]:
            ax.add_patch(Circle((x, y), PEG_D / 2, color="#9ccfd2"))
        for lx0, ly0, lx1, ly1 in d["lips"]:
            ax.add_patch(plt.Rectangle((lx0, ly0), lx1 - lx0, ly1 - ly0, color="#6fb3b8"))
        ax.add_patch(plt.Rectangle((x0, y0), x1 - x0, y1 - y0, fill=False, lw=1.5))
        for x, y in posts:
            ax.add_patch(Circle((x, y), HEAD_D / 2, fill=False, ls="--", color="#555"))
            ax.add_patch(Circle((x, y), M3_HOLE / 2, color="#333"))
            ax.annotate(f"({x:g}, {y:g})", (x, y), xytext=(0, -14), textcoords="offset points", ha="center", fontsize=7)
        ax.add_patch(Circle(insert, INSERT_D / 2, color="#d08c2e"))
        ax.annotate(f"M5 insert ({insert[0]:g}, {insert[1]:g})", insert, xytext=(0, 12), textcoords="offset points",
                    ha="center", fontsize=7)
        ax.set_xlim(x0 - 3, x1 + 3); ax.set_ylim(y0 - 3, y1 + 3); ax.set_aspect("equal")
        lip = f" + {LIP_DEPTH:g} mm lip" if d["lips"] else ""
        ax.set_title(f"{x1 - x0:.0f} x {y1 - y0:.0f} x {PLATE_T:g} mm{lip}, seen from the back", fontsize=9)
        ax.tick_params(labelsize=7)
    fig.suptitle(f"Junction mounts, seen from the back, x along the rows: posts {PITCH_X:g} mm apart along the rows, "
                 f"{PITCH_Y:g} mm across the row seams. Light circles: pegs into the magnet-boss cups. Shaded: lips.",
                 fontsize=12)
    fig.tight_layout()
    fig.savefig(path, dpi=110)


def preview_blocks(path):
    """Face views of the two wall blocks, drawn from the same constants as the solids."""
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from matplotlib.patches import Circle, Rectangle

    fig, axes = plt.subplots(1, 2, figsize=(8, 6))
    cx = BLOCK_W / 2
    for ax, (name, height, holes) in zip(axes, (("hanger (top of each stile)", HANGER_H, HANGER_HOLES),
                                               ("bumper (bottom of each stile)", BUMPER_H, BUMPER_HOLES))):
        ax.add_patch(Rectangle((0, 0), BLOCK_W, height, fill=False, lw=1.5))
        for y in holes:
            ax.add_patch(Circle((cx, y), M5_HEAD_D / 2, fill=False, ls="--", color="#555"))
            ax.add_patch(Circle((cx, y), M5_HOLE / 2, color="#333"))
            ax.annotate(f"M5, y = {y:g}", (cx, y), xytext=(14, -3), textcoords="offset points", fontsize=7)
        if height == HANGER_H:
            top = KEY_ENTRY_Y + KEY_DROP
            ax.add_patch(Rectangle((cx - KEY_ENTRY_D / 2, KEY_ENTRY_Y), KEY_ENTRY_D,
                                   KEY_DROP + KEY_ENTRY_D / 2 + 1, fill=False, ls=":", color="#d08c2e"))
            ax.add_patch(Rectangle((cx - KEY_SLOT_W / 2, KEY_ENTRY_Y), KEY_SLOT_W, KEY_DROP, color="#d08c2e"))
            ax.add_patch(Circle((cx, top), KEY_SLOT_W / 2, color="#d08c2e"))
            ax.add_patch(Circle((cx, KEY_ENTRY_Y), KEY_ENTRY_D / 2, color="#d08c2e"))
            ax.annotate("keyhole: in here,", (cx, KEY_ENTRY_Y), xytext=(14, -3), textcoords="offset points",
                        fontsize=7)
            ax.annotate(f"block drops {KEY_DROP:g} mm:\nscrew head ends here,\nbehind the lip", (cx, top), xytext=(14, -6),
                        textcoords="offset points", fontsize=7)
        ax.set_xlim(-5, BLOCK_W + 40); ax.set_ylim(-5, HANGER_H + 5); ax.set_aspect("equal")
        ax.set_title(f"{name}\n{BLOCK_W:g} x {height:g} x {BLOCK_T:g} mm, wall face, top up", fontsize=9)
        ax.tick_params(labelsize=7)
    fig.suptitle(f"Wall blocks: hold the stiles {BLOCK_T:g} mm off the wall (M5 x 30 into T-nuts in the stile)",
                 fontsize=11)
    fig.tight_layout()
    fig.savefig(path, dpi=110)


def main():
    parts = {"quad": quad(), "pair_side": pair_side(), "pair_top": pair_top(), "pair_mid": pair_mid(),
             "corner_a": corner_a(), "corner_b": corner_b()}
    for name, d in parts.items():
        m, posts, insert = d["m"], d["posts"], d["insert"]
        assert m.status().name == "NoError", (name, m.status())
        # One through hole per post; the insert hole is blind and adds none.
        assert m.genus() == len(posts), (name, m.genus())
        web = wall_thickness(posts, insert)
        assert web >= 2.0, f"{name}: only {web:.1f} mm between holes"
        lo, hi = np.asarray(m.bounding_box()[:3]), np.asarray(m.bounding_box()[3:])
        # The lip fits in the back's step; each peg stands clear of the screw holes.
        assert LIP_GAP + LIP_T <= BACK_INSET + 0.05, "the lip would stand outside the LED board's edge"
        for px, py in d["pegs"]:
            gap = min(np.hypot(px - x, py - y) for x, y in posts) - PEG_D / 2 - M3_HOLE / 2
            assert gap >= 1.0, f"{name}: a peg crowds a screw hole ({gap:.1f} mm)"
            assert lo[0] + 1 <= px - PEG_D / 2 and px + PEG_D / 2 <= hi[0] - 1 and \
                lo[1] + 1 <= py - PEG_D / 2 and py + PEG_D / 2 <= hi[1] - 1, f"{name}: a peg overhangs the plate"
        write_stl(print_pose(m), ROOT / "stl" / f"{name}.stl")
        print(f"{name:9s} {hi[0] - lo[0]:.0f} x {hi[1] - lo[1]:.0f} x {hi[2] - lo[2]:.0f} mm, "
              f"{m.volume() / 1000:.1f} cm3, thinnest web {web:.1f} mm, {m.num_tri()} triangles")
    preview(parts, ROOT.parent / "docs" / "diagrams" / "junction-mounts.png")
    for name, (m, holes) in {"hanger": hanger(), "bumper": bumper()}.items():
        assert m.status().name == "NoError", (name, m.status())
        # One through hole per M5 screw; the keyhole is open only on the wall face.
        assert m.genus() == holes, (name, m.genus())
        write_stl(m, ROOT / "stl" / f"{name}.stl")
        print(f"{name:9s} {m.volume() / 1000:.1f} cm3, {m.num_tri()} triangles")
    preview_blocks(ROOT.parent / "docs" / "diagrams" / "wall-blocks.png")


if __name__ == "__main__":
    main()
