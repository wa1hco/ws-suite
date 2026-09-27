# WS artwork record

The design record for the WS, WS-MAP and EME65 icons, the installer header and the
branding kit (web logos, banners, letterhead). It documents what the artwork is,
how it was made, which fonts it uses and under which licences, so the facts are at
hand for a copyright registration or a trademark application.

**This file records facts; it is not legal advice.** See "Before registering" below.

## Identity

| Item | Value |
|---|---|
| Product | WS - Weak Signal Digital Modes Suite (`PROJECT_DESCRIPTION` in `CMakeLists.txt`) |
| Programs | WS (`ws.exe`), WS-MAP (`wsmap.exe`), EME65 (`eme65.exe`) |
| Design accepted | 2026-09-11 |
| Design direction and final selection | Andreas Junge, N6NU |
| Program maintainer | Uwe Risse, DG2YCB |
| Rights holder | Uwe Risse, DG2YCB (decided by Andreas Junge, 2026-09-11) |

### The marks

- **WS** - a blue graticule globe with the letters "WS" in white, outlined in navy.
- **WS-MAP** - the same globe, same size, with the letters "MAP".
- **EME65** - a smaller graticule globe at lower left and a grey moon with four craters at upper right, no letters.
- **Wordmark** - "WS" set in DejaVu Sans Bold, navy, with the tagline "Weak Signal Digital Modes Suite" in DejaVu Sans.

## How it was made

- **No third-party artwork.** Every shape is computed: meridians and parallels projected
  orthographically onto a sphere, radial gradients, and circles for the moon and its
  craters. The Wikimedia Commons globes (`File:Globe.svg`, `File:GreenGlobe.svg`) and the
  former WSJT-X globe were evaluated during the design and **not used**.
- **Letters are outlines** taken from the fonts below, converted to SVG paths by
  `design/ttf2path.py`. No font file is distributed and none is needed to render the SVGs.
- **Provenance of the drawing code.** The generator scripts in `design/` were written with
  Claude, Anthropic's AI assistant, working to Andreas Junge's direction: he proposed the
  concepts (earth and moon for the EME programs), chose between generated alternatives,
  and specified the final changes (WS unchanged; WS-MAP as the same globe with "MAP" and
  no moon; EME65 without the "65"). Record this accurately in any registration - see below.

### Tools

| Step | Tool |
|---|---|
| SVG generation | Python 3 scripts in `design/` (`make_artwork.py`, `wsart.py`, `ttf2path.py`) |
| Bitmaps | `rsvg-convert` (librsvg, MSYS2 mingw64) and Pillow 12.3, via `make_icons.py` |

## Fonts

Identified from each file's own `name` table. Source: Debian 13 packages on the build
host, `fonts-dejavu-core 2.37-8` and `fonts-liberation 1:2.1.5-3`.

| Use | Full name | Version | PostScript name | SHA-256 of the font file |
|---|---|---|---|---|
| "WS", "MAP", wordmark, program names | DejaVu Sans Bold | Version 2.37 | DejaVuSans-Bold | `a4c5bc453ca281d90ea079e596da7ae0dfeb5777497c29ec254e76d97ff6f890` |
| Tagline, letterhead footer, installer header | DejaVu Sans | Version 2.37 | DejaVuSans | `57f73e11f51999432bf7ab22ce55b6f945d5eca1bf824404cfa9ec2e3718c84e` |

**DejaVu Sans** - manufacturer "DejaVu fonts team", http://dejavu.sourceforge.net.
Copyright notice in the font: "Copyright (c) 2003 by Bitstream, Inc. All Rights Reserved.
Copyright (c) 2006 by Tavmjong Bah. All Rights Reserved. DejaVu changes are in public
domain". Licence: the Bitstream Vera Fonts licence (with the Arev fonts licence for glyphs
from Tavmjong Bah), http://dejavu.sourceforge.net/wiki/index.php/License. It permits use,
copying and embedding without charge. Its naming restriction applies to modified *fonts*
("Bitstream Vera" / "Arev" may not be used in a modified font's name), not to artwork that
contains letter shapes. Neither "Bitstream Vera" nor "DejaVu" appears in any mark here.

**Liberation Sans Bold 2.1.5** (SIL Open Font License 1.1, "Liberation" is a Red Hat
trademark) was compared during the design and **not used**.

**Segoe UI** (Microsoft) was used only for early preview renders and is **not used** in any
committed artwork.

## Colours

| Role | Hex |
|---|---|
| Globe gradient - highlight | `#5fb4ff` |
| Globe gradient - mid | `#1f6fd1` |
| Globe gradient - edge, wordmark navy | `#0b2f73` |
| Globe outline | `#08224f` |
| Graticule lines | `#ffffff` at 55 % opacity |
| Letters | `#ffffff`, outline `#0b2f73` at 5 % of the type size |
| Moon gradient | `#fdfdf8` to `#a3a8b1` |
| Moon rim / craters | `#5b6270` / `#8b929c` at 50 % |
| Tagline blue / accent sky / grey text | `#1f6fd1` / `#5fb4ff` / `#4a5160` |
| Banner background | `#071f4f` to `#1a5fbf`, diagonal |

## Geometry (1024 x 1024 icon box)

The globe fills the icon box almost completely: a circle inscribed in a square reads
smaller than a shape reaching the corners, and beside WSJT-X and JTDX on a Windows
desktop an earlier 92 % globe looked undersized (Uwe, 2026-09-11).

| Element | Value |
|---|---|
| Projection | orthographic, centre latitude 23 deg, centre longitude -20 deg |
| WS / WS-MAP globe | centre (512, 512), radius 500, graticule every 30 deg, line width 15 |
| WS letters | centred on the globe, at most 640 wide, at most 404 units type size |
| MAP letters | centred on the globe, at most 724 wide, at most 319 units type size |
| EME65 earth | centre (355, 669), radius 340, graticule every 45 deg, line width 19 |
| EME65 moon | centre (809, 215), radius 205; craters at (-0.30, -0.22) r 0.22, (0.30, 0.20) r 0.15, (0.02, 0.48) r 0.11, (-0.40, 0.32) r 0.09, in moon radii |

## Files

| File | What |
|---|---|
| `ws_icon.svg`, `wsmap_icon.svg`, `eme65_icon.svg` | the three program icons, master artwork |
| `ws_globe.svg` | the globe without letters, for the utilities' icon set |
| `installer_logo.svg` | NSIS installer header, 150 x 57 |
| `DragNDrop Background.svg` | macOS disk-image background (pre-existing artwork, text changed to WS) |
| `branding/web/` | horizontal logo (light, dark), suite lockup, favicon set, app icons 192/512 |
| `branding/banner/` | 1500 x 500 banner (and 3000 x 1000), 1200 x 630 social preview |
| `branding/letterhead/` | A4 letterhead as SVG and PDF, header and footer strips at 300 dpi for word processors |
| `make_icons.py` | renders every committed bitmap from the SVGs |
| `design/` | the generator scripts that write the SVGs |

## Before registering

Points to take to an intellectual-property attorney; they are flagged here, not settled:

- **AI-assisted material.** The U.S. Copyright Office requires applicants to disclose
  AI-generated content and protects only the human-authored contribution (for example the
  selection, arrangement and modifications a person made). The provenance section above
  is written so that disclosure can be made accurately. Other countries' offices differ.
- **Trademark.** Rights come from use as a source identifier, and registration is by
  goods/services class (software: Nice class 9). Two-letter marks such as "WS" and simple
  globe devices are common in software, so a clearance search for "WS" and similar globe
  marks in class 9 is advisable before filing.
- **Rights holder and the chain of title.** The marks are to be held by Uwe Risse, DG2YCB,
  while the human authorship of the artwork is Andreas Junge's (direction, selection and the
  final design decisions). Where authorship and ownership sit with different people, offices
  generally expect the transfer to be in writing and signed; a short assignment from Andreas
  to Uwe, dated and naming this artwork, is worth having on file before any registration.
