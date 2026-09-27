"""Render the committed bitmaps from the SVGs in artwork/.

    python make_icons.py <repository-root> [<output-root>]

Needs rsvg-convert (librsvg; set RSVG_CONVERT to its path if it is not on PATH)
and Python 3 with Pillow. Replaces the older Inkscape + ImageMagick
make_graphics.sh. Output root defaults to the repository root, so the files land
where the build expects them.
"""
import io
import os
import shutil
import struct
import subprocess
import sys

from PIL import Image

REPO = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), ".."))
ROOT = os.path.abspath(sys.argv[2]) if len(sys.argv) > 2 else REPO
ART = os.path.join(REPO, "artwork")
RSVG = os.environ.get("RSVG_CONVERT") or shutil.which("rsvg-convert") or "rsvg-convert"

ICO_SIZES = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]
MAC_SIZES = [("16x16", 16), ("16x16@2x", 32), ("32x32", 32), ("32x32@2x", 64), ("128x128", 128),
             ("128x128@2x", 256), ("256x256", 256), ("256x256@2x", 512), ("512x512", 512), ("512x512@2x", 1024)]


def render(svg, w, h=None, background=None):
    """Render one SVG at width w (and height h if given, else proportional); returns RGBA."""
    cmd = [RSVG, "-w", str(w)] + (["-h", str(h)] if h else [])
    if background:
        cmd += ["-b", background]
    png = subprocess.run(cmd + [os.path.join(ART, svg)], capture_output=True, check=True).stdout
    return Image.open(io.BytesIO(png)).convert("RGBA")


def out(rel):
    path = os.path.join(ROOT, rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    return path


PNG_FROM = 256  # only this frame is PNG-compressed; some older resource compilers
                # mishandle PNG frames, and 256 is the size Windows documents for it


def _bmp_frame(im):
    """32-bit BGRA DIB with an all-zero AND mask, as stored inside .ico files."""
    w, h = im.size
    header = struct.pack("<IiiHHIIiiII", 40, w, 2 * h, 1, 32, 0, 0, 0, 0, 0, 0)
    r, g, b, a = im.split()
    bgra = Image.merge("RGBA", (b, g, r, a)).transpose(Image.FLIP_TOP_BOTTOM).tobytes()
    mask_row = ((w + 31) // 32) * 4
    return header + bgra + bytes(mask_row * h)


def ico(svg, rel, sizes=ICO_SIZES):
    # Every size is rendered natively from the SVG, not scaled from one bitmap.
    # Microsoft's layout for Windows icons: small frames uncompressed, large
    # frames PNG-compressed, which keeps the .ico (and each .exe) small.
    frames = []
    for s in sizes:
        im = render(svg, s)
        if s >= PNG_FROM:
            buf = io.BytesIO()
            im.save(buf, format="PNG", optimize=True)
            frames.append((s, buf.getvalue()))
        else:
            frames.append((s, _bmp_frame(im)))
    offset = 6 + 16 * len(frames)
    directory, blobs = [], []
    for s, data in frames:
        directory.append(struct.pack("<BBBBHHII", s % 256, s % 256, 0, 0, 1, 32, len(data), offset))
        blobs.append(data)
        offset += len(data)
    with open(out(rel), "wb") as f:
        f.write(struct.pack("<HHH", 0, 1, len(frames)) + b"".join(directory) + b"".join(blobs))
    print("ico ", rel, sizes, os.path.getsize(out(rel)), "bytes")


def png(svg, rel, w, h=None, background=None):
    render(svg, w, h, background).save(out(rel), format="PNG", optimize=True)
    print("png ", rel, f"{w}x{h or w}")


def bmp24(svg, rel, w, h):
    render(svg, w, h, background="#ffffff").convert("RGB").save(out(rel), format="BMP")
    print("bmp ", rel, f"{w}x{h}")


def pdf(svg, rel):
    subprocess.run([RSVG, "-f", "pdf", "-o", out(rel), os.path.join(ART, svg)], check=True)
    print("pdf ", rel)


# ---------------------------------------------------------------- applications
ico("ws_icon.svg", "icons/windows-icons/wsjtx.ico")          # ws.exe and the installer
ico("wsmap_icon.svg", "qmap/wsjt.ico")                       # wsmap.exe
ico("eme65_icon.svg", "map65/wsjt.ico")                      # eme65.exe
bmp24("installer_logo.svg", "icons/windows-icons/installer_logo.bmp", 150, 57)
for name, px in MAC_SIZES:
    png("ws_icon.svg", f"icons/Darwin/wsjtx.iconset/icon_{name}.png", px)
    png("ws_globe.svg", f"icons/Darwin/wsjt.iconset/icon_{name}.png", px)
png("DragNDrop Background.svg", "icons/Darwin/DragNDrop Background.png", 640, 480, background="#ffffff")
png("ws_icon.svg", "icons/Unix/ws_icon.png", 128)
# Qt resource icons: each program sets these as its window icon, so the title
# bar, task bar and alt-tab entry do not depend on the executable's resources
for svg_, stem_ in (("ws_icon.svg", "ws"), ("wsmap_icon.svg", "wsmap"), ("eme65_icon.svg", "eme65")):
    for px_ in (16, 32, 48, 128, 256):
        png(svg_, f"icons/app/{stem_}_icon_{px_}.png", px_)
# the About box shows the WS logo at 96 x 96; rendered at that size, not scaled
png("ws_icon.svg", "icons/app/ws_icon_96.png", 96)

# ---------------------------------------------------------------- branding
B = "artwork/branding"
ico("ws_icon.svg", f"{B}/web/favicon.ico", [16, 32, 48])
png("ws_icon.svg", f"{B}/web/favicon-32.png", 32)
png("ws_icon.svg", f"{B}/web/icon-192.png", 192)
png("ws_icon.svg", f"{B}/web/icon-512.png", 512)
# Apple touch icons must be opaque; the system rounds the corners itself
touch = Image.new("RGBA", (180, 180), "#ffffff")
touch.alpha_composite(render("ws_icon.svg", 156), (12, 12))
touch.convert("RGB").save(out(f"{B}/web/apple-touch-icon.png"), format="PNG", optimize=True)
print("png ", f"{B}/web/apple-touch-icon.png", "180x180")
# width only: the height follows each SVG's own aspect ratio
png("branding/web/ws-logo-horizontal.svg", f"{B}/web/ws-logo-horizontal.png", 1200)
png("branding/web/ws-logo-horizontal-dark.svg", f"{B}/web/ws-logo-horizontal-dark.png", 1200)
png("branding/web/ws-suite-lockup.svg", f"{B}/web/ws-suite-lockup.png", 1200)
png("branding/banner/ws-banner-1500x500.svg", f"{B}/banner/ws-banner-1500x500.png", 1500, 500)
png("branding/banner/ws-banner-1500x500.svg", f"{B}/banner/ws-banner-3000x1000.png", 3000, 1000)
png("branding/banner/ws-social-1200x630.svg", f"{B}/banner/ws-social-1200x630.png", 1200, 630)
pdf("branding/letterhead/ws-letterhead-a4.svg", f"{B}/letterhead/ws-letterhead-a4.pdf")
png("branding/letterhead/ws-letterhead-a4.svg", f"{B}/letterhead/ws-letterhead-a4-preview.png", 1240, 1754, background="#ffffff")
png("branding/letterhead/ws-letterhead-header.svg", f"{B}/letterhead/ws-letterhead-header-300dpi.png", 2480, 591)
png("branding/letterhead/ws-letterhead-footer.svg", f"{B}/letterhead/ws-letterhead-footer-300dpi.png", 2480, 177)
