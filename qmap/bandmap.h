#ifndef BANDMAP_H
#define BANDMAP_H

#include <QDialog>
#include <QList>
#include <QWidget>
#include "decode_label.h"

class QComboBox;
class QLabel;
class QTimer;
class BandMapPlot;

// Phase 5 of qmap/MULTI_INSTANCE_DESIGN.md: a vertical, frequency-axis
// view of recently-decoded stations, retained for a configurable window
// (10/15/30 min) rather than just the current-minute sticky overlay
// WideGraph already shows. Independent feature -- built on the same
// DecodeLabel data shape and the same callsignClicked signal contract
// as CPlotter/WideGraph, but keeps its own longer-retention list rather
// than reusing WideGraph's (which is deliberately short-lived, tied to
// the waterfall's own TRperiod-based aging).
class BandMap : public QDialog
{
  Q_OBJECT
public:
  explicit BandMap(QString const& settings_filename, QWidget* parent = nullptr);
  ~BandMap();

  // Same parameter shape as WideGraph::addDecodeLabel so MainWindow can
  // feed both from the same call sites with the same arguments.
  void addEntry(double freq_khz, const QString& callsign, bool is_cq = false,
                const QString& grid = QString{}, bool is_active = false,
                const QString& mode = QString{}, int even_period = -1,
                double fsked_khz = -1.0);
  void ageEntries();
  void clearEntries();
  void saveSettings();
  // Called from MainWindow::closeEvent before shutdown proceeds: Qt6 (unlike
  // Qt5) also delivers closeEvent to secondary windows while the application
  // quits, which would clear m_wasOpen as if the user had closed the window.
  void markShutdown() { m_shuttingDown = true; }
protected:
  // Keep m_wasOpen tracking the USER's intent (shown = open, X = closed):
  // saveSettings() persists this member, because the destructor saves once
  // more during teardown when isVisible() is already false and would
  // otherwise overwrite the state captured at closeEvent time (the Astro
  // window documents the same trap in MainWindow::closeEvent).
  void showEvent(QShowEvent* e) override;
  void closeEvent(QCloseEvent* e) override;
private:

signals:
  // Mirrors CPlotter::callsignClicked exactly -- MainWindow connects
  // this straight to the same handleCallsignClick slot it already uses
  // for the waterfall overlay and decoded-text list.
  void callsignClicked(const QString& call, double freq_khz,
                       const QString& grid, const QString& mode, int even_period,
                       double fsked_khz, bool start_qso);

private slots:
  void onRetentionChanged(int index);

private:
  void loadSettings();
  void updateCountLabel();

  QString m_settings_filename;
  BandMapPlot* m_plot;
  QComboBox* m_retentionCombo;
  QLabel* m_countLabel;
  int m_retentionMinutes {10};
  // Whether the window was open (visible) last session -- read by
  // loadSettings() at construction time, acted on once the dialog is
  // otherwise fully built (see the ctor body).
  bool m_wasOpen {false};
  bool m_shuttingDown {false};
  // BandMap owns the long-retention model; BandMapPlot only renders
  // whatever's pushed to it via setEntries(). Kept separate from
  // WideGraph::m_decodeLabels, which is intentionally short-lived
  // (tied to TRperiod-based aging for the waterfall overlay).
  QList<DecodeLabel> m_entries;
  // Self-contained 1 Hz aging sweep -- BandMap doesn't rely on
  // MainWindow's guiUpdate or WideGraph's own timer for this, so it
  // keeps working correctly even if shown/hidden independently.
  QTimer* m_ageTimer;
};

// The custom-painted vertical frequency axis. Kept as its own QWidget
// (rather than folding into BandMap) so hit-testing/paint logic stays
// separate from the dialog chrome, mirroring how CPlotter is its own
// widget inside WideGraph.
class BandMapPlot : public QWidget
{
  Q_OBJECT
public:
  explicit BandMapPlot(QWidget* parent = nullptr);
  void setEntries(const QList<DecodeLabel>& entries, int retentionMinutes);

signals:
  void callsignClicked(const QString& call, double freq_khz,
                       const QString& grid, const QString& mode, int even_period,
                       double fsked_khz, bool start_qso);

protected:
  void paintEvent(QPaintEvent*) override;
  void mousePressEvent(QMouseEvent*) override;
  void mouseDoubleClickEvent(QMouseEvent*) override;

private:
  int yForFreq(double freq_khz) const;
  bool hitTest(const QPoint& pt, DecodeLabel& out) const;

  QList<DecodeLabel> m_entries;
  int m_retentionMinutes {10};
  double m_minFreq {0.0};
  double m_maxFreq {1.0};
  // Rebuilt every paint; consumed by the two click handlers, matching
  // CPlotter's m_decodeLabelHitRects pattern.
  QList<QPair<QRect, DecodeLabel>> m_hitRects;
};

#endif // BANDMAP_H
