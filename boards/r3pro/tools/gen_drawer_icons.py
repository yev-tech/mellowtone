#!/usr/bin/env python3
"""Draws the line glyphs (white on transparent) on a 24-unit grid with
2-unit round strokes, supersampled 8x then downsampled:
  assets/themes/pull_down/m_*.png   quick-drawer tiles and sliders, 20 px
  assets/themes/topbar/m_*.png      status bar, 24 px canvas (the bar
                                    scales Lucide-sized art to 16 px)
  assets/themes/launcher/radio.png  Home's Radio shortcut, 140 px
plus outlined codec labels for the status bar. The app recolours glyphs.
All artwork is drawn here; nothing is traced from other icon sets.

usage: gen_drawer_icons.py [FONTS_DIR]
FONTS_DIR (or MT_FONTS_DIR) holds NotoSans-Medium.ttf for the codec labels,
the same directory ./mt image --fonts takes (boards/r3pro/image/fonts.sha256).
Needs Python 3 with Pillow."""
import math, os, sys
from PIL import Image, ImageDraw

OUT = os.path.join(os.path.dirname(__file__), '../../../assets/themes/pull_down')
SIZE, SS = 20, 8
K = SIZE * SS / 24.0
W = 2.0  # stroke, grid units

def P(x, y): return (x * K, y * K)

def stroke(d, pts, w=W, closed=False):
    pts = [P(*p) for p in pts]
    if closed: pts = pts + [pts[0]]
    r = w * K / 2
    d.line(pts, fill=255, width=int(round(w * K)))
    for x, y in pts: d.ellipse((x - r, y - r, x + r, y + r), fill=255)

def arc(cx, cy, r, a0, a1, n=48):
    return [(cx + r * math.cos(math.radians(a0 + (a1 - a0) * i / n)), cy + r * math.sin(math.radians(a0 + (a1 - a0) * i / n))) for i in range(n + 1)]

def bez(p0, p1, p2, p3, n=40):
    out = []
    for i in range(n + 1):
        t = i / n; u = 1 - t
        out.append((u**3*p0[0] + 3*u*u*t*p1[0] + 3*u*t*t*p2[0] + t**3*p3[0], u**3*p0[1] + 3*u*u*t*p1[1] + 3*u*t*t*p2[1] + t**3*p3[1]))
    return out

def dot(d, x, y, r):
    x, y, r = x * K, y * K, r * K
    d.ellipse((x - r, y - r, x + r, y + r), fill=255)

def rrect(d, x0, y0, x1, y1, rad, w=W):
    pts = (arc(x1 - rad, y0 + rad, rad, -90, 0, 12) + arc(x1 - rad, y1 - rad, rad, 0, 90, 12) +
           arc(x0 + rad, y1 - rad, rad, 90, 180, 12) + arc(x0 + rad, y0 + rad, rad, 180, 270, 12))
    stroke(d, pts, w, closed=True)

def wifi(d):
    for r in (3.6, 7.6, 11.6): stroke(d, arc(12, 20, r, 225, 315))
    dot(d, 12, 19.6, 1.5)
def bt(d): stroke(d, [(7, 7.5), (17, 16.5), (12, 21), (12, 3), (17, 7.5), (7, 16.5)])
def moon(d):
    # filled crescent: a disc with an offset disc cut out of it
    dot(d, 12, 12.5, 8.6)
    x, y, r = 16.6 * K, 7.6 * K, 7.2 * K
    d.ellipse((x - r, y - r, x + r, y + r), fill=0)
def crossfade(d):
    stroke(d, bez((3, 17), (8, 17), (9, 7), (14, 7)) + [(21, 7)])
    stroke(d, bez((3, 7), (8, 7), (9, 17), (14, 17)) + [(21, 17)])
def gapless(d):
    pts = []
    for i in range(121):
        t = 2 * math.pi * i / 120
        s = 1 + math.sin(t) ** 2
        pts.append((12 + 8.5 * math.cos(t) / s, 12 + 8.5 * math.sin(t) * math.cos(t) / s))
    stroke(d, pts, closed=True)
def remote(d):
    rrect(d, 7, 2.5, 17, 21.5, 3)
    dot(d, 12, 7.5, 1.6)
    stroke(d, [(10, 13), (14, 13)]); stroke(d, [(10, 16.5), (14, 16.5)])
def sun(d):
    stroke(d, arc(12, 12, 4, 0, 360, 64), closed=True)
    for k in range(8):
        a = math.radians(45 * k)
        stroke(d, [(12 + 7.5 * math.cos(a), 12 + 7.5 * math.sin(a)), (12 + 9.5 * math.cos(a), 12 + 9.5 * math.sin(a))])
def speaker(d):
    stroke(d, [(3.5, 9), (7.5, 9), (12.5, 4.5), (12.5, 19.5), (7.5, 15), (3.5, 15)], closed=True)
    stroke(d, arc(14.5, 12, 3.5, -50, 50, 24)); stroke(d, arc(14.5, 12, 7, -50, 50, 32))

ICONS = {'wifi': wifi, 'bt': bt, 'sleep': moon, 'crossfade': crossfade, 'gapless': gapless,
         'rc': remote, 'brightness': sun, 'volume': speaker}
def draw_mask(fn, size):
    global K
    K = size * SS / 24.0
    mask = Image.new('L', (size * SS, size * SS), 0)
    fn(ImageDraw.Draw(mask))
    return mask.resize((size, size), Image.LANCZOS)

def save_mask(mask, path):
    img = Image.new('RGBA', mask.size, (255, 255, 255, 0))
    img.putalpha(mask)
    img.save(path)

def render(fn, size, path):
    save_mask(draw_mask(fn, size), path)

def render_wifi_level(level, size, path):
    # All three arcs are always drawn so the icon keeps its size at every
    # signal level; the unlit ones are 45 % alpha.
    from PIL import ImageChops
    # Wider than the drawer glyph: the bar draws it at 16 px beside the
    # Bluetooth rune and battery, so the arcs span nearly the full canvas.
    radii = (4.4, 9.0, 13.6)
    def lit(d):
        for r in radii[:level]: stroke(d, arc(12, 20.5, r, 222, 318))
        dot(d, 12, 20, 1.6)
    def unlit(d):
        for r in radii[level:]: stroke(d, arc(12, 20.5, r, 222, 318))
    a = draw_mask(lit, size)
    b = draw_mask(unlit, size).point(lambda v: v * 45 // 100)
    save_mask(ImageChops.lighter(a, b), path)

def codec_chip(text, path):
    from PIL import ImageFont
    fonts_dir = sys.argv[1] if len(sys.argv) > 1 else os.environ.get('MT_FONTS_DIR', '')
    font_path = os.path.join(fonts_dir, 'NotoSans-Medium.ttf')
    if not fonts_dir or not os.path.isfile(font_path):
        sys.exit('gen_drawer_icons.py: pass FONTS_DIR (or set MT_FONTS_DIR) with NotoSans-Medium.ttf')
    S = 4
    font = ImageFont.truetype(font_path, 10 * S)
    tw = int(font.getlength(text))
    h = 15 * S
    w = tw + 2 * 4 * S + 2 * S
    img = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle((S // 2, S // 2, w - 1 - S // 2, h - 1 - S // 2), radius=4 * S, outline=(93, 98, 91, 255), width=S)
    asc, desc = font.getmetrics()
    d.text(((w - tw) / 2, (h - (asc + desc)) / 2 + S * 0.3), text, font=font, fill=(163, 168, 159, 255))
    img.resize((w // S, h // S), Image.LANCZOS).save(path)

os.makedirs(OUT, exist_ok=True)
for name, fn in ICONS.items():
    render(fn, SIZE, os.path.join(OUT, 'm_' + name + '.png'))
TOP = os.path.join(os.path.dirname(__file__), '../../../assets/themes/topbar')
os.makedirs(TOP, exist_ok=True)
for lvl in range(4):
    render_wifi_level(lvl, 24, os.path.join(TOP, 'm_wifi_%d.png' % lvl))
render(bt, 24, os.path.join(TOP, 'm_bt.png'))
render(speaker, 24, os.path.join(TOP, 'm_volume.png'))
for key, text in (('sbc', 'SBC'), ('aac', 'AAC'), ('aptx', 'aptX'), ('aptx_hd', 'aptX HD'), ('ldac', 'LDAC'), ('uat', 'UAT')):
    codec_chip(text, os.path.join(TOP, 'm_codec_' + key + '.png'))
def battery(fill, charging=False):
    def fn(d):
        rrect(d, 2, 7, 20, 17, 2.5, 1.6)
        stroke(d, [(22.3, 10.5), (22.3, 13.5)], 1.8)
        if fill > 0:
            x0, x1 = 4.2, 4.2 + (17.8 - 4.2) * fill
            d.rounded_rectangle((x0 * K, 9.2 * K, x1 * K, 14.8 * K), radius=1.0 * K, fill=255)
        if charging:
            bolt = [(12.6, 7.6), (8.6, 12.6), (11.4, 12.6), (10.2, 16.4), (14.4, 11.2), (11.6, 11.2)]
            d.polygon([P(*p) for p in bolt], fill=0)
            # thin dark rim around the bolt so it reads against the fill
    return fn
for key, fill, ch in (('full', 1.0, False), ('medium', 0.6, False), ('low', 0.3, False), ('empty', 0.0, False),
                      ('charging', 1.0, True)):
    render(battery(fill, ch), 24, os.path.join(TOP, 'm_battery_' + key + '.png'))

def radio(d):
    # Table radio: body, a tuning dial on the right, grille slots on the
    # left and a telescopic antenna leaning back from the top edge.
    rrect(d, 2.5, 9, 21.5, 20.5, 2.5)
    stroke(d, [(7, 9), (17.5, 3.5)])
    dot(d, 17.5, 3.5, 1.3)
    stroke(d, arc(16, 14.75, 2.6, 0, 360, 48), closed=True)
    for y in (12.5, 14.75, 17):
        stroke(d, [(6, y), (10.5, y)], 1.6)

LAUNCHER = os.path.join(os.path.dirname(__file__), '../../../assets/themes/launcher')
os.makedirs(LAUNCHER, exist_ok=True)
# Drawn white; Home tints it (gui_home_clock.c add_shortcut()).
render(radio, 140, os.path.join(LAUNCHER, 'radio.png'))
print('ok', len(ICONS) + 4 + 2 + 6 + 5 + 1)
