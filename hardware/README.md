# Printed parts

Junction mounts that hold the panels to the wooden frame. Where they go, and the screws they
take, is in [the assembly guide](../docs/assembly-guide.md), section 6.

![Junction mounts](../docs/diagrams/junction-mounts.png)

| File | Part | Print |
| --- | --- | --- |
| `stl/quad.stl` | Four panel corners | 6 |
| `stl/pair.stl` | Two posts across a seam: the wall's edges, and the optional mid-edge posts | 10, or 18 with the optional ones |
| `stl/corner.stl` | One post at each outer corner of the wall | 4 |

## Printing

- PETG or ASA, not PLA: the mounts carry the wall and sit on warm panel backs.
- Print flat, as the STL is oriented: the face with the counterbores and the insert hole up.
  No supports.
- 4 walls, 40% infill or more, 0.2 mm layers. The holes need to be round, so check that the
  slicer isn't using a coarse arc resolution.
- Press an M5 heat-set insert into each mount's 6.4 mm hole with a soldering iron, flush with
  the top face, and let it cool before loading it.

## Fit one before printing the rest

The posts' position, 6 mm in from each panel edge, is scaled from the supplier's photo, so it
is good to about 1 mm. Print one quad, lay four panels face down with their corners together,
and screw the quad on with M3 x 10 screws. If all four start without forcing, print the rest.

If they don't line up, measure from a post's centre to the panel's edge, set `POST_INSET` at
the top of `mounts.py`, and build the STLs again:

```sh
python3 -m venv .tools/cad-venv
.tools/cad-venv/bin/pip install manifold3d==3.5.5 numpy matplotlib
.tools/cad-venv/bin/python hardware/mounts.py
```

The script checks each part is a clean solid with one through hole per post and at least
2 mm of material between holes, then rewrites `stl/` and the drawing above. The other
settings (plate thickness, counterbore, insert hole, the insert's 9 mm offset) are at the top
of the same file. The edge inserts sit `POST_INSET + OFFSET` (15 mm) in from the wall's top
and bottom edges, on the 38 mm rails flush with those edges: keep that figure under about
30 mm if you change either.
