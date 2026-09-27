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
  void setMode65(int n);
  void set2Dspec(bool b);
  double fGreen();
  void setLockTxRx(bool b);
  double rxFreq();
  double txFreq();
//  void resetWaterfall()
//  void updateFreqLabel();

  // Decoded-callsign overlay (N6NU 2026-05-12, ported from QMAP).
  // WideGraph maintains the list and pushes it here; we render the
  // callsigns as labels on top of the waterfall at the audio-offset
  // x-position. Triggers update() to schedule a paintEvent.
  void setDecodeLabels(const QList<DecodeLabel>& labels);
  // Master alpha (0..255) scaling every label's background/tick/text
  // alpha proportionally. 255 = no change to the legacy colours; lower
  // values let more of the waterfall show through. View menu offers
  // None=255, Medium=200, High=175.
  void setDecodeLabelAlpha(int alpha);
  // Font-size picker for the overlay (Small=7, Normal=8 default,
  // Medium=10, Large=12). Triggers update() to repaint.
  void setDecodeLabelFontSize(DecodeLabelFontSize sz);
  DecodeLabelFontSize decodeLabelFontSize() const { return m_decodeFontSize; }
  // Anchor position for the overlay (Top = legacy stack-down from
  // waterfall top; Bottom = stack-up from the divider so fresh
  // signals at the top of the waterfall remain visible).
  void setDecodeLabelPosition(DecodeLabelPosition p);
  DecodeLabelPosition decodeLabelPosition() const { return m_decodeLabelPosition; }

  // Past-period decode (View -> Show/Decode past decodes on Wide Graph):
  // when enabled, draw() tags every appended waterfall row with its
  // period ("yyMMdd_hhmm") so a click on a past row can identify which
  // saved ring file to replay. Disabled = zero bookkeeping, no behavior
  // change.
  void setPastEnabled(bool b);

signals:
  void freezeDecode0(int n);
  void freezeDecode1(int n);
  // Past-period decode: emitted on a left click in the wideband waterfall
  // when past-period mode is on. tag = "yyMMdd_hhmm" of the clicked row's
  // period, or empty when the row has no tag (scale strip, pre-enable
  // rows, disk replay rows). MainWindow arms/disarms the Decode button.
  void pastPeriodClicked(const QString& tag);
  // Click-to-work: emitted when mousePressEvent/mouseDoubleClickEvent hits
  // a callsign overlay rectangle. WideGraph relays this to MainWindow,
  // which populates its own dxCallEntry/dxGridEntry (MAP65 is standalone,
  // unlike QMAP -- no shared memory involved). start_qso: false = single
  // click (transfer only), true = double click (transfer AND enable Tx).
  void callsignClicked(const QString& call, double freq_khz, bool is_jt65, const QString& grid, bool start_qso);

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
  // Render m_decodeLabels overlay on top of the waterfall pixmap.
  // Stacks colliding labels vertically (max 5 rows) so a busy band
  // doesn't paint labels on top of each other.
  void paintDecodeLabels(QPainter& painter);

  QList<DecodeLabel> m_decodeLabels;
  // Click-to-work hit-test cache. Filled inside paintDecodeLabels on every
  // paint pass; each pair holds the on-screen rect plus the DecodeLabel
  // that painted it. mousePressEvent/mouseDoubleClickEvent walk this list
  // to find which (if any) callsign was clicked.
  QList<QPair<QRect, DecodeLabel>> m_decodeLabelHitRects;
  // Past-period decode: per-row period tags, index 0 = newest (top) row of
  // m_WaterfallPixmap, matching the scroll(0,1,...) direction in draw().
  // Only maintained while m_pastEnabled.
  bool m_pastEnabled = false;
  QList<QString> m_rowTag;
  // Suppresses the freq-select fallback in mouseDoubleClickEvent right
  // after a single click already hit a callsign overlay (mirrors QMAP).
  bool    callsign_overlay_clicked {false};
  // Overlay master alpha. 255 = legacy (fully opaque colours).
  int     m_decodeLabelAlpha {255};
  // Overlay font size. Default Normal=8 pt.
  DecodeLabelFontSize m_decodeFontSize {DecodeLabelFontSize::Normal};
  // Overlay anchor. Top = legacy stacking down from upper-waterfall top.
  DecodeLabelPosition m_decodeLabelPosition {DecodeLabelPosition::Top};

  QPixmap m_WaterfallPixmap;
  QPixmap m_ZoomWaterfallPixmap;
  QPixmap m_2DPixmap;
  unsigned char m_zwf[32768*400];
  QPixmap m_ScalePixmap;
  QPixmap m_ZoomScalePixmap;
  QSize   m_Size;
  QString m_Str;
  QString m_HDivText[483];
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
  qint32  m_mode65;
  qint32  m_i0;
  qint32  m_xClick;
  qint32  m_TXkHz;
  qint32  m_TxDF;

private slots:
  void mousePressEvent(QMouseEvent *event) override;
  void mouseDoubleClickEvent(QMouseEvent *event) override;
};

#endif // PLOTTER_H
