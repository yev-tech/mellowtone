#!/usr/bin/env python3
"""Draws the Mellowtone logo: rising-tone arcs above the EQ wordmark
(the two "l"s are two level bars), accent yellow on the UI's near-black.

Writes boot_animation/en/0.png and 0.jpg (baseline JPEG, which the launcher's
decoder reads) for each board at its panel size, and the project images in
docs/images/: the README banner, the GitHub social preview (1280x640) and a
square icon of the arcs alone. The wordmark font is a parameter and is not
kept in the repository; the shipped images were drawn with Bricolage
Grotesque SemiBold (SIL Open Font License).

    python3 tools/assets/generate_boot_logo.py --font BricolageGrotesque.ttf
"""
import argparse
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[2]
TARGETS = [
    (ROOT / "assets/themes/boot_animation/en", 480, 800),
    (ROOT / "boards/r3proii/assets/themes/boot_animation/en", 480, 720),
    (ROOT / "boards/r3ii_2025/assets/themes/boot_animation/en", 320, 480),
    (ROOT / "boards/r3pro/assets/themes/boot_animation/en", 360, 480),
]
DOCS = ROOT / "docs/images"
BG = (13, 15, 14)
ACCENT = (239, 220, 132)
SS = 4  # supersampling factor


def mix(alpha):
    return tuple(round(b + (a - b) * alpha) for a, b in zip(ACCENT, BG))


def round_line(d, x0, y0, x1, y1, w, color):
    d.line([(x0, y0), (x1, y1)], fill=color, width=w)
    for x, y in ((x0, y0), (x1, y1)):
        d.ellipse([x - w / 2, y - w / 2, x + w / 2, y + w / 2], fill=color)


def arc(d, cx, cy, r, w, color):
    d.arc([cx - r - w / 2, cy - r - w / 2, cx + r + w / 2, cy + r + w / 2], 180, 360, fill=color, width=w)
    for x in (cx - r, cx + r):
        d.ellipse([x - w / 2, cy - w / 2, x + w / 2, cy + w / 2], fill=color)


def draw(width, height, font_path, basis=None, lift=0.03):
    """basis: the width the logo is sized for (default: the canvas width);
    lift: how far above centre the logo sits, as a share of the height;
    None centres the drawn logo exactly (the boot images keep 0.03)."""
    W, H = width * SS, height * SS
    B = (basis or width) * SS
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)

    # Wordmark size from the basis width; everything else follows from it.
    size = round(B * 0.15)
    font = ImageFont.truetype(str(font_path), size)
    try:  # variable font: SemiBold at the largest optical size
        axes = font.get_variation_axes()
        values = []
        for a in axes:
            name = a["name"].decode() if isinstance(a["name"], bytes) else a["name"]
            values.append({"Weight": 600, "Optical size": a["maximum"]}.get(name, a["default"]))
        font.set_variation_by_axes(values)
    except OSError:  # static font
        pass
    ascent, _ = font.getmetrics()
    cap = font.getbbox("h")  # ascender box of a lowercase letter
    bar_h = cap[3] - cap[1]
    bar_w = round(size * 0.13)
    gap = round(size * 0.06)
    left, right = "me", "owtone"
    lw = font.getlength(left)
    rw = font.getlength(right)
    total = lw + gap + bar_w + gap + bar_w + gap + rw

    unit = B * 0.085  # arc radius step
    stroke = max(SS * 3, round(unit * 0.32))
    arcs_h = 3 * unit + stroke
    block_h = arcs_h + unit * 1.1 + bar_h
    if lift is None:  # drawn extent: outer arc top to the wordmark baseline
        drawn_h = block_h - stroke / 2 + stroke * 1.6
        top = (H - drawn_h) / 2 - stroke / 2
    else:
        top = (H - block_h) / 2 - H * lift

    # Rising-tone mark: three half arcs over a horizon line.
    cx = W / 2
    base = top + arcs_h
    for i, alpha in ((3, 0.4), (2, 0.7), (1, 1.0)):
        arc(d, cx, base, unit * i, stroke, mix(alpha))
    horizon = base + stroke * 1.6
    round_line(d, cx - unit * 3.4, horizon, cx + unit * 3.4, horizon, round(stroke * 0.75), ACCENT)

    # EQ wordmark.
    baseline = horizon + unit * 1.1 + bar_h
    x = (W - total) / 2
    d.text((x, baseline), left, font=font, fill=(236, 238, 233), anchor="ls")
    x += lw + gap
    d.rounded_rectangle([x, baseline - bar_h, x + bar_w, baseline], radius=bar_w / 2, fill=ACCENT)
    x += bar_w + gap
    short = bar_h * 0.65
    d.rounded_rectangle([x, baseline - short, x + bar_w, baseline], radius=bar_w / 2, fill=mix(0.75))
    x += bar_w + gap
    d.text((x, baseline), right, font=font, fill=(236, 238, 233), anchor="ls")

    return img.resize((width, height), Image.LANCZOS)


def draw_icon(px):
    """The rising-tone arcs and horizon alone, centred on a square."""
    W = px * SS
    img = Image.new("RGB", (W, W), BG)
    d = ImageDraw.Draw(img)
    unit = W * 0.12
    stroke = round(unit * 0.32)
    horizon_gap = stroke * 1.6
    mark_h = 3 * unit + stroke / 2 + horizon_gap + stroke * 0.375
    base = (W - mark_h) / 2 + 3 * unit + stroke / 2
    cx = W / 2
    for i, alpha in ((3, 0.4), (2, 0.7), (1, 1.0)):
        arc(d, cx, base, unit * i, stroke, mix(alpha))
    horizon = base + horizon_gap
    round_line(d, cx - unit * 3.4, horizon, cx + unit * 3.4, horizon, round(stroke * 0.75), ACCENT)
    return img.resize((px, px), Image.LANCZOS)


def rounded(img, radius):
    """img with transparent rounded corners."""
    mask = Image.new("L", (img.width * SS, img.height * SS), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, mask.width - 1, mask.height - 1], radius=radius * SS, fill=255)
    out = img.convert("RGBA")
    out.putalpha(mask.resize(img.size, Image.LANCZOS))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--font", required=True, type=Path, help="wordmark font (TTF/OTF)")
    args = ap.parse_args()
    for out_dir, w, h in TARGETS:
        img = draw(w, h, args.font)
        out_dir.mkdir(parents=True, exist_ok=True)
        img.save(out_dir / "0.png", optimize=True)
        img.save(out_dir / "0.jpg", quality=92, progressive=False, subsampling=0)
        print(f"{out_dir.relative_to(ROOT)}: {w}x{h}")

    DOCS.mkdir(parents=True, exist_ok=True)
    project = [
        ("mellowtone-banner.png", rounded(draw(1200, 520, args.font, basis=800, lift=None), 36)),
        ("social-preview.png", draw(1280, 640, args.font, basis=720, lift=None)),
        ("mellowtone-icon.png", draw_icon(512)),
    ]
    for name, img in project:
        img.save(DOCS / name, optimize=True)
        print(f"{(DOCS / name).relative_to(ROOT)}: {img.width}x{img.height}")


if __name__ == "__main__":
    main()
