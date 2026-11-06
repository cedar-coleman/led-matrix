"""Render the PR whiteboard pages at the wall's native 256x96, plus LED-dot previews.

Uses the Adafruit GFX classic 5x7 font (the default font of the HUB75 DMA library), so
the preview matches what the firmware will draw pixel for pixel.

Usage: python3 -I render_whiteboard.py <project dir> <project dir>/glcdfont.c
Reads <project dir>/athletes-from-chalkboard.csv, writes <project dir>/whiteboard/.
"""
import csv
import re
import sys
from pathlib import Path

from PIL import Image, ImageDraw

W, H = 256, 96

BG = (0, 0, 0)
ROW_ALT = (0, 10, 28)  # every other row, dim navy band
GRID = (0, 70, 170)  # chalkboard's blue column tape
TEAL = (0, 150, 150)  # brand teal: outer border and header rule
TITLE = (255, 255, 255)
HEADER = (235, 215, 70)  # chalkboard's yellow header chalk
NAME = (170, 220, 255)
NUM = (255, 255, 255)
PAGE = (120, 120, 120)

# x positions of the vertical rules; columns sit between them
NAME_W, NUM_W = 114, 27
RULES = [0, NAME_W + 1]
for _ in range(5):
    RULES.append(RULES[-1] + NUM_W + 1)
assert RULES[-1] == W - 1, RULES

TITLE_H = 11  # title band rows 0..10
HEADER_Y = 12  # header text row 12..19
DATA_Y = 21  # first data row; rule at 20
ROW_H = 8
ROWS_PER_PAGE = (H - 1 - DATA_Y) // ROW_H  # bottom rule at 95


def load_font(path: Path):
    src = path.read_text()
    body = src[src.index("font[]"):]
    data = [int(x, 16) for x in re.findall(r"0x[0-9A-Fa-f]{2}", body)]
    return [data[i * 5:(i + 1) * 5] for i in range(len(data) // 5)]


class Canvas:
    def __init__(self, font):
        self.img = Image.new("RGB", (W, H), BG)
        self.px = self.img.load()
        self.font = font

    def put(self, x, y, c):
        if 0 <= x < W and 0 <= y < H:
            self.px[x, y] = c

    def rect(self, x0, y0, x1, y1, c):
        for y in range(y0, y1 + 1):
            for x in range(x0, x1 + 1):
                self.put(x, y, c)

    def glyph(self, ch, x, y, c):
        cols = self.font[ord(ch)]
        for i, bits in enumerate(cols):
            for row in range(8):
                if bits >> row & 1:
                    self.put(x + i, y + row, c)

    @staticmethod
    def advance(ch, narrow_dot):
        # in numbers '.' is a single baseline pixel with a 2 px advance, so "136.5" is 25 px
        # wide and fits a 27 px column with 1 px clear of each rule; firmware does the same
        return 2 if narrow_dot and ch == "." else 6

    def width(self, text, narrow_dot=False):
        return sum(self.advance(ch, narrow_dot) for ch in text) - 1

    def text(self, s, x, y, c, narrow_dot=False):
        for ch in s:
            if narrow_dot and ch == ".":
                self.put(x, y + 6, c)
            else:
                self.glyph(ch, x, y, c)
            x += self.advance(ch, narrow_dot)


def instagram_icon(cv: Canvas, x0, y0):
    """9x9 pixel-art Instagram glyph, gradient orange (bottom-left) to magenta (top-right)."""
    art = [
        ".GGGGGGG.",
        "GWWWWWWWG",
        "GWGGGGWWG",
        "GWGGWGGWG",
        "GWGWGWGWG",
        "GWGGWGGWG",
        "GWGGGGGWG",
        "GWWWWWWWG",
        ".GGGGGGG.",
    ]
    for y, line in enumerate(art):
        for x, ch in enumerate(line):
            if ch == "W":
                cv.put(x0 + x, y0 + y, (255, 255, 255))
            elif ch == "G":
                t = (x + (8 - y)) / 16  # 0 at bottom-left, 1 at top-right
                cv.put(x0 + x, y0 + y, (255, int(170 * (1 - t)), int(40 + 160 * t)))


def fmt(v):
    if v in ("", None):
        return ""
    f = float(v)
    return str(int(f)) if f.is_integer() else f"{f:.1f}"


def render_page(font, athletes, page, pages):
    cv = Canvas(font)

    # title band
    instagram_icon(cv, 2, 1)
    cv.text("THE STRENGTH TANK", 15, 2, TITLE)
    label = f"{page}/{pages}"
    cv.text(label, W - 2 - cv.width(label), 2, PAGE)

    # alternating row bands
    for i in range(ROWS_PER_PAGE):
        if i % 2:
            y = DATA_Y + i * ROW_H
            cv.rect(1, y, W - 2, y + ROW_H - 1, ROW_ALT)

    # header
    cv.text("Name", 3, HEADER_Y, HEADER)
    for i, h in enumerate(["S", "CJ", "Ttl", "BS", "FS"]):
        left = RULES[i + 1] + 1
        cv.text(h, left + (NUM_W - cv.width(h)) // 2, HEADER_Y, HEADER)

    # rules: teal frame and header rule, blue column rules
    for x in RULES[1:-1]:
        cv.rect(x, HEADER_Y - 1, x, H - 2, GRID)
    cv.rect(0, TITLE_H, W - 1, TITLE_H, TEAL)
    cv.rect(0, DATA_Y - 1, W - 1, DATA_Y - 1, TEAL)
    cv.rect(0, H - 1, W - 1, H - 1, TEAL)
    cv.rect(0, TITLE_H, 0, H - 1, TEAL)
    cv.rect(W - 1, TITLE_H, W - 1, H - 1, TEAL)

    # data
    max_chars = NAME_W // 6
    for i, a in enumerate(athletes):
        y = DATA_Y + i * ROW_H
        name = a["name"]
        if len(name) > max_chars:
            name = name[: max_chars - 1] + "~"
        cv.text(name, 3, y, NAME)
        for j, key in enumerate(["snatch_kg", "clean_jerk_kg", "total_kg", "back_squat_kg", "front_squat_kg"]):
            s = fmt(a[key])
            right = RULES[j + 2] - 2  # last lit column, 1 px clear of the rule
            cv.text(s, right - cv.width(s, True), y, NUM, narrow_dot=True)
    return cv.img


def led_preview(img, scale=6):
    out = Image.new("RGB", (img.width * scale, img.height * scale), (12, 12, 12))
    d = ImageDraw.Draw(out)
    px = img.load()
    r = scale * 0.42
    for y in range(img.height):
        for x in range(img.width):
            c = px[x, y]
            if c == BG:
                c = (22, 22, 22)  # unlit LEDs are visible as dark grey dots
            cx, cy = x * scale + scale / 2, y * scale + scale / 2
            d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=c)
    return out


def main(base: Path, font_path: Path):
    font = load_font(font_path)
    with open(base / "athletes-from-chalkboard.csv", newline="") as f:
        athletes = list(csv.DictReader(f))
    out = base / "whiteboard"
    out.mkdir(exist_ok=True)

    pages = [athletes[i:i + ROWS_PER_PAGE] for i in range(0, len(athletes), ROWS_PER_PAGE)]
    previews = []
    for n, chunk in enumerate(pages, 1):
        img = render_page(font, chunk, n, len(pages))
        img.save(out / f"page-{n}.png")
        prev = led_preview(img)
        prev.save(out / f"page-{n}-led-preview.png")
        previews.append(prev)

    pw, ph = previews[0].size
    gap = 24
    sheet = Image.new("RGB", (pw, (ph + gap) * len(previews) - gap), (40, 40, 40))
    for i, prev in enumerate(previews):
        sheet.paste(prev, (0, i * (ph + gap)))
    sheet.save(out / "contact-sheet.png")
    print(f"{len(athletes)} athletes, {ROWS_PER_PAGE} per page, {len(pages)} pages -> {out}")


if __name__ == "__main__":
    main(Path(sys.argv[1]), Path(sys.argv[2]))
