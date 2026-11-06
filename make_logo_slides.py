"""Render Strength Tank logo slides at the wall's native 256x96, plus LED-dot previews.

Usage: python3 -I make_logo_slides.py <dir containing strength-tank-logo-source.png>
Outputs go to <dir>/slides/.
"""
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

W, H = 256, 96
TEAL = (0, 150, 150)
WHITE = (255, 255, 255)
BLACK = (0, 0, 0)
DIN = "/System/Library/Fonts/Supplemental/DIN Condensed Bold.ttf"
SS = 8  # supersample factor for text and logo before downscaling


def logo_mask(src: Path, size: int) -> Image.Image:
    """Alpha mask of the logo cropped to its bounding box and scaled to size x size."""
    alpha = Image.open(src).convert("RGBA").getchannel("A")
    alpha = alpha.crop(alpha.getbbox())
    side = max(alpha.size)
    square = Image.new("L", (side, side), 0)
    square.paste(alpha, ((side - alpha.width) // 2, (side - alpha.height) // 2))
    return square.resize((size, size), Image.LANCZOS)


def text_mask(lines, box_w, box_h):
    """Stack text lines (text, relative height) centred in a box, supersampled then reduced."""
    bw, bh = box_w * SS, box_h * SS
    mask = Image.new("L", (bw, bh), 0)
    draw = ImageDraw.Draw(mask)
    gap = 3 * SS
    total_rel = sum(rel for _, rel in lines)
    usable = bh - gap * (len(lines) - 1)
    y = 0
    for text, rel in lines:
        target_h = int(usable * rel / total_rel)
        size = target_h
        font = ImageFont.truetype(DIN, size)
        # shrink until the line fits the width
        while draw.textbbox((0, 0), text, font=font)[2] > bw and size > 4:
            size -= 2
            font = ImageFont.truetype(DIN, size)
        l, t, r, b = draw.textbbox((0, 0), text, font=font)
        x = (bw - (r - l)) // 2 - l
        draw.text((x, y + (target_h - (b - t)) // 2 - t), text, font=font, fill=255)
        y += target_h + gap
    return mask.resize((box_w, box_h), Image.LANCZOS)


def compose(fg, masks_and_positions):
    img = Image.new("RGB", (W, H), TEAL)
    solid = Image.new("RGB", (W, H), fg)
    for mask, (x, y) in masks_and_positions:
        full = Image.new("L", (W, H), 0)
        full.paste(mask, (x, y))
        img = Image.composite(solid, img, full)
    return img


def led_preview(img, scale=6):
    """Draw each pixel as a round LED dot on black, like the physical panel."""
    out = Image.new("RGB", (img.width * scale, img.height * scale), (12, 12, 12))
    d = ImageDraw.Draw(out)
    px = img.load()
    r = scale * 0.42
    for y in range(img.height):
        for x in range(img.width):
            cx, cy = x * scale + scale / 2, y * scale + scale / 2
            d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=px[x, y])
    # faint panel seams every 64x32 to show the 4x3 grid
    for gx in range(64, W, 64):
        d.line((gx * scale, 0, gx * scale, out.height), fill=(60, 60, 60), width=1)
    for gy in range(32, H, 32):
        d.line((0, gy * scale, out.width, gy * scale), fill=(60, 60, 60), width=1)
    return out


def main(base: Path):
    src = base / "strength-tank-logo-source.png"
    out = base / "slides"
    out.mkdir(exist_ok=True)

    mark = logo_mask(src, H - 4)
    centred = [(mark, ((W - mark.width) // 2, 2))]

    mark_side = logo_mask(src, H - 6)
    words = text_mask(
        [("THE", 0.7), ("STRENGTH", 2.0), ("TANK", 2.0), ("SAN DIEGO · CA", 0.8)],
        W - mark_side.width - 14, H - 10,
    )
    side_by_side = [(mark_side, (4, 3)), (words, (mark_side.width + 10, 5))]

    variants = {
        "logo-centered-white": (WHITE, centred),
        "logo-centered-black": (BLACK, centred),
        "logo-wordmark-white": (WHITE, side_by_side),
        "logo-wordmark-black": (BLACK, side_by_side),
    }
    previews = []
    for name, (fg, layers) in variants.items():
        img = compose(fg, layers)
        img.save(out / f"{name}.png")  # native 256x96, what the wall shows
        img.save(out / f"{name}.gif")  # single-frame GIF, the format the board plays
        prev = led_preview(img)
        prev.save(out / f"{name}-led-preview.png")
        previews.append((name, prev))

    # contact sheet of all four LED previews
    pw, ph = previews[0][1].size
    label_h = 40
    sheet = Image.new("RGB", (pw, (ph + label_h) * len(previews)), (30, 30, 30))
    d = ImageDraw.Draw(sheet)
    font = ImageFont.truetype(DIN, 30)
    for i, (name, prev) in enumerate(previews):
        y = i * (ph + label_h)
        d.text((10, y + 5), name, font=font, fill=(220, 220, 220))
        sheet.paste(prev, (0, y + label_h))
    sheet.save(out / "contact-sheet.png")
    print("wrote", sorted(p.name for p in out.iterdir()))


if __name__ == "__main__":
    main(Path(sys.argv[1]))
