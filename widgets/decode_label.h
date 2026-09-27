#ifndef WSJTX_DECODE_LABEL_H
#define WSJTX_DECODE_LABEL_H

#include <QString>
#include <QtGlobal>

// One decoded-callsign label rendered on top of the WSJT-X audio
// waterfall at its audio-offset x-position. WideGraph maintains a
// list (refreshed each time DisplayText emits decodedCallsign);
// CPlotter reads the list via setDecodeLabels() and overlays in
// paintEvent. Same idea as the QMAP-side overlay (qmap/decode_label.h)
// — separate copy here so the WSJT-X tree stays self-contained.
struct DecodeLabel {
    double  freq_hz;        // audio offset (Hz) of the decoded signal
    QString callsign;       // sender's call (from decodedText.deCallAndGrid)
    QString grid;           // 4-char Maidenhead grid if present in the
                            // decoded message (CQ line); preserved across
                            // dedup refreshes so a later reply that omits
                            // the grid doesn't blank it out
    qint64  last_seen_ms;   // wall-clock of most recent fresh decode
    bool    is_cq;          // CQ / CQDX / QRZ message → highlight differently
    bool    is_even_period; // derived from (time_sec / TRperiod) % 2 == 0
    bool    is_active;      // matches WSJT-X's current "DX Call" entry
    bool    heard_me;       // LiveCQ says this station decoded MY signal
                            // within the aging window (Q65 pile-up mode)

    DecodeLabel(double f, const QString& c, qint64 t, bool cq, bool even)
        : freq_hz(f), callsign(c), last_seen_ms(t), is_cq(cq),
          is_even_period(even), is_active(false), heard_me(false) {}
    DecodeLabel()
        : freq_hz(0), last_seen_ms(0), is_cq(false),
          is_even_period(false), is_active(false), heard_me(false) {}
};

// Font-size choice for the callsign overlay. Menu values:
//   7 pt  = Small   (tightest packing, hardest to read at distance)
//   8 pt  = Normal  (default — fits dense FT8 bands, still legible)
//   10 pt = Medium  (more readable, more stacking pressure)
//   12 pt = Large   (easiest to read, biggest stacking pressure)
enum class DecodeLabelFontSize {
  Small  = 7,
  Normal = 8,
  Medium = 10,
  Large  = 12,
};

// Anchor position for the callsign overlay on the upper waterfall.
// Mirrors qmap/decode_label.h and map65/decode_label.h. Top = stack
// down from the top of the waterfall (legacy); Bottom = stack up from
// the bottom edge (just above the 2D-spectrum divider at m_h1) so
// fresh strong signals at the top remain visible.
enum class DecodeLabelPosition {
  Top    = 0,
  Bottom = 1,
};

#endif // WSJTX_DECODE_LABEL_H
