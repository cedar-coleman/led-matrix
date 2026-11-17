"""Draw the assembly diagrams in docs/diagrams/ as SVG (stdlib only).

Run: python3 -I docs/diagrams/make_diagrams.py
The layout follows firmware/lib/render/src/panel_map.h: the board plugs into the front
top-right panel (top LEFT seen from behind), rows 1 and 3 upright, row 2 upside down.
All rear views are drawn as you see the wall standing behind it.
"""
from pathlib import Path

OUT = Path(__file__).resolve().parent
PW, PH = 320, 160          # panel size in mm, 1 SVG unit = 1 mm in the wall drawings
COLS, ROWS = 4, 3
RED, BLACK, BLUE = "#d62828", "#222222", "#1f6fd1"
ORANGE, GREY, TEAL = "#f08c00", "#8a8a8a", "#009696"
FUSE = "#f2c14e"


class Svg:
    def __init__(self, w, h):
        self.w, self.h, self.parts = w, h, []

    def add(self, s):
        self.parts.append(s)

    def rect(self, x, y, w, h, fill="none", stroke=BLACK, sw=2, rx=0, dash=None, opacity=1):
        d = f' stroke-dasharray="{dash}"' if dash else ""
        self.add(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{rx}" fill="{fill}" '
                 f'stroke="{stroke}" stroke-width="{sw}" fill-opacity="{opacity}"{d}/>')

    def line(self, pts, stroke=BLACK, sw=3, dash=None, arrow=False):
        d = f' stroke-dasharray="{dash}"' if dash else ""
        m = ' marker-end="url(#arrow)"' if arrow else ""
        p = " ".join(f"{x},{y}" for x, y in pts)
        self.add(f'<polyline points="{p}" fill="none" stroke="{stroke}" stroke-width="{sw}" '
                 f'stroke-linejoin="round" stroke-linecap="round"{d}{m}/>')

    def text(self, x, y, s, size=18, anchor="middle", fill=BLACK, weight="normal"):
        s = s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
        self.add(f'<text x="{x}" y="{y}" font-size="{size}" text-anchor="{anchor}" fill="{fill}" '
                 f'font-weight="{weight}" font-family="Helvetica, Arial, sans-serif">{s}</text>')

    def circle(self, x, y, r, fill=BLACK, stroke="none"):
        self.add(f'<circle cx="{x}" cy="{y}" r="{r}" fill="{fill}" stroke="{stroke}" stroke-width="2"/>')

    def save(self, name):
        head = (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {self.w} {self.h}" '
                f'width="{self.w}" height="{self.h}">'
                '<defs><marker id="arrow" viewBox="0 0 10 10" refX="8" refY="5" markerWidth="6" '
                'markerHeight="6" orient="auto-start-reverse"><path d="M0,0 L10,5 L0,10 z" '
                'fill="context-stroke"/></marker></defs>'
                f'<rect width="{self.w}" height="{self.h}" fill="white"/>')
        (OUT / name).write_text(head + "".join(self.parts) + "</svg>\n")


# ---- the wall seen from behind -------------------------------------------------------

def chain_number(bcol, row):
    """Chain number (1 = board's panel) of the panel at rear-view column bcol."""
    fcol = COLS - 1 - bcol
    idx = row * COLS + (COLS - 1 - fcol) if row % 2 == 0 else row * COLS + fcol
    return idx + 1


def upside_down(row):
    return row % 2 == 1


def connector_x(bcol, row, which):
    """x of a panel's IN or OUT header seen from behind (upright: IN left, OUT right)."""
    left = bcol * PW + 52
    right = bcol * PW + PW - 52
    if upside_down(row):
        left, right = right, left
    return left if which == "in" else right


def supply_of(n):
    return "A" if n <= 6 else "B"


def draw_wall(s, ox, oy, tag=None, single=False):
    for row in range(ROWS):
        for bcol in range(COLS):
            x, y = ox + bcol * PW, oy + row * PH
            n = chain_number(bcol, row)
            tint = "#fdeaea" if (single or supply_of(n) == "A") else "#e8f0fb"
            s.rect(x, y, PW, PH, fill=tint, stroke=GREY, sw=2)
            s.text(x + PW / 2, y + 34, f"Panel {n}", size=22, weight="bold")
            s.text(x + PW / 2, y + 56, "upside down" if upside_down(row) else "upright", size=15, fill=GREY)
            # orientation arrow (the panel's own up arrow)
            ax = x + PW / 2 + 70
            if upside_down(row):
                s.line([(ax, y + 22), (ax, y + 52)], stroke=GREY, sw=3, arrow=True)
            else:
                s.line([(ax, y + 52), (ax, y + 22)], stroke=GREY, sw=3, arrow=True)
            # headers
            for which, label in (("in", "IN"), ("out", "OUT")):
                cx = ox + connector_x(bcol, row, which)
                s.rect(cx - 20, y + PH / 2 - 10, 40, 20, fill="#cfe0f7", stroke=BLUE, sw=2)
                s.text(cx, y + PH / 2 + 30, label, size=14, fill=BLUE, weight="bold")
            # power socket
            px, py = x + PW / 2, y + PH / 2 + 2
            s.rect(px - 22, py - 9, 44, 18, fill="#fff", stroke=RED, sw=2)
            s.text(px, py + 36, tag(n) if tag else "5 V in", size=14, fill=RED, weight="bold")
    # ribbons: OUT of panel n to IN of panel n+1
    pos = {}
    for row in range(ROWS):
        for bcol in range(COLS):
            pos[chain_number(bcol, row)] = (bcol, row)
    for n in range(1, 12):
        (c1, r1), (c2, r2) = pos[n], pos[n + 1]
        x1 = ox + connector_x(c1, r1, "out")
        x2 = ox + connector_x(c2, r2, "in")
        y1 = oy + r1 * PH + PH / 2 - 10
        y2 = oy + r2 * PH + PH / 2 - 10
        if r1 == r2:
            s.line([(x1, y1), (x1, y1 - 22), (x2, y1 - 22), (x2, y2)], stroke=BLUE, sw=5)
        else:
            side = 26 if x1 > ox + PW * 2 else -26
            s.line([(x1, y1 + 20), (x1 + side, y1 + 30), (x1 + side, y2 - 6), (x2, y2)], stroke=BLUE, sw=5,
                   dash="10,6")
    # controller on panel 1's IN
    c1, r1 = pos[1]
    bx = ox + connector_x(c1, r1, "in")
    by = oy + r1 * PH + PH / 2
    s.rect(bx - 25, by - 21, 50, 42, fill=ORANGE, stroke=BLACK, sw=2, rx=4, opacity=0.9)
    s.text(bx, by + 5, "ESP32", size=12, weight="bold")
    s.text(ox - 12, oy + 20, "Board plugs", size=14, anchor="end", fill=ORANGE, weight="bold")
    s.text(ox - 12, oy + 38, "straight onto", size=14, anchor="end", fill=ORANGE, weight="bold")
    s.text(ox - 12, oy + 56, "panel 1 IN", size=14, anchor="end", fill=ORANGE, weight="bold")
    return pos


def legend(s, x, y, items):
    for i, (color, dash, label) in enumerate(items):
        yy = y + i * 28
        s.line([(x, yy), (x + 50, yy)], stroke=color, sw=5, dash=dash)
        s.text(x + 62, yy + 6, label, size=17, anchor="start")


def bus(s, x, y, label, n, first=1):
    """A pair of bus bars (+ above, - below), n screw positions numbered from first."""
    w = 40 + n * 34
    s.rect(x, y, w, 22, fill="#f6c6c6", stroke=RED, sw=2, rx=3)
    s.rect(x, y + 34, w, 22, fill="#d9d9d9", stroke=BLACK, sw=2, rx=3)
    for i in range(n):
        s.circle(x + 32 + i * 34, y + 11, 6, fill="white", stroke=RED)
        s.circle(x + 32 + i * 34, y + 45, 6, fill="white", stroke=BLACK)
        s.text(x + 32 + i * 34, y + 76, str(first + i), size=14, weight="bold")
    s.text(x + w / 2, y - 10, label, size=16, weight="bold")
    s.text(x - 8, y + 17, "+5 V", size=14, anchor="end", fill=RED)
    s.text(x - 8, y + 51, "GND", size=14, anchor="end")
    return w


def din_block(s, x, y, label, color):
    """A 2-in / 12-out lever block drawn as its two isolated halves: orange (supply A, panels
    1-6, input on the left) and blue (supply B, panels 7-12, input on the right). Returns the
    y of the inputs."""
    w, h = 240, 44
    s.rect(x, y, w / 2, h, fill="#fde3c4", stroke=color, sw=2, rx=3)
    s.rect(x + w / 2, y, w / 2, h, fill="#d3e4f8", stroke=color, sw=2, rx=3)
    for i in range(12):
        cx = x + 12 + i * 19.6
        s.circle(cx, y + 12, 5, fill="white", stroke=color)
        s.text(cx, y - 6, str(i + 1), size=11, weight="bold")
    s.rect(x - 14, y + h / 2 - 7, 14, 14, fill=ORANGE, stroke=color, sw=1.5)
    s.rect(x + w, y + h / 2 - 7, 14, 14, fill=BLUE, stroke=color, sw=1.5)
    s.text(x + w / 2, y + h - 6, label, size=13, weight="bold", fill=color)
    return y + h / 2


def lrs(s, x, y, name):
    """Mean Well LRS-100-5 footprint (129 x 97 mm) with its 7-way terminal block on the left."""
    s.rect(x, y, 129 * 1.6, 97 * 1.6, fill="#eeeeee", stroke=BLACK, sw=2, rx=4)
    s.text(x + 120, y + 30, name, size=20, weight="bold")
    s.text(x + 120, y + 54, "LRS-100-5", size=15, fill=GREY)
    s.text(x + 120, y + 74, "5 V 18 A", size=15, fill=GREY)
    labels = ["L", "N", "⏚", "-V", "-V", "+V", "+V"]
    for i, lab in enumerate(labels):
        ty = y + 14 + i * 19
        color = RED if lab == "+V" else (BLACK if lab == "-V" else GREY)
        s.rect(x + 6, ty, 26, 16, fill="white", stroke=color, sw=1.5)
        s.text(x + 19, ty + 12, lab, size=11, fill=color)
    return {"L": y + 22, "N": y + 41, "FG": y + 60, "-V": y + 89, "+V": y + 127, "x": x}


def rear_two_supplies():
    s = Svg(1600, 1300)
    ox, oy = 160, 90
    s.text(800, 40, "Rear view: two Mean Well LRS-100-5 (as seen standing behind the wall)", size=26, weight="bold")
    draw_wall(s, ox, oy, tag=lambda n: f"to output {n}")
    # supplies and the two power blocks: on the lower seam rail's back, drawn below the wall
    plate_y = oy + 3 * PH + 175
    s.rect(ox - 20, plate_y - 20, 4 * PW + 40, 420, fill="#f4f4f4", stroke=GREY, sw=2, dash="8,6")
    s.text(ox + 4 * PW + 10, plate_y + 5, "on the back of the lower seam rail (drawn below the wall to keep it readable)", size=16, anchor="end", fill=GREY)
    bx = ox + 360
    yp = din_block(s, bx, plate_y + 20, "+5 V block", RED)
    yg = din_block(s, bx, plate_y + 90, "GND block", BLACK)
    A = lrs(s, ox + 60, plate_y + 150, "Supply A")
    B = lrs(s, ox + 2 * PW + 60, plate_y + 150, "Supply B")
    for sup in (A, B):
        s.line([(sup["x"] + 6, sup["L"]), (sup["x"] - 70, sup["L"])], stroke=GREY, sw=3)
        s.line([(sup["x"] + 6, sup["N"]), (sup["x"] - 70, sup["N"])], stroke=GREY, sw=3)
        s.line([(sup["x"] + 6, sup["FG"]), (sup["x"] - 70, sup["FG"])], stroke="#2b9348", sw=3)
        s.text(sup["x"] - 74, sup["N"] + 5, "to cord", size=13, anchor="end", fill=GREY)
    # supply A: +V through its fuse to the orange half of the + block, -V to the orange half of GND
    ax = A["x"]
    s.line([(ax, A["+V"]), (ax - 40, A["+V"]), (ax - 40, yp), (bx - 14, yp)], stroke=RED, sw=5)
    s.rect(ax + 90, yp - 10, 46, 20, fill=FUSE, stroke=BLACK, sw=2, rx=4)
    s.text(ax + 113, yp - 16, "20 A fuse", size=13)
    s.line([(ax, A["-V"]), (ax - 20, A["-V"]), (ax - 20, yg), (bx - 14, yg)], stroke=BLACK, sw=5)
    # supply B: the same into the blue halves, from the right
    bx2, rx = B["x"], bx + 254
    s.line([(bx2, B["+V"]), (bx2 - 20, B["+V"]), (bx2 - 20, yp), (rx, yp)], stroke=RED, sw=5)
    s.rect(rx + 6, yp - 10, 46, 20, fill=FUSE, stroke=BLACK, sw=2, rx=4)
    s.text(rx + 29, yp - 16, "20 A", size=13)
    s.line([(bx2, B["-V"]), (bx2 - 45, B["-V"]), (bx2 - 45, yg), (rx, yg)], stroke=BLACK, sw=5)
    # ground join between the supplies' second -V screws, under the supplies
    gy = plate_y + 360
    s.line([(ax, A["-V"] + 8), (ax - 10, A["-V"] + 8), (ax - 10, gy), (bx2 - 10, gy), (bx2 - 10, B["-V"] + 8),
            (bx2, B["-V"] + 8)], stroke=BLACK, sw=4, dash="4,4")
    s.text(ox + PW + 120, gy - 8, "ground join, second -V to second -V (never join +V)", size=14)
    # controller power lead from supply A's second terminals
    s.text(ox - 150, oy + 3 * PH + 100, "Board 5 V lead:", size=14, anchor="start", fill=ORANGE, weight="bold")
    s.text(ox - 150, oy + 3 * PH + 118, "supply A's second", size=14, anchor="start", fill=ORANGE)
    s.text(ox - 150, oy + 3 * PH + 136, "+V / -V, 2 A fuse", size=14, anchor="start", fill=ORANGE)
    s.line([(ax, A["+V"] + 8), (ox - 40, A["+V"] + 8), (ox - 40, oy + PH / 2 + 30), (ox + 52 - 25, oy + PH / 2 + 15)],
           stroke=ORANGE, sw=3, dash="6,4")
    legend(s, 1200, 1170, [(BLUE, None, "HUB75 ribbon (20 cm, in a row)"), (BLUE, "10,6", "HUB75 ribbon (30 cm, row to row)"),
                           (RED, None, "+5 V (panel lead / feed)"), (BLACK, None, "GND")])
    s.text(160, 1250, "Each panel's own lead: plug into its 5 V socket; red wire into the + block, black into the GND block, both at the",
           size=15, anchor="start", fill=GREY)
    s.text(160, 1270, "output with that panel's number. The halves are isolated: orange = supply A (shaded red), blue = supply B.",
           size=15, anchor="start", fill=GREY)
    s.save("wall-rear-two-supplies.svg")


def rear_single_supply():
    s = Svg(1720, 1180)
    ox, oy = 160, 90
    s.text(800, 40, "Rear view: one Mean Well UHP-500-5 (as seen standing behind the wall)", size=26, weight="bold")
    draw_wall(s, ox, oy, tag=lambda n: f"lead to branch {(n - 1) // 3 + 1}, slot {n}", single=True)
    pos = {}
    for row in range(ROWS):
        for bcol in range(COLS):
            pos[chain_number(bcol, row)] = (bcol, row)
    groups = [(1, 2, 3), (4, 5, 6), (7, 8, 9), (10, 11, 12)]
    gy = oy + 3 * PH + 60
    gx = [ox + 10 + i * 320 for i in range(4)]
    for gi, g in enumerate(groups):
        bus(s, gx[gi], gy, f"Branch {gi + 1}: panels {g[0]}-{g[-1]}", 3, g[0])
    # main bus bars: + on top, GND below, spanning the wall so each branch feeds straight down
    my = oy + 3 * PH + 230
    mx0, mx1 = ox + 10, ox + 4 * PW - 10
    s.rect(mx0, my, mx1 - mx0, 22, fill="#f6c6c6", stroke=RED, sw=2, rx=3)
    s.rect(mx0, my + 34, mx1 - mx0, 22, fill="#d9d9d9", stroke=BLACK, sw=2, rx=3)
    s.text(mx0 - 8, my + 17, "+5 V", size=14, anchor="end", fill=RED)
    s.text(mx0 - 8, my + 51, "GND", size=14, anchor="end")
    s.text((mx0 + mx1) / 2, my + 80, "Main bus bars (rated 100 A)", size=16, weight="bold")
    for gi in range(4):
        fx = gx[gi] + 166  # just right of each branch's bars
        # + : main + bus -> 20 A fuse -> branch + bar (enters from the right end)
        s.line([(fx, my), (fx, gy + 11), (gx[gi] + 142, gy + 11)], stroke=RED, sw=4)
        s.rect(fx - 10, gy + 92, 20, 40, fill=FUSE, stroke=BLACK, sw=2, rx=4)
    s.text(gx[0] + 186, gy + 118, "20 A fuse per branch, 12 AWG", size=14, anchor="start")
    s.text(ox + 640, gy + 160, "Each branch's black GND wire runs next to its red one, from the branch GND bar to the main GND bar.",
           size=14, fill=GREY)
    # the supply, with one 8 AWG lead per output terminal, entering the bus bars from the right end
    uy = my + 130
    ux = ox + 300
    s.rect(ux, uy, 232 * 2.2, 81 * 1.4, fill="#eeeeee", stroke=BLACK, sw=2, rx=4)
    s.text(ux + 255, uy + 50, "Mean Well UHP-500-5  (5 V 80 A)", size=20, weight="bold")
    s.text(ux + 255, uy + 76, "bolted to a 450 x 450 x 3 mm aluminium plate", size=15, fill=GREY)
    s.text(ux + 255, uy + 98, "TB2/TB3: 1,2 = -V   3,4 = +V    TB1: 1 L, 2 N, 3 earth", size=14, fill=GREY)
    tx = ux + 232 * 2.2
    for k, (col, ty) in enumerate(((RED, my + 6), (RED, my + 16), (BLACK, my + 40), (BLACK, my + 50))):
        y0 = uy + 20 + k * 22
        s.line([(tx, y0), (mx1 + 40 + k * 14, y0), (mx1 + 40 + k * 14, ty), (mx1, ty)], stroke=col, sw=5)
    s.text(mx1 + 110, uy + 30, "8 AWG, short:", size=14, anchor="start")
    s.text(mx1 + 110, uy + 48, "one lead per", size=14, anchor="start")
    s.text(mx1 + 110, uy + 66, "output terminal", size=14, anchor="start")
    legend(s, 160, 1080, [(BLUE, None, "HUB75 ribbon (20 cm)"), (BLUE, "10,6", "HUB75 ribbon (30 cm)"), (RED, None, "+5 V"),
                           (BLACK, None, "GND")])
    s.save("wall-rear-single-supply.svg")


# ---- one panel and the controller -------------------------------------------------------

def panel_detail():
    s = Svg(1640, 700)
    s.text(820, 40, "One panel from behind, upright and upside down, and the board on panel 1", size=26, weight="bold")
    for i, (title, flip) in enumerate((("Upright (rows 1 and 3)", False), ("Upside down (row 2)", True))):
        x, y, w, h = 60 + i * 520, 110, 460, 230
        s.text(x + w / 2, y - 18, title, size=20, weight="bold")
        s.rect(x, y, w, h, fill="#fafafa", stroke=GREY, sw=3)
        inx, outx = (x + w - 70, x + 70) if flip else (x + 70, x + w - 70)
        for cx, lab in ((inx, "IN (HUB75, from the previous panel or the board)"), (outx, "OUT (to the next panel)")):
            s.rect(cx - 34, y + h / 2 - 16, 68, 32, fill="#cfe0f7", stroke=BLUE, sw=2)
            for k in range(8):
                for j in range(2):
                    s.circle(cx - 26 + k * 7.5, y + h / 2 - 7 + j * 14, 2.2, fill=BLUE)
        s.text(inx, y + h / 2 + 46, "IN", size=18, fill=BLUE, weight="bold")
        s.text(outx, y + h / 2 + 46, "OUT", size=18, fill=BLUE, weight="bold")
        # printed arrows
        ax = x + w / 2
        if flip:
            s.line([(ax - 60, y + 40), (ax - 60, y + 80)], stroke=BLACK, sw=4, arrow=True)
            s.line([(ax + 40, y + 60), (ax - 10, y + 60)], stroke=BLACK, sw=4, arrow=True)
        else:
            s.line([(ax - 60, y + 80), (ax - 60, y + 40)], stroke=BLACK, sw=4, arrow=True)
            s.line([(ax - 10, y + 60), (ax + 40, y + 60)], stroke=BLACK, sw=4, arrow=True)
        s.text(ax + 70, y + 66, "printed arrows", size=14, anchor="start", fill=GREY)
        # power socket
        s.rect(ax - 36, y + h / 2 + 50, 72, 26, fill="white", stroke=RED, sw=2)
        for k in range(4):
            s.circle(ax - 24 + k * 16, y + h / 2 + 63, 4, fill=RED if k < 2 else BLACK)
        s.text(ax, y + h / 2 + 100, "5 V socket: 2 x +5 V (red), 2 x GND (black)", size=13)
    s.text(60, 395, "How to tell IN from OUT: the sideways arrow on the back points from IN to OUT, and the up arrow shows",
           size=16, anchor="start")
    s.text(60, 418, "which way is up. Seen from behind, an upright panel's IN is on the left; upside down, it is on the right.",
           size=16, anchor="start")
    s.text(60, 441, "If your panels are printed differently, follow the arrows, not this drawing: data always goes OUT -> IN.",
           size=16, anchor="start", fill=RED)
    # controller close-up
    x, y = 1150, 120
    s.text(x + 170, y - 28, "Board on panel 1 (rear top-left)", size=20, weight="bold")
    s.rect(x, y, 340, 200, fill="#fafafa", stroke=GREY, sw=3)
    s.text(x + 170, y + 225, "panel 1, upright", size=14, fill=GREY)
    s.rect(x + 30, y + 60, 120, 100, fill=ORANGE, stroke=BLACK, sw=2, rx=6, opacity=0.95)
    s.text(x + 90, y + 100, "Waveshare", size=15, weight="bold")
    s.text(x + 90, y + 120, "ESP32-S3", size=15, weight="bold")
    s.text(x + 90, y + 140, "50 x 42 mm", size=13)
    s.rect(x + 34, y + 66, 18, 22, fill="#555", stroke=BLACK, sw=1)
    s.rect(x + 34, y + 96, 18, 22, fill="#555", stroke=BLACK, sw=1)
    s.text(x + 26, y + 82, "USB-C data", size=12, anchor="end")
    s.text(x + 26, y + 112, "USB-C power", size=12, anchor="end")
    s.circle(x + 140, y + 70, 7, fill="white", stroke=RED)
    s.circle(x + 140, y + 150, 7, fill="white", stroke=BLACK)
    s.line([(x + 147, y + 70), (x + 260, y + 40), (x + 330, y + 40)], stroke=RED, sw=3)
    s.line([(x + 147, y + 150), (x + 260, y + 180), (x + 330, y + 180)], stroke=BLACK, sw=3)
    s.rect(x + 240, y + 30, 36, 18, fill=FUSE, stroke=BLACK, sw=2, rx=3)
    s.text(x + 258, y + 22, "2 A", size=12)
    s.text(x + 345, y + 45, "+5 V", size=13, anchor="start", fill=RED)
    s.text(x + 345, y + 62, "from supply A", size=13, anchor="start", fill=RED)
    s.text(x + 345, y + 185, "GND", size=13, anchor="start")
    s.text(x + 345, y + 202, "from supply A", size=13, anchor="start")
    s.text(x + 170, y + 280, "Screen-power M3 terminals: 5 V (top) and GND (bottom)", size=13)
    s.text(x + 170, y + 300, "The board's back has a keyed 2 x 8 socket that", size=13)
    s.text(x + 170, y + 318, "plugs straight onto panel 1's IN header.", size=13)
    # bottom notes
    notes = [
        "Ribbons: panel to panel in the same row, use the 20 cm ribbons that came with the panels (IN and OUT are about 10 cm apart).",
        "Row to row (panel 4 to 5, panel 8 to 9) the headers are about 16 cm apart vertically: use a 30 cm ribbon. You need two.",
        "Power: every panel's own lead plugs into its 5 V socket; the other end goes into its supply's half of the two DIN blocks.",
        "Board power: plug in the 2 A-fused lead only when the laptop is NOT on USB, or leave it off and power the board from USB while testing.",
    ]
    for i, n in enumerate(notes):
        s.text(60, 560 + i * 30, n, size=16, anchor="start")
    s.save("panel-and-board.svg")


RAIL_L = 1219        # 48 in of 2020, centred
STILES = (100, 1142)  # left edges, rear view, clear of the mount screws, the board and the headers
SUPPLY_X = (180, 980)  # left edges of the supplies on the lower seam rail's back
BLOCKS_X = 656        # the two KV4212 blocks side by side from here


# ---- mounting ---------------------------------------------------------------------------

def mounting():
    s = Svg(1640, 760)
    s.text(820, 40, "Mounting and cooling", size=26, weight="bold")
    # side section, not to scale
    wx, top = 120, 110
    s.text(wx + 130, top - 20, "Side section (not to scale)", size=18, weight="bold")
    s.rect(wx - 40, top, 40, 560, fill="#d8cbb8", stroke=GREY, sw=2)
    s.text(wx - 20, top + 590, "wall", size=16)
    s.rect(wx, top + 20, 30, 70, fill="none", stroke=TEAL, sw=2, dash="5,3")
    s.rect(wx, top + 470, 30, 40, fill="none", stroke=TEAL, sw=2, dash="5,3")
    s.rect(wx + 30, top + 10, 20, 510, fill="#c9ced6", stroke=BLACK, sw=2)
    for yy in (top + 180, top + 350):
        s.rect(wx + 50, yy - 10, 20, 20, fill="#aeb6bf", stroke=BLACK, sw=2)
        s.rect(wx + 70, yy - 13, 9, 26, fill=ORANGE, stroke=BLACK, sw=1.5)
    s.rect(wx + 79, top + 10, 15, 510, fill="#333", stroke=BLACK, sw=2)
    s.rect(wx + 17, top + 302, 30, 97, fill="none", stroke=BLACK, sw=2, dash="4,3")
    for k in range(3):
        s.line([(wx + 8, top + 540 - k * 180), (wx + 8, top + 470 - k * 180)], stroke=TEAL, sw=2, arrow=True)
        s.line([(wx + 104, top + 500 - k * 180), (wx + 104, top + 440 - k * 180)], stroke="#e07a5f", sw=2, arrow=True)

    def callout(x, y, ty, text, color=BLACK):
        s.line([(x, y), (304, ty - 5)], stroke=GREY, sw=1.5)
        s.text(310, ty, text, size=15, anchor="start", fill=color)

    callout(wx + 15, top + 50, top + 30, "printed wall block, 30 mm: keyhole hanger at the top of each stile")
    callout(wx + 15, top + 490, top + 70, "bumper block at the bottom of each stile")
    callout(wx + 40, top + 120, top + 110, "stile (2020), behind the rails")
    callout(wx + 60, top + 180, top + 150, "seam rail (2020), directly behind the panels")
    callout(wx + 74, top + 185, top + 190, "junction mount, 9 mm: M5 x 25 from the rail's back into its insert")
    callout(wx + 86, top + 250, top + 230, "LED panels, face out (about 15 mm deep)")
    callout(wx + 32, top + 360, top + 290, "supply on the lower seam rail's back, beside the stiles (dashed)")
    callout(wx + 104, top + 430, top + 330, "warm air off the panels' backs", "#e07a5f")
    callout(wx + 8, top + 520, top + 370, "cooler air in at the bottom, up behind the frame and past the supplies", TEAL)
    s.text(310, top + 420, "Wall to panel face: about 95 mm. Nothing below or beside the screen.", size=15, anchor="start")
    s.text(310, top + 446, "The gap stays open at the top, bottom and sides: don't box it in.", size=15, anchor="start")
    # the lower seam rail seen from behind
    px, py = 1000, 120
    sc = 0.5
    s.text(px + 300, py - 20, "Lower seam rail from behind (half scale)", size=18, weight="bold")
    rl = 1219 * sc
    s.rect(px, py + 80, rl, 10, fill="#aeb6bf", stroke="#3d4650", sw=1.5)
    for i, x in enumerate(SUPPLY_X):
        xx = px + (x - 30.5) * sc
        s.rect(xx, py + 85 - 24, 129 * sc, 48, fill="#f8f8f8", stroke=BLACK, sw=1.5, rx=3, opacity=0.9)
        s.text(xx + 32, py + 55, f"Supply {'AB'[i]}", size=12, weight="bold")
    bx = px + (BLOCKS_X - 30.5) * sc
    s.rect(bx, py + 75, 64, 20, fill="#fde3c4", stroke=BLACK, sw=1.5)
    s.text(bx + 32, py + 55, "blocks", size=12, weight="bold")
    for x in (46, 320, 640, 960, 1234):
        s.circle(px + (x - 30.5) * sc, py + 85, 5, fill=ORANGE, stroke=BLACK)
    for x in (169, 489, 809, 1129):
        s.circle(px + (x - 30.5) * sc, py + 85, 4, fill="white", stroke=ORANGE)
    for x in STILES:
        s.rect(px + (x - 30.5) * sc, py + 40, 10, 90, fill="none", stroke="#5c6670", sw=1.5, dash="3,3")
    notes = [
        "Positions from the wall's left edge, seen from behind:",
        "supply A 180-309 mm, blocks 656-784 mm, supply B 980-1109 mm.",
        "Each sits between mount screws (orange) and clear of the stiles (dashed).",
        "Supplies: base bolted to an adapter (2 x M3, at most 3 mm into the case),",
        "adapter to the rail's rear slot with M5 T-nuts.",
        "Blocks: M3 screws through their 4 mm holes into M3 T-nuts.",
        "Fuses: inline on each +V feed, close to the supply.",
        "AC: a switched IEC inlet on a stile near the bottom, or a cord with",
        "an inline switch; cover the supplies' AC terminals.",
    ]
    for i, t in enumerate(notes):
        s.text(px, py + 180 + i * 24, t, size=14, anchor="start", weight="bold" if i == 0 else "normal")
    s.save("mounting.svg")


# ---- the frame --------------------------------------------------------------------------

def frame():
    """Rear elevation of the 2020 frame with the panel outline, and a side section."""
    s = Svg(1800, 900)
    s.text(900, 40, "2020 frame (rear view, as seen standing behind the wall)", size=26, weight="bold")
    ox, oy = 80, 110
    W_, H_ = 4 * PW, 3 * PH
    rx0 = ox + (W_ - RAIL_L) / 2
    # stiles behind the rails, with the wall blocks behind them
    for x in STILES:
        s.rect(ox + x, oy, 20, H_, fill="#c9ced6", stroke="#5c6670", sw=2)
        s.rect(ox + x - 3, oy + 2, 26, 80, fill="none", stroke=TEAL, sw=2, dash="5,3")
        s.rect(ox + x - 3, oy + H_ - 42, 26, 40, fill="none", stroke=TEAL, sw=2, dash="5,3")
    # panel outline, dashed
    for row in range(ROWS):
        for bcol in range(COLS):
            s.rect(ox + bcol * PW, oy + row * PH, PW, PH, fill="none", stroke=GREY, sw=1.5, dash="6,5")
            n = chain_number(bcol, row)
            s.text(ox + bcol * PW + PW / 2, oy + row * PH + 50, f"panel {n}", size=16, fill=GREY)
    # the two seam rails
    for k in (1, 2):
        y = oy + k * PH
        s.rect(rx0, y - 10, RAIL_L, 20, fill="#aeb6bf", stroke="#3d4650", sw=2)
        s.text(ox + W_ + 16, y + 6, "seam rail", size=14, anchor="start")
    # supplies and blocks on the lower seam rail's back (wall side)
    ly = oy + 2 * PH
    for i, x in enumerate(SUPPLY_X):
        s.rect(ox + x, ly - 48, 129, 97, fill="#f8f8f8", stroke=BLACK, sw=2, rx=4, opacity=0.9)
        s.text(ox + x + 64, ly - 22, f"Supply {'AB'[i]}", size=14, weight="bold")
    for k, col in enumerate((RED, BLACK)):
        bx = ox + BLOCKS_X + k * 66
        s.rect(bx, ly - 20, 32, 39, fill="#fde3c4", stroke=col, sw=2, rx=2)
        s.rect(bx + 32, ly - 20, 32, 39, fill="#d3e4f8", stroke=col, sw=2, rx=2)
    s.text(ox + BLOCKS_X + 64, ly + 40, "+5 V and GND blocks", size=13, weight="bold")
    # junction mounts at their inserts
    req, edge = [], []
    for k in (1, 2):
        y = oy + k * PH
        req += [(ox + 46, y), (ox + W_ - 46, y)] + [(ox + c * PW, y) for c in (1, 2, 3)]
    for y in (oy + 15, oy + H_ - 15):
        edge += [(ox + 15, y), (ox + W_ - 15, y)] + [(ox + c * PW, y) for c in (1, 2, 3)]
    for x, y in req:
        s.circle(x, y, 11, fill=ORANGE, stroke=BLACK)
        s.circle(x, y, 4, fill=BLACK)
    for x, y in edge:
        s.circle(x, y, 9, fill="white", stroke=GREY)
    for k in (1, 2):
        for c in range(COLS):
            x, y = ox + PW / 2 + c * PW + 9, oy + k * PH
            s.circle(x, y, 8, fill="white", stroke=ORANGE)
            s.circle(x, y, 3, fill=BLACK)
    # controller corner
    s.rect(ox + 27, oy + PH / 2 - 21, 50, 42, fill=ORANGE, stroke=BLACK, sw=2, rx=4, opacity=0.9)
    s.text(ox + 52, oy + PH / 2 + 5, "ESP32", size=12, weight="bold")
    # key
    ky = oy + H_ + 60
    items = [
        (lambda x, y: (s.circle(x, y, 11, fill=ORANGE, stroke=BLACK), s.circle(x, y, 4, fill=BLACK)),
         "junction mount fixed to a seam rail (10): 6 quads, and 4 side pairs with the insert 40 mm in"),
        (lambda x, y: (s.circle(x, y, 8, fill="white", stroke=ORANGE), s.circle(x, y, 3, fill=BLACK)),
         "optional (8): mid-edge pairs on the seam rails"),
        (lambda x, y: s.circle(x, y, 9, fill="white", stroke=GREY),
         "edge mounts (10): join the panels along the top and bottom edges; not fixed to the frame"),
        (lambda x, y: s.rect(x - 11, y - 12, 22, 24, fill="none", stroke=TEAL, sw=2, dash="5,3"),
         "printed wall blocks behind the stiles: keyhole hanger at the top, bumper at the bottom"),
    ]
    for i, (draw, text) in enumerate(items):
        draw(ox + 10, ky + i * 32)
        s.text(ox + 30, ky + i * 32 + 5, text, size=15, anchor="start")
    s.text(ox, ky + 4 * 32 + 10, "Extrusion: 4 x 48 in 2020. 2 seam rails (uncut), 2 stiles of 480 mm from one length, "
           "one spare. 8 corner brackets, 2 at each rail-stile crossing.", size=15, anchor="start")
    # side section through the lower seam
    sx, sy = 1460, 120
    s.text(sx + 110, sy - 30, "Side section through a seam", size=18, weight="bold")
    s.rect(sx, sy, 30, 560, fill="#d8cbb8", stroke=BLACK, sw=1.5)  # wall
    x = sx + 30
    s.rect(x, sy + 40, 30, 60, fill="none", stroke=TEAL, sw=2, dash="5,3")  # hanger
    s.rect(x, sy + 480, 30, 40, fill="none", stroke=TEAL, sw=2, dash="5,3")  # bumper
    s.rect(x + 30, sy + 30, 20, 500, fill="#c9ced6", stroke=BLACK, sw=1.5)  # stile
    for yy in (sy + 190, sy + 350):
        s.rect(x + 50, yy - 10, 20, 20, fill="#aeb6bf", stroke=BLACK, sw=1.5)  # rail
        s.rect(x + 70, yy - 13, 9, 26, fill=ORANGE, stroke=BLACK, sw=1.5)  # mount
    s.rect(x + 79, sy + 30, 15, 500, fill="#333", stroke=BLACK, sw=1.5)  # panel
    s.rect(x + 17, sy + 350 - 48, 30, 97, fill="none", stroke=BLACK, sw=1.5, dash="4,3")  # supply, beside the stile
    for k in range(3):
        s.line([(x + 8, sy + 520 - k * 170), (x + 8, sy + 450 - k * 170)], stroke=TEAL, sw=2, arrow=True)
    labels = [("wall", sx + 15, sy + 60), ("wall block, 30 mm", x + 15, sy + 70), ("stile (2020)", x + 40, sy + 120),
              ("seam rail (2020)", x + 60, sy + 190), ("mount (9 mm)", x + 74, sy + 205), ("panel", x + 86, sy + 260),
              ("supply (beside the stiles)", x + 32, sy + 330)]
    for i, (t, lx, ly2) in enumerate(labels):
        ty = sy + 40 + i * 30
        s.line([(lx, ly2), (sx + 160, ty - 4)], stroke=GREY, sw=1)
        s.text(sx + 166, ty, t, size=13, anchor="start")
    s.text(sx + 10, sy + 590, "air rises behind the frame,", size=13, anchor="start", fill=TEAL)
    s.text(sx + 10, sy + 608, "past the supplies", size=13, anchor="start", fill=TEAL)
    s.text(sx + 10, sy + 640, "about 95 mm wall to panel face", size=14, anchor="start")
    s.save("frame.svg")


if __name__ == "__main__":
    rear_two_supplies()
    rear_single_supply()
    panel_detail()
    mounting()
    frame()
    print("wrote", sorted(p.name for p in OUT.glob("*.svg")))
