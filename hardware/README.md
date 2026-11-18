# Printed parts

Junction mounts that hold the panels to the 2020 frame, and the blocks that hold the frame
45 mm off the wall. Where they go, and the screws they take, is in
[the assembly guide](../docs/assembly-guide.md), section 6.

![Junction mounts](../docs/diagrams/junction-mounts.png)

| File | Part | Print |
| --- | --- | --- |
| `stl/quad.stl` | Four panel corners; its two wide-spaced pairs of magnet-boss pockets go along the rows | 6 |
| `stl/pair_edge.stl` | Two posts across a seam on the wall's outer edge, with a lip down the panels' side: the left and right edges (onto the end stiles) and the top and bottom edges | 10 |
| `stl/pair.stl` | The same without the lip, for the optional mid-edge posts on the row seams | 8 (optional) |
| `stl/corner.stl` | One post at each outer corner of the wall, with lips down both outer sides | 4 |
| `stl/hanger.stl` | Top of each end stile: keyhole that hangs on a screw in the wall | 2 |
| `stl/bumper.stl` | Bottom of each end stile: rests on the wall | 2 |

![Wall blocks](../docs/diagrams/wall-blocks.png)

## Printing

- PETG or ASA, not PLA: the mounts carry the wall and sit on warm panel backs.
- Print flat, as the STL is oriented, with no supports. Mounts: back face down (the
  counterbores and the insert hole on the bed), so the lips and the magnet-boss pockets
  point up. Wall blocks: the wall face (counterbores and keyhole) down; the keyhole's
  pocket bridges 10 mm.
- 4 walls, 40% infill or more, 0.2 mm layers. The holes need to be round, so check that the
  slicer isn't using a coarse arc resolution.
- Press an M5 heat-set insert into each fixed mount's 6.4 mm hole (on the back face) with a
  soldering iron, flush with the face, and let it cool before loading it.

## Fit one before printing the rest

The posts' position, 6 mm in from each panel edge, is measured with a caliper on the panels
(5.8 to 6.5 mm), so it is good to about half a millimetre. Print one quad and one
`pair_edge`. Lay four panels face down with their corners together and screw the quad on
with M3 x 10 screws: all four should start without forcing, and the quad should sit flat on
the rims without rocking. Then fit the `pair_edge` at an outer edge: the lip should slide
down the panels' side without forcing and without play. If all of that holds, print the
rest.

If the screws don't line up, measure from a post's centre to the panel's edge and set
`POST_INSET`. If the quad rocks, a magnet boss stands taller than its pocket: measure how far
the bosses stand above the rim and set `BOSS_POCKET` a millimetre deeper (keep
`PLATE_T - INSERT_DEPTH - BOSS_POCKET` at 1 mm or more, raising `PLATE_T` if needed). If the
lip binds or rattles, change `LIP_GAP`. All are at the top of `mounts.py`; then build the
STLs again:

```sh
python3 -m venv .tools/cad-venv
.tools/cad-venv/bin/pip install manifold3d==3.5.5 numpy matplotlib
.tools/cad-venv/bin/python hardware/mounts.py
```

The script checks each part is a clean solid with one through hole per post and at least
2 mm of material between holes, then rewrites `stl/` and the drawing above. The other
settings (plate thickness, counterbore, insert hole, the insert's 9 mm offset) are at the
top of the same file too. The edge inserts sit `POST_INSET + OFFSET` (15 mm) in from the
wall's left and right edges, on the end stiles (10 to 30 mm in): keep that figure between
about 13 and 27 mm if you change either.
