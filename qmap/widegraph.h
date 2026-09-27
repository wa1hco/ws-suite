#ifndef WIDEGRAPH_H
#define WIDEGRAPH_H

#include <QDialog>
#include <QList>
#include <QString>

#include "decode_label.h"

class QTimer;

namespace Ui {
  class WideGraph;
}

class WideGraph : public QDialog
{
  Q_OBJECT

public:
  explicit WideGraph (QString const& settings_filename, QWidget * parent = nullptr);
  ~WideGraph();

  void   dataSink2(float s[], int nkhz, int ihsym, int ndiskdata,
                   uchar lstrong[]);
  int    QSOfreq();
  int    nSpan();
  int    nStartFreq();
  float  fSpan();
  void   saveSettings();
  void   setDF(int n);
  int    DF();
  int    Tol();
  void   setTol(int n);
  void   setFcal(int n);
  void   setPalette(QString palette);
  void   setFsample(int n);
  void   setIqRateLabel(int rate_hz, const QString& source);
  // Multi-instance identity (qmap/MULTI_INSTANCE_DESIGN.md Phase 1).
  // Mirrors MainWindow::updateWindowTitle()'s logic so the waterfall
  // window -- arguably the one operators watch/click most in a
  // multi-instance setup -- is identifiable too. Read from settings at
  // construction; MainWindow also calls this live after the Settings
  // dialog is accepted, so a changed config name updates immediately
  // rather than only on next launch.
  void   setInstanceLabel(int id, const QString& name);
  void   setMode65(int n);
  void   setPeriod(int n);
  void   setDecodeFinished();
  double fGreen();
  void   updateFreqLabel();
  void   enableSetRxHardware(bool b);

  // Push one decoded callsign onto the waterfall overlay. Called from
  // mainwindow's decode-append loop (works for both disk-load and
  // wire-mode since both flow through that one place). The freq is
  // the audio-offset kHz column from the decode line; sender is the
  // first non-CQ callsign-shaped token in the message. If the same
  // callsign+freq was added recently the entry is just refreshed
  // (last_seen_ms bumped); otherwise a new entry appears at the top
  // of the waterfall and persists until aged out.
  void   addDecodeLabel(double freq_khz, const QString& callsign, bool is_cq = false,
                        const QString& grid = QString{}, bool is_active = false,
                        const QString& mode = QString{}, int even_period = -1,
                        double fsked_khz = -1.0);
  // Drop entries whose last_seen_ms is older than
  // m_decodeLabelPeriods * m_TRperiod seconds. Called every second
  // by the existing checkStreamLive timer. Default 5 periods (5 min
  // for Q65-60), tunable via [Display]/decode_label_periods.
  void   ageDecodeLabels();

  // Wipe all overlay labels. Called by MainWindow when opening a new
  // file so stale calls from the previous decode don't linger.
  void   clearDecodeLabels();

  // Re-derive the red "active DX call" highlight for every existing
  // label immediately, instead of waiting for each station's next
  // decode. Mirrors MAP65's WideGraph::updateActiveCallsign -- wire it
  // to wherever QMAP's DXcall changes (click-to-work set/clear) so the
  // newly-selected call turns red at once and the previous one clears,
  // rather than the old call staying red until both happen to decode
  // again. Pass "" to clear all highlights.
  void   updateActiveCallsign(const QString& call);

  // Read/write the "show decoded callsigns on waterfall" toggle.
  // Used by MainWindow to mirror the WideGraph row checkbox into the
  // View menu action so the two stay in sync.
  bool   decodeLabelsEnabled() const { return m_decodeLabelsEnabled; }
  void   setDecodeLabelsEnabled(bool on);

  // Wide-waterfall tick spacing in kHz. MainWindow's View menu owns
  // the canonical knob; this getter/setter lets the menu read/write
  // through to the plotter and persist via WideGraph's QSettings.
  int    tickSpacingKhz() const { return m_tickSpacingKhz; }
  void   setTickSpacingKhz(int khz);

  // Decoded-callsign overlay opacity. View menu owns the knob; this
  // getter/setter lets it sync initial check state and route changes
  // through to plotter + persistence. Three accepted values:
  // 255 (None / fully opaque), 200 (Medium), 175 (High).
  int    decodeLabelAlpha() const { return m_decodeLabelAlpha; }
  void   setDecodeLabelAlpha(int alpha);

  // Decoded-callsign overlay font size. View menu owns the picker;
  // this getter/setter lets it sync initial check state and route
  // changes through to plotter + persistence. Four accepted values:
  // Small=7, Normal=8, Medium=10, Large=12.
  DecodeLabelFontSize decodeLabelFontSize() const { return m_decodeFontSize; }
  void   setDecodeLabelFontSize(DecodeLabelFontSize sz);

  // Decoded-callsign overlay anchor position. View menu owns the
  // toggle; Top (legacy) stacks down from the upper-waterfall top edge,
  // Bottom stacks up from the divider so recent strong signals at the
  // top of the waterfall remain visible.
  DecodeLabelPosition decodeLabelPosition() const { return m_decodeLabelPosition; }
  void   setDecodeLabelPosition(DecodeLabelPosition p);

  qint32 m_qsoFreq;

signals:
  void freezeDecode2(int n);
  void f11f12(int n);
  // Emitted whenever the WideGraph-side "Show callsigns" checkbox is
  // toggled by the user, so MainWindow can update the View menu's
  // matching QAction without each side polling the other.
  void   decodeLabelsEnabledChanged(bool on);
  // Click-to-work: relayed up from CPlotter::callsignClicked. MainWindow
  // turns this into a mem_qmap shared-memory write that WSJT-X polls.
  // freq_khz is the audio-offset kHz stored on the DecodeLabel; MainWindow
  // combines it with int(fcenter) to get an absolute RF in Hz before
  // writing. grid carries through to WSJT-X's dxGridEntry; mode is the
  // Q65 sub-mode designator ("60A" etc) that sets WSJT-X's sbTR/sbSubmode.
  void   callsignClicked(const QString& call, double freq_khz,
                         const QString& grid, const QString& mode, int even_period,
                         double fsked_khz, bool start_qso);

public slots:
  void wideFreezeDecode(int n);

protected:
  virtual void keyPressEvent( QKeyEvent *e );
  void resizeEvent(QResizeEvent* event);

private slots:
  void on_waterfallAvgSpinBox_valueChanged(int arg1);
  void on_cbFreqSpanMode_currentIndexChanged(int idx);
  void on_freqSpanValueSpinBox_valueChanged(int arg1);
  void on_zeroSpinBox_valueChanged(int arg1);
  void on_gainSpinBox_valueChanged(int arg1);
  void on_autoZeroPushButton_clicked();
  void on_cbSpec2d_toggled(bool checked);
  void checkStreamLive();

private:
  void renderIqRateLabel();
  // Push the current mode/value into the plotter (setNSpan +
  // setBinsPerPixel). Called from the constructor and from each of
  // the FreqSpan slots; centralises the kHz→nbpp arithmetic.
  void applyFreqSpan();
  // Effective span in kHz given current mode + active rate. Auto =
  // active rate; 96/256 = the fixed value; User = the user spinbox.
  int  effectiveSpanKhz() const;

  Ui::WideGraph * ui;
  QString m_settings_filename;
  bool   m_bLockTxRx;
public:
  double m_TxOffset;
private:
  qint32 m_waterfallAvg;
  qint32 m_fCal;
  qint32 m_fSample;
  qint32 m_mode65;
  qint32 m_TRperiod=60;

  // No-stream detector: setIqRateLabel() caches its arguments here so
  // the 1 Hz checkStreamLive() poll can re-render the label with a
  // "(no stream)" suffix when soundin's last-packet timestamp is older
  // than 2 s. Also caches Linrad TCP host/port so the late-bridge probe
  // (one-shot on stream-live edge when bridgeAdvertisedRateHz()==0)
  // can hit the right endpoint.
  qint32  m_iqRateHz {0};
  QString m_iqRateSource;
  bool    m_iqRateLabelSet {false};
  bool    m_streamLive     {false};
  QTimer* m_streamCheckTimer {nullptr};
  QString m_linradHost;
  quint16 m_linradTcpPort {0};

  // FreqSpan: the *mode* persists, not a literal kHz that gets
  // clamped. cbFreqSpanMode index 0..3 = Auto / 96 / 256 / User.
  // freqSpanValueSpinBox is the editable readout (User mode only;
  // disabled in Auto/96/256). m_freqSpanUserValue retains the User
  // mode's value across mode switches and across baseline-mode
  // auto-relaunches (range 90..256 kHz, regardless of active rate).
  // INI keys: WideGraph/FreqSpanMode = "auto"|"96"|"256"|"user";
  // WideGraph/FreqSpanUserValue = <int>.
  enum FreqSpanMode { SpanAuto = 0, Span96 = 1, Span256 = 2, SpanUser = 3 };
  qint32 m_freqSpanUserValue {96};

  // Decoded-callsign overlay state. List grows on each tap from
  // mainwindow, ages on the 1 Hz checkStreamLive tick. Capped to
  // protect the paint loop from runaway lists on crowded bands —
  // 200 is comfortably more than any plausible EME minute.
  QList<DecodeLabel> m_decodeLabels;
  bool   m_decodeLabelsEnabled {true};
  int    m_decodeLabelPeriods  {5};   // disappear after N×TRperiod of no decode
  // Overlay opacity preset (0..255). UI offers 150/200/255. Persisted
  // to qmap.ini as WideGraph/decode_label_alpha.
  int    m_decodeLabelAlpha    {255};
  // Overlay font size (Small=7, Normal=8 (default), Medium=10, Large=12).
  // Persisted to qmap.ini as WideGraph/decode_label_font_size.
  DecodeLabelFontSize m_decodeFontSize {DecodeLabelFontSize::Normal};
  DecodeLabelPosition m_decodeLabelPosition {DecodeLabelPosition::Top};
  static constexpr int kDecodeLabelMax = 200;

  // Wide-waterfall tick spacing (kHz). Default 5 = legacy display.
  // INI key WideGraph/TickSpacingKhz; menu choices 5/10/20/50.
  int    m_tickSpacingKhz {5};
};

#endif // WIDEGRAPH_H
