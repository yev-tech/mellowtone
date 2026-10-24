#!/usr/bin/env python3
"""Draw the Stream Media icons in assets/themes/stream_media/.

  subsonic_row.png / subsonic_row_s.png  192x192, the Subsonic row in Stream
                                         Media (a generic media-server stack)
  download.png                           34x34 white, the Subsonic screens'
                                         download button (the app tints it)

Everything is drawn from plain geometry here; no source images are read.
Needs Python 3 with Pillow.
"""

from __future__ import annotations

import argparse
from pathlib import Path

from PIL import Image, ImageDraw


ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "assets/themes/stream_media"
SIZE = 192
SCALE = 4


def server_glyph() -> Image.Image:
    """Three stacked server units with status lights, and a note in front."""
    s = SCALE
    art = Image.new("RGBA", (SIZE * s, SIZE * s), (0, 0, 0, 0))
    d = ImageDraw.Draw(art)
    blue = (72, 173, 229, 255)
    deep = (34, 98, 156, 255)
    light = (214, 238, 252, 255)
    accent = (255, 196, 92, 255)

    # Server units: rounded slabs with a slot and two lights each.
    for top in (22, 70, 118):
        d.rounded_rectangle((24 * s, top * s, 168 * s, (top + 40) * s), radius=10 * s,
                            fill=deep, outline=blue, width=4 * s)
        d.rounded_rectangle((40 * s, (top + 16) * s, 104 * s, (top + 24) * s), radius=4 * s, fill=blue)
        for cx in (134, 150):
            d.ellipse(((cx - 5) * s, (top + 15) * s, (cx + 5) * s, (top + 25) * s), fill=light)

    # A beamed pair of notes over the lower right corner: "music server".
    d.ellipse((104 * s, 150 * s, 130 * s, 172 * s), fill=accent)
    d.ellipse((142 * s, 142 * s, 168 * s, 164 * s), fill=accent)
    d.rectangle((124 * s, 112 * s, 130 * s, 162 * s), fill=accent)
    d.rectangle((162 * s, 104 * s, 168 * s, 154 * s), fill=accent)
    d.polygon([(124 * s, 112 * s), (168 * s, 104 * s), (168 * s, 116 * s), (124 * s, 124 * s)], fill=accent)
    return art.resize((SIZE, SIZE), Image.Resampling.LANCZOS)


def download_glyph(size: int = 34) -> Image.Image:
    """A down arrow into an open tray, white on transparency."""
    s = SCALE * 4
    art = Image.new("RGBA", (size * s, size * s), (0, 0, 0, 0))
    d = ImageDraw.Draw(art)
    white = (255, 255, 255, 255)
    u = size * s / 24.0  # 24-unit grid
    w = round(2.2 * u)

    def line(points: list[tuple[float, float]]) -> None:
        pts = [(x * u, y * u) for x, y in points]
        d.line(pts, fill=white, width=w, joint="curve")
        r = w / 2
        for x, y in pts:
            d.ellipse((x - r, y - r, x + r, y + r), fill=white)

    line([(12, 3.5), (12, 14.5)])
    line([(7.5, 10.5), (12, 15), (16.5, 10.5)])
    line([(4, 15.5), (4, 20), (20, 20), (20, 15.5)])
    return art.resize((size, size), Image.Resampling.LANCZOS)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--output-dir", type=Path, default=OUT)
    args = parser.parse_args()

    output = args.output_dir
    output.mkdir(parents=True, exist_ok=True)
    subsonic = server_glyph()
    subsonic.save(output / "subsonic_row.png", optimize=True)
    subsonic.save(output / "subsonic_row_s.png", optimize=True)
    download_glyph().save(output / "download.png", optimize=True)


if __name__ == "__main__":
    main()
