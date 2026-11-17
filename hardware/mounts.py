"""Junction mounts that hold the panels to the wooden frame (docs/assembly-guide.md, section 6).

Each mount screws onto the panels' M3 posts from the back and carries an M5 heat-set insert,
which takes the M5 x 25 screw driven through the rail from behind. Three parts:

  quad    where four panel corners meet: four posts on a square, insert in the middle
  pair    two posts across a seam: at the wall's outer edges, and at the mid-edge posts
          along the row seams; the insert sits OFFSET to one side, clear of the screw heads
  corner  one post at each of the wall's four outer corners; insert OFFSET in on both axes

So every insert lands on a rail line: the quads and the side pairs on the row seams, the top
and bottom pairs and the corners OFFSET + POST_INSET (15 mm) in from the wall's top and
bottom edges.

The panel's posts sit about POST_INSET from each edge. That figure is scaled from the
supplier's photo of the back (320 mm across), so it is good to about 1 mm: print one quad and
test-fit it on four panels before printing the rest. If it is off, change POST_INSET and run
again:

    .tools/cad-venv/bin/python hardware/mounts.py

Writes hardware/stl/{quad,pair,corner}.stl and docs/diagrams/junction-mounts.png.
Needs manifold3d, numpy and matplotlib (see hardware/README.md).
"""
from pathlib import Path

import numpy as np
from manifold3d import Manifold

# Panel posts (mm)
POST_INSET = 6.0      # post centre from each panel edge, scaled from the supplier's photo
SEAM_GAP = 0.0        # gap between neighbouring panels; they butt together
PITCH = 2 * POST_INSET + SEAM_GAP   # post to post across a seam

# Plate and holes (mm)
PLATE_T = 9.0         # plate thickness
M3_HOLE = 3.4         # M3 clearance
HEAD_D = 6.2          # counterbore for an M3 socket head (5.5 mm), so no washer is needed
HEAD_DEPTH = 3.5      # leaves 5.5 mm under the head: an M3 x 10 goes 4.5 mm into the post
INSERT_D = 6.4        # M5 heat-set insert hole: use the insert maker's figure
INSERT_DEPTH = 7.5    # blind, from the back face; leaves a 1.5 mm floor
OFFSET = 9.0          # pair and corner: insert centre from the posts' line
MARGIN = 7.0          # plate edge beyond a hole centre, except on a wall edge
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


def part(outline, posts, insert):
    body = plate(*outline)
    for x, y in posts:
        body -= screw_hole(x, y)
    return body - insert_hole(*insert)


def quad():
    h = PITCH / 2
    e = h + MARGIN
    posts = [(sx * h, sy * h) for sx in (-1, 1) for sy in (-1, 1)]
    return part((-e, -e, e, e), posts, (0, 0)), posts, (0, 0)


def pair():
    # Posts along x; the wall's outer edge (or the seam's far side) is at -y.
    h = PITCH / 2
    e = h + MARGIN
    posts = [(-h, 0), (h, 0)]
    insert = (0, OFFSET)
    return part((-e, -POST_INSET, e, OFFSET + INSERT_D / 2 + 3.8), posts, insert), posts, insert


def corner():
    # The wall's outer corner is at (-POST_INSET, -POST_INSET).
    far = OFFSET + INSERT_D / 2 + 3.8
    insert = (OFFSET, OFFSET)
    return part((-POST_INSET, -POST_INSET, far, far), [(0, 0)], insert), [(0, 0)], insert


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
    tri = v[t]
    n = np.cross(tri[:, 1] - tri[:, 0], tri[:, 2] - tri[:, 0])
    n /= np.linalg.norm(n, axis=1, keepdims=True)
    rec = np.zeros(len(t), dtype=[("n", "<f4", 3), ("v", "<f4", (3, 3)), ("a", "<u2")])
    rec["n"], rec["v"] = n, tri
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "wb") as f:
        f.write(b"junction mount, esp32-gym-matrix hardware/mounts.py".ljust(80, b" "))
        f.write(np.uint32(len(t)).tobytes())
        f.write(rec.tobytes())


def preview(parts, path):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from matplotlib.patches import Circle
    from mpl_toolkits.mplot3d.art3d import Poly3DCollection

    fig = plt.figure(figsize=(12, 7.5))
    for col, (name, (m, posts, insert)) in enumerate(parts.items()):
        mesh = m.to_mesh()
        v = np.asarray(mesh.vert_properties)[:, :3]
        tri = v[np.asarray(mesh.tri_verts)]
        ax = fig.add_subplot(2, 3, col + 1, projection="3d")
        ax.add_collection3d(Poly3DCollection(tri, facecolor="#6fb3b8", edgecolor="#2d5f63", linewidths=0.05))
        lo, hi = v.min(0), v.max(0)
        c, s = (lo + hi) / 2, (hi - lo).max() / 2
        ax.set_xlim(c[0] - s, c[0] + s); ax.set_ylim(c[1] - s, c[1] + s); ax.set_zlim(0, 2 * s)
        ax.view_init(elev=35, azim=-60)
        ax.set_axis_off()
        ax.set_title(f"{name}: back face up", fontsize=11)

        ax = fig.add_subplot(2, 3, col + 4)
        x0, y0, x1, y1 = lo[0], lo[1], hi[0], hi[1]
        ax.add_patch(plt.Rectangle((x0, y0), x1 - x0, y1 - y0, fill=False, lw=1.5))
        for x, y in posts:
            ax.add_patch(Circle((x, y), HEAD_D / 2, fill=False, ls="--", color="#555"))
            ax.add_patch(Circle((x, y), M3_HOLE / 2, color="#333"))
            ax.annotate(f"({x:g}, {y:g})", (x, y), xytext=(0, -14), textcoords="offset points", ha="center", fontsize=7)
        ax.add_patch(Circle(insert, INSERT_D / 2, color="#d08c2e"))
        ax.annotate(f"M5 insert ({insert[0]:g}, {insert[1]:g})", insert, xytext=(0, 12), textcoords="offset points",
                    ha="center", fontsize=7)
        ax.set_xlim(x0 - 3, x1 + 3); ax.set_ylim(y0 - 3, y1 + 3); ax.set_aspect("equal")
        ax.set_title(f"{x1 - x0:.0f} x {y1 - y0:.0f} x {PLATE_T:g} mm, from the back", fontsize=9)
        ax.tick_params(labelsize=7)
    fig.suptitle(f"Junction mounts: M3 posts {POST_INSET:g} mm from each panel edge ({PITCH:g} mm across a seam)",
                 fontsize=12)
    fig.tight_layout()
    fig.savefig(path, dpi=110)


def main():
    parts = {"quad": quad(), "pair": pair(), "corner": corner()}
    for name, (m, posts, insert) in parts.items():
        assert m.status().name == "NoError", (name, m.status())
        # One through hole per post; the insert hole is blind and adds none.
        assert m.genus() == len(posts), (name, m.genus())
        web = wall_thickness(posts, insert)
        assert web >= 2.0, f"{name}: only {web:.1f} mm between holes"
        lo, hi = np.asarray(m.bounding_box()[:3]), np.asarray(m.bounding_box()[3:])
        write_stl(m, ROOT / "stl" / f"{name}.stl")
        print(f"{name:6s} {hi[0] - lo[0]:.0f} x {hi[1] - lo[1]:.0f} x {hi[2] - lo[2]:.0f} mm, "
              f"{m.volume() / 1000:.1f} cm3, thinnest web {web:.1f} mm, {m.num_tri()} triangles")
    preview(parts, ROOT.parent / "docs" / "diagrams" / "junction-mounts.png")


if __name__ == "__main__":
    main()
