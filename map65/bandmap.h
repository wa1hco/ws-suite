#ifndef BANDMAP_H
#define BANDMAP_H

#include <QWidget>

namespace Ui {
    class BandMap;
}

class BandMap : public QWidget
{
  Q_OBJECT

public:
  explicit BandMap (QString const& settings_filename, QWidget *parent = 0);
  void setText(QString t);
  void setColors(QString t, bool bold = false);

  ~BandMap();

signals:
  // Click-to-work: emitted when a callsign line is clicked. BandMap only
  // ever knows freq+callsign (no mode/grid/period), unlike the waterfall
  // and decoded-text/Messages click paths.
  void callsignClicked(QString hiscall, bool isDoubleClick);

protected:
  void resizeEvent(QResizeEvent* event) override;
  // Saves geometry immediately on every close (individual X, or app-wide
  // shutdown via MainWindow::closeEvent calling close()) rather than
  // relying solely on ~BandMap() -- which only runs at true C++
  // destruction (MainWindow's own teardown at process exit), not on a
  // plain close()/hide(). Messages already has this same explicit save;
  // BandMap didn't, which is why its geometry never persisted.
  void closeEvent(QCloseEvent* event) override;

private slots:
  void selectCallsign2(bool ctrl, bool isDoubleClick);

private:
  Ui::BandMap *ui;
  QString m_settings_filename;
  QString m_bandMapText;
  QString m_colorBackground;
  QString m_color0;
  QString m_color1;
  QString m_color2;
  QString m_color3;
  // True for every preset except "Classic" -- keeps Classic's appearance
  // byte-for-byte unchanged for existing users.
  bool m_boldText = false;
};

#endif
