"""Write every WS artwork SVG: app icons, installer header, web logos, banners, letterhead.

    python make_artwork.py <artwork-dir>

Letters are outlines from DejaVu Sans / DejaVu Sans Bold 2.37 (see ARTWORK-RECORD.md),
so no SVG written here needs a font installed to render identically.
"""
import os
import sys

import wsart
from wsart import NAVY, defs, font, icon_eme, icon_globe, icon_map, icon_ws

OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(wsart.HERE)
BOLD = "DejaVuSans-Bold.ttf"
BOOK = "DejaVuSans.ttf"
BLUE = "#1f6fd1"
SKY = "#5fb4ff"
GREY = "#4a5160"
TAGLINE = "Weak Signal Digital Modes Suite"
NAMES = "WS  ·  WS-MAP  ·  EME65"
HOMEPAGE = "sourceforge.net/projects/wsjt-x-improved"
NOTE = "<!-- WS artwork, 2026. Authorship, fonts, colours and licences: artwork/ARTWORK-RECORD.md -->\n"


def text(s, x, y, size, fontfile, fill, anchor="start"):
    return f'<path d="{font(fontfile).text_path(s, x, y, size, anchor=anchor)}" fill="{fill}"/>'


def width(s, size, fontfile):
    return font(fontfile).text_width(s, size)


def fit(s, max_width, max_size, fontfile):
    return min(max_size, max_width / width(s, 1.0, fontfile))


def doc(title, body, w, h, extra=""):
    return (NOTE + f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" viewBox="0 0 {w} {h}"{extra}>'
            f'<title>{title}</title>{body}</svg>\n')


def doc_mm(title, body, w_mm, h_mm, units_per_mm=10):
    return (NOTE + f'<svg xmlns="http://www.w3.org/2000/svg" width="{w_mm}mm" height="{h_mm}mm" '
            f'viewBox="0 0 {w_mm * units_per_mm} {h_mm * units_per_mm}"><title>{title}</title>{body}</svg>\n')


def write(rel, content):
    path = os.path.join(OUT, rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(content)
    print("wrote", rel)


# ---------------------------------------------------------------- app icons
write("ws_icon.svg", doc("WS", defs() + icon_ws(), 1024, 1024))
write("wsmap_icon.svg", doc("WS-MAP", defs() + icon_map(), 1024, 1024))
write("eme65_icon.svg", doc("EME65", defs() + icon_eme(), 1024, 1024))
write("ws_globe.svg", doc("WS globe (utilities)", defs() + icon_globe(), 1024, 1024))

# ---------------------------------------------------------------- NSIS header, 150 x 57, white
tag_size = fit("Digital Modes Suite", 84, 8.6, BOOK)
write("installer_logo.svg", doc(
    "WS installer header",
    defs() + '<rect width="150" height="57" fill="#fff"/>'
    + icon_ws(3, 3, 51 / 1024)
    + text("WS", 60, 27, 25, BOLD, NAVY)
    + text("Weak Signal", 61, 40, tag_size, BOOK, GREY)
    + text("Digital Modes Suite", 61, 50, tag_size, BOOK, GREY),
    150, 57))


# ---------------------------------------------------------------- web: horizontal logo, light and dark
def horizontal(dark):
    word, tag = ("#ffffff", "#cfe6ff") if dark else (NAVY, BLUE)
    x_text = 256 + 40
    w = x_text + max(width("WS", 170, BOLD), width(TAGLINE, 38, BOOK)) + 8
    body = defs() + icon_ws(0, 0, 256 / 1024) + text("WS", x_text - 4, 160, 170, BOLD, word) + text(TAGLINE, x_text, 222, 38, BOOK, tag)
    return doc("WS - " + TAGLINE, body, round(w), 256)


write("branding/web/ws-logo-horizontal.svg", horizontal(False))
write("branding/web/ws-logo-horizontal-dark.svg", horizontal(True))
write("branding/web/favicon.svg", doc("WS", defs() + icon_ws(), 1024, 1024))

# three programs side by side with their names
cell, gap = 256, 110
lock = ""
for i, (name, draw) in enumerate((("WS", icon_ws), ("WS-MAP", icon_map), ("EME65", icon_eme))):
    x = i * (cell + gap)
    # each icon gets its own gradient ids in the shared document
    lock += defs(f"i{i}") + draw(x, 0, cell / 1024, p=f"i{i}")
    lock += text(name, x + cell / 2, cell + 62, 46, BOLD, NAVY, anchor="middle")
write("branding/web/ws-suite-lockup.svg", doc("WS, WS-MAP, EME65", lock, 3 * cell + 2 * gap, cell + 84))


# ---------------------------------------------------------------- banners
def banner(w, h, icon, x_icon, y_icon, x_text, word_size, word_base, tag_size, tag_base, names_size, names_base, title):
    bg = (f'<defs><linearGradient id="bg" x1="0" y1="0" x2="1" y2="1">'
          f'<stop offset="0" stop-color="#071f4f"/><stop offset="1" stop-color="#1a5fbf"/></linearGradient></defs>'
          f'<rect width="{w}" height="{h}" fill="url(#bg)"/>')
    # a large faint graticule globe bleeding off the right edge
    r = h * 1.05
    cx, cy = w - h * 0.35, h * 0.92
    deco = (f'<g opacity="0.16"><circle cx="{cx:.0f}" cy="{cy:.0f}" r="{r:.0f}" fill="none" stroke="#fff" stroke-width="{h * 0.012:.1f}"/>'
            f'<path d="{wsart.graticule(r, cx, cy, 20)}" fill="none" stroke="#fff" stroke-width="{h * 0.006:.1f}"/></g>')
    body = (bg + deco + defs() + icon_ws(x_icon, y_icon, icon / 1024)
            + text("WS", x_text - word_size * 0.02, word_base, word_size, BOLD, "#ffffff")
            + text(TAGLINE, x_text, tag_base, tag_size, BOOK, "#e3f0ff")
            + text(NAMES, x_text, names_base, names_size, BOLD, SKY))
    return doc(title, body, w, h)


write("branding/banner/ws-banner-1500x500.svg",
      banner(1500, 500, 300, 90, 100, 440, 150, 225, 42, 295, 34, 372, "WS banner"))
write("branding/banner/ws-social-1200x630.svg",
      banner(1200, 630, 320, 70, 155, 435, 150, 300, 36, 368, 30, 440, "WS social preview"))


# ---------------------------------------------------------------- letterhead, A4 portrait (0.1 mm units)
def letterhead_header():
    return (defs() + icon_ws(150, 120, 270 / 1024)
            + text("WS", 460, 300, 175, BOLD, NAVY)
            + text(TAGLINE, 466, 378, 56, BOOK, BLUE)
            + text("WS  ·  WS-MAP  ·  EME65", 1950, 300, 44, BOLD, GREY, anchor="end")
            + f'<rect x="150" y="440" width="1800" height="7" fill="{NAVY}"/>'
            + f'<rect x="150" y="455" width="1800" height="3" fill="{SKY}"/>')


footer = (f'<rect x="150" y="2790" width="1800" height="3" fill="{SKY}"/>'
          + text("WS — " + TAGLINE + "   ·   " + HOMEPAGE, 1050, 2860, 36, BOOK, GREY, anchor="middle"))
write("branding/letterhead/ws-letterhead-a4.svg", doc_mm("WS letterhead (A4)", letterhead_header() + footer, 210, 297))
write("branding/letterhead/ws-letterhead-header.svg",
      NOTE + '<svg xmlns="http://www.w3.org/2000/svg" width="210mm" height="50mm" viewBox="0 0 2100 500">'
      f'<title>WS letterhead header</title>{letterhead_header()}</svg>\n')
write("branding/letterhead/ws-letterhead-footer.svg",
      NOTE + '<svg xmlns="http://www.w3.org/2000/svg" width="210mm" height="15mm" viewBox="0 2750 2100 150">'
      f'<title>WS letterhead footer</title>{footer}</svg>\n')
