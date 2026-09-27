// Runtime sample-rate / FFT-size config for QMAP wide-mode.
//
// Companion to qmap/libqmap/qmap_params.f90 — both files declare the
// same set of compile-time MAX values, and the C++ side calls
// qmap_set_runtime_config() (defined in the Fortran module) once at
// startup to bump the active values above the 96 kHz baseline.
//
// Defaults (BASELINE_*) reproduce upstream DG2YCB QMAP behaviour
// byte-for-byte; selecting a wider mode is opt-in.
#pragma once

#include <cstddef>
#include <QString>

namespace qmap_runtime {

// Active --config <path> from argv, empty when not passed. Set in
// main(), read by widegraph's auto-relaunch so the spawned QMAP
// inherits the same INI (without it, instance B would auto-relaunch
// into instance A's INI on a rate change).
extern QString g_config_path;

// --decode-log <path> CLI: when non-empty, mainwindow.cpp appends one
// CSV-friendly line per decode to this file (in addition to showing
// it in the GUI). Used by the regression harness to verify decodes
// against mapsim's known-truth manifest.
extern QString g_decode_log_path;

// --open <path> CLI: when non-empty, MainWindow loads this .iq file
// at startup as if the user had picked it via File->Open. Lets the
// regression harness drive a deterministic test cycle without
// fighting the file-dialog via SendKeys.
extern QString g_open_path;

// --exit-after-decode CLI: when true, QMAP calls QApplication::quit()
// shortly after a decode line lands (in either disk-load or wire
// mode). Lets the regression harness watch QProcess.finished instead
// of blocking on a worst-case timer; needed in wire mode so QMAP
// releases UDP 50004 between tests. As a no-decode backstop, also
// fires 200 ms after diskDat() finishes when the file was below
// threshold (disk-load only — wire mode relies on the harness's
// per-test timeout).
extern bool g_exit_after_decode;

// --decoder-timing-log <path> CLI: enables K1JT's existing per-stage
// timer instrumentation (call timer(...) sites in qmapa.f90 etc.)
// and writes one summary block per decoder cycle to <path>. Lets the
// harness see WHERE the decoder spends time (fftbig vs candidate
// loop vs q65b inner) so we can spot regressions that approach the
// 6 s real-time bail before they break auto-sequencing.
extern QString g_decoder_timing_log_path;

constexpr int MAX_IQ_RATE_HZ = 256000;
constexpr int MAX_NSMAX      = 60 * MAX_IQ_RATE_HZ;            // 15,360,000
constexpr int MAX_NFFT       = 131072;                         // next pow2 above 32768 * 256/96
constexpr int BASELINE_RATE  = 96000;
constexpr int BASELINE_NFFT  = 32768;
constexpr int BASELINE_NSMAX = 60 * BASELINE_RATE;             // 5,760,000

// Runtime active values mirroring the Fortran qmap_params module
// (nrate_active / nfft_active / nsmax_active). Set by main.cpp via
// setActive() after parsing --samplerate; consumers (mainwindow,
// widegraph) bound their loops by activeNfft() / activeRateHz().
// Defaults preserve 96 kHz baseline so no consumer needs to change
// behaviour just because it now consults the runtime variable.
extern int g_active_rate_hz;   // Hz, e.g. 96000 / 256000
extern int g_active_nfft;      // FFT bin count, e.g. 32768 / 131072
extern int g_active_nsmax;     // = 60 * g_active_rate_hz
extern int g_rate_source;      // 0=baseline 1=cli 2=ini-manual 3=auto-detect
extern int g_bridge_advertised_rate_hz;  // 0 if bridge not reachable; else what TCP handshake reported

inline int activeRateHz()       { return g_active_rate_hz; }
inline int activeNfft()         { return g_active_nfft;    }
inline int activeNsmax()        { return g_active_nsmax;   }
inline int rateSource()         { return g_rate_source;    }
inline int bridgeAdvertisedRateHz() { return g_bridge_advertised_rate_hz; }

inline bool rateMismatch() {
  // Mismatch only flagged if bridge actually replied AND the active
  // (forced) rate doesn't match what it said. Auto-detected sources
  // can never mismatch (they took the bridge's value as truth).
  return g_bridge_advertised_rate_hz > 0 &&
         g_bridge_advertised_rate_hz != g_active_rate_hz;
}

inline const char* rateSourceLabel() {
  switch (g_rate_source) {
    case 1: return "cli";
    case 2: return "manual";
    case 3: return "auto";
    default: return "baseline";
  }
}

void setActive(int rate_hz, int nfft);                  // defined in main.cpp
void setRateSource(int source);                         // defined in main.cpp
void setBridgeAdvertisedRateHz(int rate_hz);            // defined in main.cpp

}  // namespace qmap_runtime

extern "C" {
  // Defined in qmap/libqmap/qmap_params.f90.
  // Call once during main() after argv parsing, before any libqmap
  // call. Validates rate and nfft against MAX_IQ_RATE_HZ / MAX_NFFT;
  // out-of-range values are silently ignored (the active values stay
  // at their baseline defaults).
  void qmap_set_runtime_config(int rate_hz, int nfft);

  // Call from soundin.cpp inputUDP() after each readDatagram() so
  // recvpkt's nrx=+1 inner loop iterates the actual on-wire pair
  // count (legacy 348 at 96 kHz; up to LINRAD_MAX_PAIRS_PER_PACKET
  // in wide-mode). Per-packet, cheap.
  void qmap_set_iz_packet_active(int iz);
}
