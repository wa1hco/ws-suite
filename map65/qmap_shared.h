#ifndef QMAP_SHARED_H
#define QMAP_SHARED_H

#include <QtGlobal>   // qint64

// Layout of the "mem_qmap"[_N] shared-memory segment that WSJT-X polls for
// click-to-work. This is a plain-C++ MIRROR of WSJT-X's `qmapcom`
// (widgets/mainwindow.cpp) and QMAP's Fortran-COMMON `decodes_`
// (qmap/commons.h) -- the three MUST stay byte-for-byte identical up to and
// including click_start_qso. MAP65 is NOT the decode source, so it only
// writes the click_* fields; the decode-payload fields exist purely to place
// click_seq at the same offset every client agrees on.
//
// The reverse-channel fields (dxcall_seq/dxcall) are APPENDED after the
// original layout on purpose: QMAP never reads or writes past
// click_start_qso, so extending the tail leaves the existing QMAP<->WSJT-X
// contract untouched. Only WSJT-X (writer) and MAP65 (reader) use them.
struct QmapShared {
  int    ndecodes;
  int    ncand;
  int    nQDecoderDone;
  int    nWDecoderBusy;
  int    nWTransmitting;
  int    kHzRequested;
  char   result[50][72];
  // Click-to-work (MAP65 -> WSJT-X). MAP65 fills these and bumps click_seq;
  // WSJT-X edge-detects click_seq each tick and acts on the click.
  int    click_seq;
  char   click_callsign[16];
  qint64 click_rf_hz;          // absolute Hz, so WSJT-X derives its own audio offset
  int    click_even_period;
  char   click_grid[8];
  char   click_mode[8];        // Q65 sub-mode designator ("60A"); empty for JT65
  int    click_start_qso;      // 1 = double-click: also enable Tx in WSJT-X
  // Reverse channel (WSJT-X -> MAP65): WSJT-X writes its current DX Call and
  // Grid here and bumps dxcall_seq whenever either changes, so MAP65 can
  // mirror both.
  int    dxcall_seq;
  char   dxcall[16];
  char   dxgrid[8];
};

// Separate segment "mem_qmap_fwd"[_N] carrying MAP65's full decode stream to
// WSJT-X for processMessage(). Kept out of QmapShared because forwarding every
// decode needs an array that would blow the 4096-byte mem_qmap segment (and
// break QMAP). Each entry is a ready-to-parse WSJT-X DecodedText line
// ("HHMMSS -SS  D.d FFFF M  MESSAGE", # = JT65 / @ = Q65). write_seq is a
// monotonic total-written counter; a reader tracks its own last_read_seq and
// consumes lines[write_seq % QMAPFWD_CAP], skipping the tail on overrun. Layout
// hand-mirrored in widgets/mainwindow.cpp (struct QmapFwd).
static constexpr int QMAPFWD_CAP  = 64;
static constexpr int QMAPFWD_LINE = 96;
struct QmapFwd {
  int  write_seq;
  char lines[QMAPFWD_CAP][QMAPFWD_LINE];
};

#endif // QMAP_SHARED_H
