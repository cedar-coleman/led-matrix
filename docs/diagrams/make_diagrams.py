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
    s = Svg(1600, 1180)
    ox, oy = 160, 90
    s.text(800, 40, "Rear view: two Mean Well LRS-100-5 (as seen standing behind the wall)", size=26, weight="bold")
    draw_wall(s, ox, oy, tag=lambda n: f"lead to bus {'A' if n <= 6 else 'B'}, slot {n}")
    # bus bars behind the wall: A behind the top-right area (rear view top-left half), B lower
    a_x, a_y = ox + 30, oy + 3 * PH + 50
    b_x, b_y = ox + 2 * PW + 30, oy + 3 * PH + 50
    wa = bus(s, a_x, a_y, "Bus A: panels 1-6 (slot = panel number)", 6, 1)
    wb = bus(s, b_x, b_y, "Bus B: panels 7-12 (slot = panel number)", 6, 7)
    # supplies on the plate below
    plate_y = oy + 3 * PH + 175
    s.rect(ox - 20, plate_y - 20, 4 * PW + 40, 310, fill="#f4f4f4", stroke=GREY, sw=2, dash="8,6")
    s.text(ox + 4 * PW + 10, plate_y + 5, "supply plate below the wall", size=16, anchor="end", fill=GREY)
    A = lrs(s, ox + 60, plate_y + 30, "Supply A")
    B = lrs(s, ox + 2 * PW + 60, plate_y + 30, "Supply B")
    for sup, bx, by, w in ((A, a_x, a_y, wa), (B, b_x, b_y, wb)):
        # +V: fuse then up to + bus
        fx = sup["x"] + 230
        s.line([(sup["x"], sup["+V"]), (sup["x"] - 40, sup["+V"]), (sup["x"] - 40, plate_y + 250),
                (fx, plate_y + 250)], stroke=RED, sw=5)
        s.rect(fx, plate_y + 240, 46, 20, fill=FUSE, stroke=BLACK, sw=2, rx=4)
        s.text(fx + 23, plate_y + 285, "20 A fuse", size=14)
        s.line([(fx + 46, plate_y + 250), (bx + w + 40, plate_y + 250), (bx + w + 40, by + 11), (bx + w, by + 11)],
               stroke=RED, sw=5)
        # -V straight up to - bus
        s.line([(sup["x"], sup["-V"]), (sup["x"] - 20, sup["-V"]), (sup["x"] - 20, by + 45), (bx, by + 45)],
               stroke=BLACK, sw=5)
        # AC
        s.line([(sup["x"] + 6, sup["L"]), (sup["x"] - 70, sup["L"])], stroke=GREY, sw=3)
        s.line([(sup["x"] + 6, sup["N"]), (sup["x"] - 70, sup["N"])], stroke=GREY, sw=3)
        s.line([(sup["x"] + 6, sup["FG"]), (sup["x"] - 70, sup["FG"])], stroke="#2b9348", sw=3)
        s.text(sup["x"] - 74, sup["N"] + 5, "to cord", size=13, anchor="end", fill=GREY)
    # ground jumper between the two - buses
    s.line([(a_x + wa, a_y + 45), (b_x - 50, b_y + 45)], stroke=BLACK, sw=4, dash="4,4")
    s.text((a_x + wa + b_x) / 2, a_y + 36, "GND jumper (never join +5 V)", size=14)
    # controller power lead
    s.text(ox - 150, oy + 3 * PH + 100, "Board 5 V lead:", size=14, anchor="start", fill=ORANGE, weight="bold")
    s.text(ox - 150, oy + 3 * PH + 118, "A bus to board", size=14, anchor="start", fill=ORANGE)
    s.text(ox - 150, oy + 3 * PH + 136, "via 2 A fuse", size=14, anchor="start", fill=ORANGE)
    s.line([(a_x + 32, a_y), (ox - 40, a_y - 40), (ox - 40, oy + PH / 2 + 30), (ox + 52 - 25, oy + PH / 2 + 15)],
           stroke=ORANGE, sw=3, dash="6,4")
    legend(s, 1200, 1085, [(BLUE, None, "HUB75 ribbon (20 cm, in a row)"), (BLUE, "10,6", "HUB75 ribbon (30 cm, row to row)"),
                           (RED, None, "+5 V (panel lead / feed)"), (BLACK, None, "GND")])
    s.text(160, 1140, "Each panel's own lead: plug into its 5 V socket, then screw the red fork to the + bar and the black fork to the GND bar,",
           size=15, anchor="start", fill=GREY)
    s.text(160, 1160, "both at the slot with that panel's number. Shaded red: panels on supply A. Shaded blue: supply B.",
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
    s.text(x + 345, y + 62, "from bus A", size=13, anchor="start", fill=RED)
    s.text(x + 345, y + 185, "GND", size=13, anchor="start")
    s.text(x + 345, y + 202, "from bus A", size=13, anchor="start")
    s.text(x + 170, y + 280, "Screen-power M3 terminals: 5 V (top) and GND (bottom)", size=13)
    s.text(x + 170, y + 300, "The board's back has a keyed 2 x 8 socket that", size=13)
    s.text(x + 170, y + 318, "plugs straight onto panel 1's IN header.", size=13)
    # bottom notes
    notes = [
        "Ribbons: panel to panel in the same row, use the 20 cm ribbons that came with the panels (IN and OUT are about 10 cm apart).",
        "Row to row (panel 4 to 5, panel 8 to 9) the headers are about 16 cm apart vertically: use a 30 cm ribbon. You need two.",
        "Power: every panel's own lead plugs into its 5 V socket; the other end (fork terminals) screws to its supply's bus bars.",
        "Board power: plug in the 2 A-fused lead only when the laptop is NOT on USB, or leave it off and power the board from USB while testing.",
    ]
    for i, n in enumerate(notes):
        s.text(60, 560 + i * 30, n, size=16, anchor="start")
    s.save("panel-and-board.svg")


# ---- mounting ---------------------------------------------------------------------------

def mounting():
    s = Svg(1640, 900)
    s.text(820, 40, "Mounting and cooling (side section, not to scale)", size=26, weight="bold")
    wx = 140

    def callout(x, y, tx, ty, text, color=BLACK):
        s.line([(x, y), (tx - 6, ty - 5)], stroke=GREY, sw=1.5)
        s.text(tx, ty, text, size=15, anchor="start", fill=color)

    s.rect(wx - 40, 80, 40, 760, fill="#d8cbb8", stroke=GREY, sw=2)
    s.text(wx - 20, 866, "wall", size=16)
    for yy in (150, 470):
        s.rect(wx, yy, 20, 40, fill="#b08968", stroke=BLACK, sw=2)
    s.rect(wx + 20, 110, 18, 480, fill="#888", stroke=BLACK, sw=2)
    s.rect(wx + 38, 110, 16, 480, fill="#333", stroke=BLACK, sw=2)
    for k in range(4):
        s.line([(wx + 64, 560 - k * 120), (wx + 64, 510 - k * 120)], stroke="#e07a5f", sw=2.5, arrow=True)
    callout(wx + 10, 170, 330, 140, "battens (20 mm) or 2020 extrusion, screwed to the wall")
    callout(wx + 29, 300, 330, 220, "3D-printed brackets join the panels and hang on the battens")
    callout(wx + 46, 380, 330, 300, "LED panels, face out (about 15 mm deep)")
    callout(wx + 64, 450, 330, 380, "warm air rises off the panels' backs", "#e07a5f")
    s.line([(wx + 20, 600), (wx + 20, 680)], stroke=GREY, sw=1.5, dash="4,4")
    callout(wx + 22, 640, 330, 470, "100 mm or more between the bottom of the panels and the supplies")
    s.rect(wx + 15, 690, 12, 140, fill="#b8b8b8", stroke=BLACK, sw=2)
    for yy in (700, 810):
        s.rect(wx, yy, 15, 10, fill="#666", stroke=BLACK, sw=1)
    s.rect(wx + 27, 700, 48, 120, fill="#e9e9e9", stroke=BLACK, sw=2)
    s.text(wx + 51, 768, "PSU", size=13)
    for k in range(3):
        s.line([(wx + 92 + k * 14, 836), (wx + 92 + k * 14, 690)], stroke=TEAL, sw=2, arrow=True, dash="6,4")
    s.line([(wx + 8, 846), (wx + 8, 690)], stroke=TEAL, sw=2, arrow=True, dash="6,4")
    callout(wx + 8, 710, 330, 560, "15 mm standoffs: air also flows behind the plate")
    callout(wx + 21, 760, 330, 600, "aluminium plate, 3 mm (heat spreader and mount)")
    callout(wx + 51, 790, 330, 640, "supply, base bolted to the plate (2 x M3, max 3 mm into the case)")
    callout(wx + 120, 700, 330, 680, "cool air in at the bottom, out at the top", TEAL)
    s.text(330, 702, "keep 25 mm clear around the supply's mesh", size=15, anchor="start", fill=TEAL)
    # front view of the plate
    px, py = 1000, 120
    s.text(px + 300, py - 20, "Supply plate, front view (two LRS-100-5)", size=20, weight="bold")
    s.rect(px, py, 600, 380, fill="#e4e4e4", stroke=BLACK, sw=2, rx=6)
    s.text(px + 300, py + 400, "about 600 x 380 x 3 mm aluminium, 4 standoffs", size=15)
    for i, name in enumerate(("Supply A", "Supply B")):
        sx = px + 50 + i * 290
        s.rect(sx, py + 150, 206, 155, fill="#f8f8f8", stroke=BLACK, sw=2, rx=4)
        s.text(sx + 103, py + 210, name, size=18, weight="bold")
        s.text(sx + 103, py + 232, "129 x 97 x 30 mm", size=13, fill=GREY)
        s.text(sx + 103, py + 252, "2 x M3 in the base", size=13, fill=GREY)
        s.rect(sx - 4, py + 150, 30, 155, fill="none", stroke=RED, sw=2, dash="5,4")
        s.text(sx + 10, py + 325, "terminal cover", size=12, fill=RED)
        s.rect(sx + 70, py + 40, 60, 24, fill=FUSE, stroke=BLACK, sw=2, rx=4)
        s.text(sx + 100, py + 32, "20 A fuse", size=13)
    s.rect(px + 230, py + 70, 140, 40, fill="#555", stroke=BLACK, sw=2, rx=4)
    s.text(px + 300, py + 95, "IEC inlet + switch", size=13, fill="white")
    s.text(px + 300, py + 135, "100 mm between supplies", size=13, fill=GREY)
    # printed enclosure notes
    ny = 600
    s.text(1000, ny, "If you 3D print instead of using a plate:", size=18, anchor="start", weight="bold")
    lines = [
        "- Print a cradle and a terminal cover, not a closed box.",
        "  Leave the supply's mesh top and sides open.",
        "- PETG at least; ASA, ABS or PC is better. PLA softens",
        "  around 55-60 C, which a loaded supply's case can reach.",
        "- Stand the supply off the print by 5-10 mm for airflow.",
        "- Vents low and high, so warm air rises out (chimney).",
        "- Cover the AC terminals fully; clamp the cord.",
        "- Mount it below the wall, never squeezed behind panels.",
    ]
    for i, t in enumerate(lines):
        s.text(1000, ny + 30 + i * 24, t, size=14, anchor="start")
    s.save("mounting.svg")


if __name__ == "__main__":
    rear_two_supplies()
    rear_single_supply()
    panel_detail()
    mounting()
    print("wrote", sorted(p.name for p in OUT.glob("*.svg")))
