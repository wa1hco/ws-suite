#ifndef DEVSETUP_H
#define DEVSETUP_H

#include <QDialog>
#include "ui_devsetup.h"

class DevSetup : public QDialog
{
  Q_OBJECT
public:
  DevSetup(QWidget *parent=0);
  ~DevSetup();

  void initDlg();
  qint32  m_fCal;
  qint32  m_udpPort;
  qint32  m_astroFont;
  qint32  m_dB;
  // 0 = "auto" (detect via Linrad TCP handshake at startup),
  // any positive value = forced rate in Hz (overrides auto).
  // Persisted as qmap.ini [Linrad]/sample_rate_mode = "auto" | "<hz>".
  qint32  m_sampleRateModeOrZero;
  // Linrad endpoint: host (UDP+TCP) and TCP parameter-server port.
  // Persisted as qmap.ini [Linrad]/host and [Linrad]/tcp_port. UDP
  // data port is m_udpPort above ([Common]/UDPport). Restart QMAP to
  // apply changes — main.cpp's startup probe and WideGraph's late-
  // probe both read these from the file at construction time.
  QString m_linradHost;
  qint32  m_linradTcpPort;

  double  m_fAdd;
  double  m_TxOffset;

  bool    m_network;
  bool    m_restartSoundIn;

  int     m_myCallColor;

  QString m_myCall;
  QString m_myGrid;
  QString m_saveDir;
  QString m_azelDir;

  // LiveCQ destinations are independent: any combination of Official /
  // N6NU / Custom may be enabled and each CQ decode fans out to all of
  // them. Custom is inert while m_otherUrl is empty.
  QString m_otherUrl;      //liveCQ
  bool m_livecqEnabled;    //liveCQ master switch
  bool m_spotPSK = false;  // spot own decodes to PSK Reporter
  bool m_spotPSKTcpIp = false;  // ... over TCP/IP instead of UDP
  bool m_livecqOfficial;   //liveCQ -> w3sz.com
  QString m_livecqW3szAppId;  //liveCQ program name reported to w3sz.com
  bool m_livecqN6NU;       //liveCQ -> livecq.n6nu.org
  bool m_livecqCustom;     //liveCQ -> m_otherUrl

  // Multi-instance identity (qmap/MULTI_INSTANCE_DESIGN.md Phase 1).
  qint32  m_instanceId;   // 1-4
  QString m_configName;

public slots:
  void accept();

private:
  int r,g,b,r0,g0,b0,r1,g1,b1,r2,g2,b2,r3,g3,b3;
  Ui::DialogSndCard ui;
};

#endif // DEVSETUP_H
