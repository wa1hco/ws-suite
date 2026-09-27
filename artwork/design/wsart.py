"""Drawing primitives for the WS / WS-MAP / EME65 artwork (accepted 2026-09-11).

All shapes are computed - an orthographic graticule, gradients, circles - and
letters come from an open-licence font as outlines (ttf2path), so the SVGs need
no third-party artwork and no installed font.
"""
import math
import os

from ttf2path import Font

HERE = os.path.dirname(os.path.abspath(__file__))
FONT_DIR = os.path.join(os.path.dirname(HERE), "fonts")

TILT = math.radians(23.0)
ROT = math.radians(-20.0)

NAVY = "#0b2f73"
OUTLINE = "#08224f"


def project(lat, lon, r, cx, cy):
    lat, lon = math.radians(lat), math.radians(lon)
    dl = lon - ROT
    x = r * math.cos(lat) * math.sin(dl)
    y = r * (math.cos(TILT) * math.sin(lat) - math.sin(TILT) * math.cos(lat) * math.cos(dl))
    cosc = math.sin(TILT) * math.sin(lat) + math.cos(TILT) * math.cos(lat) * math.cos(dl)
    return cx + x, cy - y, cosc >= 0


def _runs(points):
    d, run = [], []
    for x, y, v in points + [(0, 0, False)]:
        if v:
            run.append((x, y))
        else:
            if len(run) > 1:
                d.append("M" + " L".join(f"{px:.1f},{py:.1f}" for px, py in run))
            run = []
    return " ".join(d)


def graticule(r, cx, cy, step):
    paths = [_runs([project(lat, lon, r, cx, cy) for lon in range(-180, 181, 3)]) for lat in range(-90 + step, 90, step)]
    paths += [_runs([project(lat, lon, r, cx, cy) for lat in range(-90, 91, 3)]) for lon in range(-180, 180, step)]
    return " ".join(p for p in paths if p)


def defs(prefix=""):
    """Gradients; prefix keeps ids unique when several icons share one SVG document."""
    p = prefix
    return (f'<defs>'
            f'<radialGradient id="{p}sea" cx="38%" cy="32%" r="75%"><stop offset="0" stop-color="#5fb4ff"/><stop offset="0.55" stop-color="#1f6fd1"/><stop offset="1" stop-color="{NAVY}"/></radialGradient>'
            f'<radialGradient id="{p}shine" cx="35%" cy="28%" r="40%"><stop offset="0" stop-color="#fff" stop-opacity="0.55"/><stop offset="1" stop-color="#fff" stop-opacity="0"/></radialGradient>'
            f'<radialGradient id="{p}moon" cx="40%" cy="35%" r="70%"><stop offset="0" stop-color="#fdfdf8"/><stop offset="1" stop-color="#a3a8b1"/></radialGradient>'
            f'</defs>')


def earth(cx, cy, r, step=30, width=14, p=""):
    """Graticule globe; all sizes are in the 1024-unit icon space, scaled by r."""
    # the grid is clipped to the globe, or the round line ends poke past the limb
    clip = f"{p}clip{int(cx)}x{int(cy)}"
    return (f'<clipPath id="{clip}"><circle cx="{cx:.1f}" cy="{cy:.1f}" r="{r:.1f}"/></clipPath>'
            f'<circle cx="{cx:.1f}" cy="{cy:.1f}" r="{r:.1f}" fill="url(#{p}sea)"/>'
            f'<path d="{graticule(r, cx, cy, step)}" clip-path="url(#{clip})" fill="none" stroke="#fff" stroke-opacity="0.55" stroke-width="{width:.1f}" stroke-linecap="round"/>'
            f'<circle cx="{cx:.1f}" cy="{cy:.1f}" r="{r:.1f}" fill="url(#{p}shine)"/>'
            f'<circle cx="{cx:.1f}" cy="{cy:.1f}" r="{r - width / 2:.1f}" fill="none" stroke="{OUTLINE}" stroke-width="{width:.1f}"/>')


def moon(cx, cy, r, p=""):
    craters = [(-0.30, -0.22, 0.22), (0.30, 0.20, 0.15), (0.02, 0.48, 0.11), (-0.40, 0.32, 0.09)]
    rim = r * 16 / 195
    spots = "".join(f'<circle cx="{cx + dx * r:.1f}" cy="{cy + dy * r:.1f}" r="{rr * r:.1f}" fill="#8b929c" opacity="0.5"/>' for dx, dy, rr in craters)
    return (f'<circle cx="{cx:.1f}" cy="{cy:.1f}" r="{r:.1f}" fill="url(#{p}moon)"/>{spots}'
            f'<circle cx="{cx:.1f}" cy="{cy:.1f}" r="{r - rim / 2:.1f}" fill="none" stroke="#5b6270" stroke-width="{rim:.1f}"/>')


_fonts = {}


def font(name):
    if name not in _fonts:
        _fonts[name] = Font(os.path.join(FONT_DIR, name))
    return _fonts[name]


def letters(text, cx, cy, max_width, max_size, fontfile, fill="#fff", stroke=NAVY, stroke_ratio=0.05, cap=0.73):
    """Outlined text centred on (cx, cy): as large as max_size allows within max_width."""
    f = font(fontfile)
    size = min(max_size, max_width / (f.text_width(text, 1000) / 1000))
    baseline = cy + cap * size / 2
    d = f.text_path(text, cx, baseline, size, anchor="middle")
    sw = size * stroke_ratio
    stroke_attr = f' stroke="{stroke}" stroke-width="{sw:.1f}" stroke-linejoin="round" paint-order="stroke"' if stroke else ""
    return f'<path d="{d}" fill="{fill}"{stroke_attr}/>'


def svg(body, w=1024, h=1024, extra=""):
    return f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" viewBox="0 0 {w} {h}"{extra}>{body}</svg>'


# ---- the three accepted icons, in a 1024 box, drawn at an offset/scale ---------

BOLD = "DejaVuSans-Bold.ttf"


# The globe nearly fills the icon box: a circle inscribed in a square already
# reads smaller than a shape reaching the corners, and beside WSJT-X and JTDX
# on a desktop a 92% globe looked undersized (Uwe, 2026-09-11).
def icon_ws(x=0, y=0, s=1.0, p="", fontfile=BOLD):
    return (earth(x + 512 * s, y + 512 * s, 500 * s, width=15 * s, p=p)
            + letters("WS", x + 512 * s, y + 512 * s, 640 * s, 404 * s, fontfile))


def icon_map(x=0, y=0, s=1.0, p="", fontfile=BOLD):
    return (earth(x + 512 * s, y + 512 * s, 500 * s, width=15 * s, p=p)
            + letters("MAP", x + 512 * s, y + 512 * s, 724 * s, 319 * s, fontfile))


def icon_eme(x=0, y=0, s=1.0, p=""):
    return (earth(x + 355 * s, y + 669 * s, 340 * s, step=45, width=19 * s, p=p)
            + moon(x + 809 * s, y + 215 * s, 205 * s, p=p))


def icon_globe(x=0, y=0, s=1.0, p=""):
    """Plain globe for the utilities' generic icon set."""
    return earth(x + 512 * s, y + 512 * s, 500 * s, width=15 * s, p=p)
