WS User Guide
=============

A short, illustrated guide to WS: the three GUI variants, every menu and
every setting, and the features WS adds to WSJT-X. Modes and operating
procedures are still covered by the WSJT-X User Guide.

en/ws-main.adoc      main document (includes the chapter files)
en/images/           screenshots

The Settings and menu pictures are drawn from the real Designer forms, so
they can be regenerated whenever a .ui file changes:

  tools/ws-guide-shots.cpp   renders every Settings tab and every menu
    build (MSYS2 mingw64):
      g++ -std=c++17 -O1 tools/ws-guide-shots.cpp -o ws-guide-shots \
          $(pkg-config --cflags --libs Qt5UiTools Qt5Widgets)
    run:
      ws-guide-shots <WS source dir> doc/ws_guide/en/images
      ws-guide-shots <WS source dir> <dir> build/ws_de.qm   (Settings tabs, translated)

  tools/preview.py           single-file HTML preview without asciidoctor
      python tools/preview.py en/ws-main.adoc ws-main.html

The main-window screenshots of the three variants (WS.png, WS-AL.png,
WS-AL_dark_style.png, WS-widescreen.png) are real screenshots.

With WSJT_GENERATE_DOCS ON the build turns it into build/doc/ws-main_en.html
(installed as ws-main-<version>.html, which Help > User Guide opens).
asciidoctor also renders it directly:  asciidoctor en/ws-main.adoc
