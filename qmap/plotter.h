///////////////////////////////////////////////////////////////////////////
// Some code in this file and accompanying files is based on work by
// Moe Wheatley, AE4Y, released under the "Simplified BSD License".
// For more details see the accompanying file LICENSE_WHEATLEY.TXT
///////////////////////////////////////////////////////////////////////////

#ifndef PLOTTER_H
#define PLOTTER_H

#include <QtGui>
#include <QFrame>
#include <QImage>
#include <QList>
#include <QToolTip>
#include <cstring>
#include "commons.h"
#include "decode_label.h"
#include "qmap_runtime_config.h"

#define VERT_DIVS 7	//specify grid screen divisions
#define HORZ_DIVS 20

class CPlotter : public QFrame
{
  Q_OBJECT
public:
  explicit CPlotter(QWidget *parent = 0);
  ~CPlotter();

  QSize minimumSizeHint() const override;
  QSize sizeHint() const override;
  QColor  m_ColorTbl[256];
  bool    m_bDecodeFinished;
  int     m_plotZero;
  int     m_plotGain;
  float   m_fSpan;
  qint32  m_nSpan;
  qint32  m_binsPerPixel;
  qint32  m_fQSO;
  qint32  m_DF;
  qint32  m_tol;
  qint32  m_fCal;

  void draw(float sw[], int i0, float splot[]);		//Update the waterfalls
  void SetRunningState(bool running);
  void setPlotZero(int plotZero);
  int  getPlotZero();
  void setPlotGain(int plotGain);
  int  getPlotGain();
  void SetCenterFreq(int f);
  qint64 centerFreq();
  void SetStartFreq(quint64 f);
  qint64 startFreq();
  void SetFreqOffset(quint64 f);
  qint64 freqOffset();
  int  plotWidth();
  void setNSpan(int n);
  void UpdateOverlay();
  void setDataFromDisk(bool b);
  void setTol(int n);
  void setBinsPerPixel(int n);
  int  binsPerPixel();
  void setFQSO(int n, bool bf);
  void setFcal(int n);
  void setNkhz(int n);
  void DecodeFinished();
  void DrawOverlay();
  int  fQSO();
  int  DF();
  int  autoZero();
  void setPalette(QString palette);
  // Wide-waterfall tick spacing in kHz (KB2SA suggestion 2026-05-10):
  // hardcoded 5 kHz cramped labels on smaller screens. Caller picks
  // 5 / 10 / 20 / 50 from the View menu; default 5 preserves the
  // legacy display. Triggers UpdateOverlay() so the change shows
  // without waiting for the next waterfall row.
  void setFreqPerDiv(double khz);
  double freqPerDiv() const { return m_FreqPerDiv; }
  void setFsample(int n);
  void setMode65(int n);
  void set2Dspec(bool b);
  double fGreen();
  void setLockTxRx(bool b);
  double rxFreq();
  double txFreq();
//  void updateFreqLabel();

  // Receive the WideGraph-managed list of decoded callsigns. Cached
  // here and rendered as text labels at the top of the waterfall in
  // paintEvent / DrawOverlay. Triggers an update().
  void setDecodeLabels(const QList<DecodeLabel>& labels);
  // Master opacity for the decoded-callsign overlay (0..255). Applied
  // to the text directly; background and tick scale proportionally so
  // their relative contrast is preserved. Triggers an update().
  void setDecodeLabelAlpha(int alpha);
  // Font-size picker for the overlay (Small=7, Normal=8, Medium=10,
  // Large=12). Triggers an update() so the next paint uses the new
  // metrics.
  void setDecodeLabelFontSize(DecodeLabelFontSize sz);
  DecodeLabelFontSize decodeLabelFontSize() const { return m_decodeFontSize; }
  // Anchor position for the callsign overlay (Top = legacy, Bottom =
  // sit just above the divider so signals at the top of the waterfall
  // aren't obscured). Triggers an update().
  void setDecodeLabelPosition(DecodeLabelPosition p);
  DecodeLabelPosition decodeLabelPosition() const { return m_decodeLabelPosition; }

signals:
  void freezeDecode0(int n);
  void freezeDecode1(int n);
  // Click-to-work: emitted when mousePressEvent hits a callsign overlay
  // rectangle. The widegraph relays this to mainwindow which writes the
  // call + freq + grid into mem_qmap for WSJT-X to pick up on the next
  // tick. freq_khz is the value stored on the DecodeLabel (audio-offset
  // kHz within the QMAP wide-mode window), which mainwindow combines
  // with int(fcenter) to derive the absolute RF in Hz. grid is empty
  // when the label was never seen with a grid (replies often omit one).
  // mode is the Q65 sub-mode designator ("60A" etc) from the decode line.
  // start_qso: false = single click (transfer only), true = double click
  // (transfer AND enable Tx in WSJT-X to start the QSO).
  void callsignClicked(const QString& call, double freq_khz,
                       const QString& grid, const QString& mode, int even_period,
                       double fsked_khz, bool start_qso);

protected:
  //re-implemented widget event handlers
  void paintEvent(QPaintEvent *event) override;
  void resizeEvent(QResizeEvent* event) override;
  void mouseMoveEvent(QMouseEvent * event) override;

private:

  void MakeFrequencyStrs();
  void UTCstr();
  int XfromFreq(float f);
  float FreqfromX(int x);
  qint64 RoundFreq(qint64 freq, int resolution);
  // Render m_decodeLabels at the top of the waterfall. Called from
  // paintEvent after the waterfall pixmap is drawn. Stacks labels
  // vertically when their x-bounding-boxes collide, capped at 5 rows.
  void paintDecodeLabels(QPainter& painter);

  QPixmap m_WaterfallPixmap;
  QPixmap m_ZoomWaterfallPixmap;
  QPixmap m_2DPixmap;
  // Zoom-waterfall pixmap buffer: row stride = activeNfft() bins, 400
  // rows of waterfall history. Sized at MAX_NFFT*400 (~50 MB) so wide-
  // mode (256 kHz / 131072 bins) addresses the full active spectrum.
  // At 96 kHz only the first 32768 columns per row are written; the
  // unused tail is just slack.
  unsigned char m_zwf[qmap_runtime::MAX_NFFT*400];
  QPixmap m_ScalePixmap;
  QPixmap m_ZoomScalePixmap;
  QSize   m_Size;
  QString m_Str;
  // Sized for wide-mode worst case: m_hdivs at 256 kHz / FreqPerDiv=0.2
  // is ~1281; was hardcoded 483 (= 96 kHz baseline) which blew up at
  // wide-mode launch via m_HDivText[1281] OOB write. Headroom for any
  // future MAX_IQ_RATE_HZ bump.
  QString m_HDivText[2048];
  bool    m_Running;
  bool    m_paintEventBusy;
  bool    m_2Dspec;
  bool    m_paintAllZoom;
  bool    m_bLockTxRx;
  double  m_CenterFreq;
  double  m_fGreen;
  double  m_TXfreq;
  qint64  m_StartFreq;
  qint64  m_ZoomStartFreq;
  qint64  m_FreqOffset;
  qint32  m_dBStepSize;
  qint32  m_FreqUnits;
  qint32  m_hdivs;
  bool    m_dataFromDisk;
  QString m_sutc;
  qint32  m_line;
  qint32  m_hist1[256];
  qint32  m_hist2[256];
  qint32  m_z1;
  qint32  m_z2;
  qint32  m_nkhz;
  qint32  m_fSample;
  qint32  m_mode65;
  qint32  m_i0;
  qint32  m_xClick;
  qint32  m_TXkHz;
  qint32  m_TxDF;
  bool    callsign_overlay_clicked;
  double  m_FreqPerDiv {5.0};  // wide-waterfall tick spacing kHz (View menu)
  // Decoded-callsign labels currently visible on the waterfall.
  // Populated from WideGraph::addDecodeLabel via setDecodeLabels.
  QList<DecodeLabel> m_decodeLabels;
  // Click-to-work hit-test cache. Filled inside paintDecodeLabels on
  // every paint pass; each pair holds the on-screen rect plus the
  // DecodeLabel that painted it. mousePressEvent walks this list to
  // find which (if any) callsign was clicked. Cleared at the start of
  // each paintDecodeLabels so labels that aged out / were dropped from
  // m_decodeLabels are not clickable as ghosts.
  QList<QPair<QRect, DecodeLabel>> m_decodeLabelHitRects;
  // Overlay master alpha (0..255). 255 = fully opaque legacy look.
  int     m_decodeLabelAlpha {255};
  // Overlay font size (Small=7, Normal=8 (default), Medium=10, Large=12).
  DecodeLabelFontSize m_decodeFontSize {DecodeLabelFontSize::Normal};
  // Overlay anchor — Top stacks down from waterfall top (legacy);
  // Bottom stacks up from the divider so labels sit in the faded/older
  // part of the waterfall and leave fresh signal traces visible.
  DecodeLabelPosition m_decodeLabelPosition {DecodeLabelPosition::Top};

private slots:
  void mousePressEvent(QMouseEvent *event) override;
  void mouseDoubleClickEvent(QMouseEvent *event) override;
};

#endif // PLOTTER_H
