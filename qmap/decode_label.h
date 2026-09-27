#ifndef DECODE_LABEL_H
#define DECODE_LABEL_H

#include <QString>
#include <QtGlobal>

// One decoded-callsign label drawn on top of the WideGraph waterfall
// at its audio-offset x-position. WideGraph maintains a list of these
// (refreshed each time mainwindow taps the decode-append path); CPlotter
// reads the list via setDecodeLabels() and overlays them in paintEvent.
//
// Lives in its own header so widegraph.h and plotter.h can both include
// it without creating a circular dependency between the two larger
// headers.
struct DecodeLabel {
    double  freq_khz;       // audio offset (kHz), straight from the decode line
    QString callsign;       // sender's call extracted from the message field
    qint64  last_seen_ms;   // wall-clock of most recent fresh decode
    int     hits;           // for tie-breaking when stacking
    bool    is_cq;          // last decode for this call was a CQ — paints green
    QString grid;           // 4- or 6-char Maidenhead grid, empty when unknown.
                            // Forwarded to WSJT-X by click-to-work.
    QString mode;           // Q65 sub-mode designator from the decode line's
                            // mode column, e.g. "60A","30A","120B". Period
                            // digits + tone-spacing letter. Forwarded to
                            // WSJT-X by click-to-work so a reply matches.
    bool    is_active;      // For now, the callsign forwarded to WSJT-X
    int     even_period;    // Tx-sequence parity of the sender's decode:
                            // 1 = even/first sequence (iseq 0), 0 = odd/second
                            // (iseq 1), -1 = unknown. Forwarded to WSJT-X by
                            // click-to-work to set Tx Even/1st to the OPPOSITE
                            // slot. Correct for Q65-30 because q65b.f90 stamps
                            // the 2nd half-minute :30, so the two 30s periods
                            // inside one minute get opposite parity.
    double  fsked_khz;      // FSKED (sked QSO-channel) kHz for this decode —
                            // q65b.f90 'fsked' column, distinct from freq_khz
                            // (frx, where the signal actually landed). freq_khz
                            // positions the label on the blob; fsked_khz is what
                            // click-to-work sends to WSJT-X so the QSO lands on
                            // the sked channel. -1 = unknown -> fall back to frx.

    // C++11 explicit ctor (gnu++11 mode in this build doesn't accept
    // QList::append({...}) brace-enclosed init lists).
    DecodeLabel(double f, const QString& c, qint64 t, int h, bool cq = false,
                const QString& g = QString{}, bool active = false,
                const QString& m = QString{}, int ep = -1, double fsk = -1.0)
        : freq_khz(f), callsign(c), last_seen_ms(t), hits(h), is_cq(cq),
          grid(g), mode(m), is_active(active), even_period(ep), fsked_khz(fsk) {}
    DecodeLabel() : freq_khz(0), last_seen_ms(0), hits(0), is_cq(false), is_active(false), even_period(-1), fsked_khz(-1.0) {}
};

// Font-size choice for the callsign overlay. Menu values:
//   7 pt  = Small   (tightest packing, hardest to read at distance)
//   8 pt  = Normal  (default — fits dense bands, still legible)
//   10 pt = Medium  (more readable, more stacking pressure)
//   12 pt = Large   (easiest to read, biggest stacking pressure)
enum class DecodeLabelFontSize {
  Small  = 7,
  Normal = 8,
  Medium = 10,
  Large  = 12,
};

// Where to anchor the callsign overlay on the upper waterfall.
//   Top    = stack down from the top edge of the upper waterfall
//            (legacy behaviour; labels can obscure the strongest
//            signals which sit at the top of recent history)
//   Bottom = stack up from the bottom edge of the upper waterfall
//            (just above the divider) — keeps recent signal traces
//            visible and pushes labels into the older / faded area
enum class DecodeLabelPosition {
  Top    = 0,
  Bottom = 1,
};

#endif // DECODE_LABEL_H
