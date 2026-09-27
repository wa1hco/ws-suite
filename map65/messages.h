#ifndef MESSAGES_H
#define MESSAGES_H

#include <QDialog>
#include "commons.h"
#include "../Network/PSKReporter.hpp"
#include <memory>
#include <QNetworkReply>
#include <QNetworkAccessManager>
#include <QDateTime>
#include <QPointer>

namespace Ui {
  class Messages;
}

class PSKReporter; // Forward declaration -- avoids including PSKReporter.h here

class Messages : public QDialog
{
  Q_OBJECT

public:
  explicit Messages (QString const& settings_filename, QWidget * parent = nullptr);
  void setText(QString t, QString t2);
  void setColors(QString t, bool bold = false);
  // Disk playback state, pushed in from MainWindow alongside setText: the
  // uploads carry it as the flags disk bit, and disk decodes go to the N6NU
  // server only (it hides them by default).
  void setDiskMode(bool disk) { m_diskMode = disk; }
  void init_psk_reporter(bool const& param1, bool const& param2, QString const& param3);
  void setClosingForShutdown(bool value) { m_closingForShutdown = value; }

  ~Messages();
  
signals:
  void click2OnCallsign(QString t2, bool ctrl, bool isDoubleClick);
  void errorOccurred(const QString &error);  // Emitted on error
  void sendLocalStationData(QString const& call, QString const& grid, QString const& antenna, QString const& rigInformation);
  void sendRemoteStationData (QString const& call, QString const& grid, quint64 freq, QString const& mode, int snr, QDateTime qSpotTime);
  void sendLocalStationData2(QString const& call, QString const& grid, QString const& theUrl);

protected:  
  void closeEvent(QCloseEvent *event);
  
private slots:
  void selectCallsign2(bool ctrl, bool isDoubleClick);
  void on_cbCQ_toggled(bool checked);
  void on_cbCQstar_toggled(bool checked);
  void onFinished(QNetworkReply *reply);     // Handles the reply from web request

private:
  Ui::Messages *ui;
  QString m_settings_filename;
  QString m_t;
  QString m_t2;
  QString m_colorBackground;
  QString m_color0;
  QString m_color1;
  QString m_color2;
  QString m_color3;
  // True for every preset except "Classic" -- keeps Classic's appearance
  // byte-for-byte unchanged for existing users.
  bool m_boldText = false;

  // Shared PSK Reporter client (Network/PSKReporter, as in upstream
  // WSJT-X 3.0.2); it runs on the GUI thread with its own timer and socket.
  std::unique_ptr<PSKReporter> pskReporter_;
  bool pskReporterTcpip_ = false;   // connection type pskReporter_ was made with
  void createPSKReporter(bool tcpip);
  // LiveCQ uploads go through a plain async QNetworkAccessManager (like
  // QMAP and WSJT-X). The old raw-socket liveCQSender held ONE socket, so
  // it could not fan a decode out to several destination hosts.
  QNetworkAccessManager* m_livecqNAM = nullptr;

  bool m_closingForShutdown = false;
  bool m_diskMode = false;
  bool m_cqOnly;
  bool m_cqStarOnly;
  bool doLiveCQ=true; //liveCQ
  void CreateLiveCQ(QStringList cqliveText);  //liveCQ
  void sendPSKReporterData(QStringList decodeList);  //PSKReporter
  void sendLiveCQData(QStringList decodeList);  // This will trigger the web request
  void initializePSKReporting();
  bool testCall(QString w);  //liveCQ

  QString w3szUrlAddr="https://w3sz.com/livecq_update.php"; //liveCQ
  QString n6nuUrlAddr="https://livecq.n6nu.org/api/livecq_update"; //liveCQ

};

#endif
