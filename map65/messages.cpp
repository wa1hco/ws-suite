#include "messages.h"
#include <QFont>
#include <QSettings>
#include "SettingsGroup.hpp"
#include "ui_messages.h"
#include "mainwindow.h"
#include "qt_helpers.hpp"
#include "../revision_utils.hpp"
#include "../Logger.hpp"
#include "../Network/PSKReporter.hpp"

#include <QCoreApplication> //liveCQ
#include <QNetworkAccessManager> //liveCQ
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QUrl>
#include <QRegularExpression>
#include <QUrlQuery>
#include <QEventLoop>
#include <QDateTime>
#include <QString>
#include <QThread>
#include <QDebug>

#include <iostream>
#include <string>
#include <memory>

Messages::Messages (QString const& settings_filename, QWidget * parent) :
  QDialog {parent},
  ui {new Ui::Messages},
  m_settings_filename {settings_filename}
{
  ui->setupUi(this);
  setWindowTitle("Messages");
  setWindowFlags (Qt::Dialog | Qt::WindowCloseButtonHint | Qt::WindowMinimizeButtonHint);
  QSettings settings {m_settings_filename, QSettings::IniFormat};
  SettingsGroup g {&settings, "MainWindow"}; // MainWindow group for
                                             // historical reasons
  // Widened from 381 (too narrow, wrapped a full decoded line). A later
  // 600->480 narrowing (based on a screenshot that apparently only showed
  // short CQ-only lines, not a full 67-char Courier New 9pt exchange line)
  // reintroduced wrapping in real use, so back to >=600 plus extra margin
  // since font-metric estimates have undershot the real render width twice
  // now -- prefer a bit of unused space over wrapped text.
  setGeometry (settings.value ("MessagesGeom", QRect {800, 400, 640, 400}).toRect ());
  ui->messagesTextBrowser->setStyleSheet( \
          "QTextBrowser { background-color : #000066; color : red; }");
  ui->messagesTextBrowser->clear();  
  
  QSettings settings2 {m_settings_filename, QSettings::IniFormat};
  SettingsGroup h {&settings2, "Common"};
  QString m_myCall=settings2.value("MyCall","").toString();
  QString m_myGrid=settings2.value("MyGrid","").toString();

  m_cqOnly=false;
  m_cqStarOnly=false;
  QString guiDate;
  QStringList allDecodes =  { "" };
  QStringList allDecodes2 = { "" };
  connect (ui->messagesTextBrowser, &DisplayText::selectCallsign, this, &Messages::selectCallsign2);

  // LiveCQ uploads are plain async HTTP posts; sendLiveCQData re-reads the
  // destination checkboxes from the ini on every decode pass, so settings
  // changes take effect immediately and no worker thread is needed.
  m_livecqNAM = new QNetworkAccessManager(this);
  connect(m_livecqNAM, &QNetworkAccessManager::finished, this, &Messages::onFinished);

  {
    QSettings settings {m_settings_filename, QSettings::IniFormat};
    SettingsGroup g {&settings, "Common"};
    createPSKReporter (settings.value ("PSKReporterTCPIP", false).toBool ());
  }
  if (m_spot_to_psk_reporter) {
    initializePSKReporting();
  }
}
 
Messages::~Messages()
{
  // pskReporter_ is a unique_ptr: it sends what is still queued and closes
  // its socket when Messages is destroyed. (LiveCQ uploads use m_livecqNAM,
  // a plain child of this object.)
  delete ui;
}

void Messages::closeEvent(QCloseEvent *event)
{
  if (!m_closingForShutdown) {
      hide();
      event->ignore(); // Don't close, just hide
      return;
  }

  // app shutdown
  QSettings settings {m_settings_filename, QSettings::IniFormat};
  SettingsGroup g {&settings, "MainWindow"};
  settings.setValue ("MessagesGeom", geometry ());
  settings.sync(); // Ensure data is written to disk 
  event->accept(); // Allow destruction 
}   

// Shared client as in upstream MAP65 3.0.2. The program information
// PSK Reporter shows is EME65 with its own version number.
void Messages::createPSKReporter(bool tcpip)
{
  pskReporterTcpip_ = tcpip;
  pskReporter_.reset (new PSKReporter {
    {tcpip,
     QCoreApplication::applicationDirPath () + "/eclipse.txt",
     QString {"EME65 v" + QCoreApplication::applicationVersion ()}.simplified ()}
  });
}

void Messages::initializePSKReporting()
{  
  QSettings settings {m_settings_filename, QSettings::IniFormat};
  SettingsGroup g {&settings, "Common"}; 
  QString receiverCallsign=settings.value("MyCall","").toString();
  QString receiverLocator=settings.value("MyGrid","").toString();
  if (pskReporter_) pskReporter_->setLocalStation(receiverCallsign, receiverLocator, "N/A", "N/A (EME65)");
}

// QSO-sequence position of a directed message, from the payload word the RX
// decoder produced. Everything here is inferred from decoded RF -- LiveCQ
// reports what came off the air, nothing else. Returns slot 0 for anything
// unclassifiable; grid -> Tx1, report -> Tx2, R-report -> Tx3, RRR/RR73 ->
// Tx4, 73 -> Tx5.
static void classifyPayload(const QString& payloadIn, int* slot, bool* is73, bool* isRR73)
{
  const QString payload = payloadIn.trimmed().toUpper();
  *slot = 0; *is73 = false; *isRR73 = false;
  if (payload == "RR73")      { *slot = 4; *isRR73 = true; }
  else if (payload == "RRR")    *slot = 4;
  else if (payload == "73")   { *slot = 5; *is73 = true; }
  else if (payload.size() >= 3 && payload[0] == 'R'
           && (payload[1] == '-' || payload[1] == '+')) *slot = 3;
  else if (payload.startsWith('-') || payload.startsWith('+')) *slot = 2;
  else if (QRegularExpression("^[A-R]{2}[0-9]{2}$").match(payload).hasMatch()) *slot = 1;
}

void Messages::sendLiveCQData(QStringList decodeList) {
  QSettings settings(m_settings_filename, QSettings::IniFormat);
  SettingsGroup g {&settings, "Common"};
  // Independent destinations (matches WSJT-X/QMAP 2026-08-12): one decode
  // fans out to every enabled URL. Custom is inert unless it parses as
  // http(s). Re-read per decode pass so settings changes apply at once.
  bool livecqEnabled = settings.value("LiveCQEnabled",true).toBool();
  bool livecqOfficial = settings.value("LiveCQDestOfficial",false).toBool();
  bool livecqN6NU = settings.value("LiveCQDestN6NU",true).toBool();
  bool livecqCustom = settings.value("LiveCQDestCustom",false).toBool();
  // Program name reported to w3sz.com: MAP65 as before the re-branding, or EME65.
  bool const w3szLegacyId = settings.value("LiveCQW3SZAppId","MAP65").toString() != "EME65";
  QString m_otherUrl = settings.value("otherUrl","").toString();
  QString m_myCall=settings.value("MyCall","").toString();
  QString m_myGrid=settings.value("MyGrid","").toString();
  bool m_xpol = settings.value("Xpol",false).toBool();
  QString rpol = "--";

  if(!livecqEnabled) return;
  QStringList urls;
  if(livecqOfficial) urls << w3szUrlAddr;
  if(livecqN6NU) urls << n6nuUrlAddr;
  if(livecqCustom && m_otherUrl.trimmed().startsWith("http")) urls << m_otherUrl.trimmed();
  if(urls.isEmpty()) return;
  for (const QString &theLine : decodeList) {
    QStringList thePostLine = theLine.split(" ",SkipEmptyParts);
    if (thePostLine.size() < 9) continue;
    // First message word: a CQ-type keeps the old path; a valid callsign is a
    // DIRECTED message ("K1JT W7GJ RR73") with that word as the addressee.
    const QString w5 = thePostLine.at(5).trimmed().toUpper();
    const bool isCQtype = (w5 == "CQ" || w5 == "QRZ" || w5 == "CQV" || w5 == "CQH" || w5 == "QRT");
    const bool isDir = !isCQtype && testCall(w5);
    if((isCQtype || isDir) && m_myCall.length() >=3 && m_myGrid.length()>=4) {
      if(allDecodes.filter(theLine.mid(0,53)).length() == 0) {
        allDecodes.append(theLine);
        QString freq = thePostLine.at(0).trimmed();
        QString dF = thePostLine.at(1).trimmed();
        QString utcdatetimestringOriginal = guiDate + " " + thePostLine.at(3).trimmed() + "00"; //needs 2 spaces between date and time
        QDateTime utcdatetimeUTC = QDateTime::fromString(utcdatetimestringOriginal, "yyyy MMM dd  HHmmss");
        utcdatetimeUTC.setTimeZone(QTimeZone::utc());
        QString utcdatetimeUTCString = utcdatetimeUTC.toString("yyyy-MM-ddTHH:mm:ss");
        utcdatetimeUTCString = utcdatetimeUTCString + "Z";
        QString dB = thePostLine.at(4).trimmed();
        QString msgType = isDir ? QString("DIR") : w5;
        QString tocall  = isDir ? w5 : QString();
        QString payload;                 // last message word of a DIR, if any
        QString callsign = "";
        QString grid = "--";
        QString mode="";
        QString txpol = " ";
        QString dT = "";
        QString modeChar = "";
        // Handle CQ CALL but NO GRID -- dot at 7
      if(thePostLine.at(7).contains(".")) {
          callsign = thePostLine.at(6).trimmed().toUpper();
          bool isCall = testCall(callsign);
          if(!isCall) continue;
          dT =thePostLine.at(7).trimmed();
          modeChar = thePostLine.at(8).trimmed(); 
          if(modeChar.contains("#")) mode = QString("JT65") + modeChar.back();
          else if(modeChar.contains(":")) mode = QString("Q65-60") + modeChar.back();          
          if(m_xpol) {
            rpol = thePostLine.at(2).trimmed();
          } else {
            rpol = "--";
          }
          txpol = "--";  
          
        // Handle CQ CALL GRID or CQ XXX CALL -- dot at 8
        } else if (thePostLine.at(8).contains(".")) {
          // Test for callsign at thePostLine(6)
          callsign = thePostLine.at(6).trimmed().toUpper();
          bool isCall = testCall(callsign);
          if(isCall) {
            if (isDir) {
              // TOCALL CALL X: X is a grid (Tx1) or the exchange payload
              // (report / R-report / RRR / RR73 / 73). classifyPayload()
              // decides; only a real grid is stored as one.
              payload = thePostLine.at(7).trimmed();
            } else {
              grid = thePostLine.at(7).trimmed();
            }
            // Handle CQ XXX CALL
          } else {
            if (isDir) continue;         // DIR sender must be a callsign
            callsign = thePostLine.at(7).trimmed().toUpper();
            bool isCall = testCall(callsign);
            if(!isCall) continue;
          }          
          dT =thePostLine.at(8).trimmed();
          modeChar = thePostLine.at(9).trimmed();
          if(modeChar.contains("#")) 
          {  
            mode = QString("JT65") + modeChar.back();            
            if (m_xpol) {
              rpol = thePostLine.at(2).trimmed();
              if(thePostLine.length()==11) {
                if(thePostLine.at(10).contains("H")) txpol = "H";
                else if(thePostLine.at(10).contains("V")) txpol = "V";
                else txpol+"--";
              } else txpol="--";
            }
          } else if(modeChar.contains(":")) {
            mode = QString("Q65-60") + modeChar.back();            
            if (m_xpol) {
              rpol = thePostLine.at(2).trimmed();
              if(thePostLine.length()==11) {
              if(thePostLine.at(10).contains("H")) txpol = "H";
              else if(thePostLine.at(10).contains("V")) txpol = "V";
              else txpol="--";
            } else txpol="--";
          }
        }
        // Handle CQ XXX CALL GRID
        }  else if(thePostLine.at(9).contains(".")) {
            if (isDir) continue;         // no four-word directed shape
            callsign = thePostLine.at(7).trimmed().toUpper();
            bool isCall = testCall(callsign);
            if(!isCall) continue;
            grid = thePostLine.at(8).trimmed();  
             
            dT =thePostLine.at(9).trimmed();
            modeChar = thePostLine.at(10).trimmed();
            if(modeChar.contains("#")) 
            {  
              mode = QString("JT65") + modeChar.back();            
              if (m_xpol) {
                rpol = thePostLine.at(2).trimmed();
                if(thePostLine.length()==12) {
                  if(thePostLine.at(11).contains("H")) txpol = "H";
                  else if(thePostLine.at(11).contains("V")) txpol = "V";
                  else txpol="--";
                } else txpol="--";                
              }
            } else if(modeChar.contains(":")) {
              mode = QString("Q65-60") + modeChar.back();            
              if (m_xpol) {
                rpol = thePostLine.at(2).trimmed();
                if(thePostLine.length()==12) {
                  if(thePostLine.at(11).contains("H")) txpol = "H";
                  else if(thePostLine.at(11).contains("V")) txpol = "V";
                  else txpol="--";
                } else txpol="--";  
              }                
            } 
        }
        else {
          continue; 
        }
        if(mode.contains("JT65") || mode.contains("Q65")) {
          // One-character flags: '0' + 6 bits (slot 1-6, disk 8, 73 16,
          // RR73 32) -- the QSO-sequence position inferred by the RX decoder
          // from the received message shape.
          int slot = 0; bool is73 = false, isRR73 = false;
          if (isDir) {
            classifyPayload(payload, &slot, &is73, &isRR73);
            if (slot == 1) { grid = payload.toUpper(); }
          } else {
            slot = 6;                    // CQ-family = Tx6 shape
          }
          const int flagsVal = slot | (m_diskMode ? 8 : 0) | (is73 ? 16 : 0) | (isRR73 ? 32 : 0);
          const QString flagsParam = "&flags=" + QString::fromLatin1(
              QUrl::toPercentEncoding(QString(QChar('0' + flagsVal))));
          QString postString =  "skedfreq=" + freq + "&rxfreq=" + dF + "&rpol=" + rpol + "&dt="  +  dT + "&dB="  + dB + "&msgtype="  +  msgType.toUpper() + (tocall.isEmpty() ? QString() : "&tocall=" + tocall) + "&callsign="  +  callsign.toUpper() + "&grid="  +  grid.toUpper() + "&mode="  +  mode + "&utcdatetime="  +  utcdatetimeUTCString + "&spotter="  +  m_myCall.toUpper() + "&spottergrid="  + m_myGrid.toUpper() + "&band=" + QString::number(int(getFcenter())) + "&txpol=" + txpol;
          // W3SZ and DL3WDG feed the w3sz site with disk replays as part of
          // developing it; that exception survives the uniform policy for
          // disk CQ-type rows only. Directed messages stay N6NU-only for
          // everyone (w3sz is live-CQ-only), and the pair's disk uploads to
          // N6NU are flagged and hidden like anyone else's.
          const QString myUp = m_myCall.trimmed().toUpper();
          const bool w3szDiskOp = (myUp == "W3SZ" || myUp == "DL3WDG");
          for (const QString &u : urls) {
            if ((isDir || m_diskMode) && !u.contains("livecq.n6nu.org")) {
              if (!(m_diskMode && !isDir && w3szDiskOp)) continue;
            }
            // w3sz.com gets the program name chosen in Settings: MAP65 with the
            // MAP65 User-Agent, exactly as before the re-branding, or EME65.
            const bool legacyW3sz = w3szLegacyId && u.contains("w3sz.com");
            // flags is N6NU-protocol only; other destinations get requests
            // byte-identical to the pre-flag client.
            QByteArray postByteArray = (postString
                + (u.contains("livecq.n6nu.org") ? flagsParam : QString())
                + "&apptype=" + (legacyW3sz ? "MAP65" : "EME65")).toUtf8();
            QNetworkRequest request{QUrl{u}};
            request.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
            request.setRawHeader("User-Agent", QString{(legacyW3sz ? "MAP65 " : "EME65 ")
                + QCoreApplication::applicationVersion()}.toUtf8());
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
            request.setTransferTimeout(15000);   // don't let a hung server queue requests
#endif
            m_livecqNAM->post(request, postByteArray);
          }
        }
      }
    }
  }
}

bool Messages::testCall(QString w)
{
// Check "callsign" to see if it could be a valid standard callsign or a valid
// compound callsign.
// Return a logical "call ok" indicator.
  if(w.indexOf('.') >= 0) return false;
  if(w.indexOf('+') >= 0) return false;
  if(w.indexOf('-') >= 0) return false;
  if(w.indexOf('?') >= 0) return false;
  w = w.replace('<',"");
  w = w.replace('>',""); 
  int n1=w.length();
  if(n1 > 11) return false;
  QString bc = QString();
  QStringList wSplit = w.split("/");
  if(wSplit.length() > 1) {
    if(wSplit.at(0).length() > wSplit.at(1).length()) {
      bc = wSplit.at(0);
    }
    else {
      bc = wSplit.at(1);
    }
  }
  else {
    bc = w;
  }
  int nbc=bc.trimmed().length();
  if(nbc > 8) return false;  //Base call should have no more than 8 characters  e.g. YW18FIFA

// One of first two characters must be a letter
  if((!bc[0].isLetter()) && (!bc[1].isLetter())) return false;
// Real calls don't start with Q, but we'll allow the placeholder
// callsign QU1RK to be considered a standard call:
  if(bc[0]=='Q' && bc.mid(0,5) != "QU1RK") return false;

// Must have a digit in 2nd or 3rd or 4th position
  int i1=0;
  if(bc[1].isDigit()) i1=1;
  if(bc[2].isDigit()) i1=2;
  if(bc[3].isDigit()) i1=3;
  if(i1==0) return false;

// Callsign must have a suffix of 1-4 letters e.g. YW18FIFA
  if(i1==nbc) return false;
  int n=0;
  QChar j=QChar();
  for (int i=i1+1; i<=nbc-1; ++i) {
     j=bc[i];
     if(j<QChar('A') || j > QChar('Z')) return false;
     n=n+1;
  }
  if(n >= 1 && n <= 4) return true;
  
  return false;  
}


void Messages::onFinished(QNetworkReply *reply)
{
    if (reply->error() == QNetworkReply::NoError) {
		qDebug() << "Reply in messages::inFinished is: " << reply->readAll();
    } else {
		qDebug() << "Error message in messages::inFinished is: " <<  reply->errorString();
    }
    reply->deleteLater();
}

void Messages::setText(QString t, QString t2)
{
  QString cfreq,cfreq0;
  m_t=t;
  m_t2=t2;

  QStringList cqliveText;  //liveCQ
  doLiveCQ = true;         //liveCQ

  QString s="QTextBrowser{background-color: "+m_colorBackground+"}";
  ui->messagesTextBrowser->setStyleSheet(s);

  ui->messagesTextBrowser->clear();
  QStringList lines = t.split( "\n", SkipEmptyParts );
  foreach( QString line, lines ) {
    QString t1=line.mid(0,81); //was 0,75
    int ncq=t1.indexOf(" CQ ");
    if((m_cqOnly or m_cqStarOnly) and  ncq< 0) continue;
    if(m_cqStarOnly) {
      QString caller=t1.mid(ncq+4,-1);
      int nz=caller.indexOf(" ");
      caller=caller.mid(0,nz);
      int i=t2.indexOf(caller);
      if(t2.mid(i-1,1)==" ") continue;
    }
    int n=line.mid(61,2).toInt();  //was 55,2
//    if(line.indexOf(":")>0) n=-1;
//    if(n==-1) ui->messagesTextBrowser->setTextColor("#ffffff");  // white
    ui->messagesTextBrowser->setFontWeight(m_boldText ? QFont::Bold : QFont::Normal);
    if(n==0) ui->messagesTextBrowser->setTextColor(m_color0);
    if(n==1) ui->messagesTextBrowser->setTextColor(m_color1);
    if(n==2) ui->messagesTextBrowser->setTextColor(m_color2);
    if(n>=3) ui->messagesTextBrowser->setTextColor(m_color3);
    QString livecqStr = t1.mid(0,59) + t1.mid(62,t1.length()-62) + " " + t1.mid(60,2); // was 53,56,56,54
    if(cqliveText.filter(livecqStr.mid(0,59)).length()==0) cqliveText.append(livecqStr); // was 0,53
    cfreq=t1.mid(5,3);
    if(cfreq == cfreq0) {
      t1="        " + t1.mid(8,-1);
    }
    cfreq0=cfreq;
    ui->messagesTextBrowser->append(t1.mid(5,67)); //was 5,61
  }
  if(doLiveCQ && cqliveText.size() > 0) {       //liveCQ
      sendLiveCQData(cqliveText);     //liveCQ
      doLiveCQ = false;               //liveCQ
    }                                 //liveCQ
  if (m_spot_to_psk_reporter && cqliveText.size() > 0) {
      sendPSKReporterData(cqliveText); //PSKReporter
  }
}

void Messages::sendPSKReporterData(QStringList decodeList) {  
    
  QSettings settings(m_settings_filename, QSettings::IniFormat);
  SettingsGroup g {&settings, "Common"};
  
  //QRZ parameters (3)
  QString receiverCallsign=settings.value("MyCall","").toString();
  QString receiverLocator=settings.value("MyGrid","").toString();
  m_spot_to_psk_reporter = settings.value("spotPSK",true).toBool();
  // the client keeps its UDP or TCP/IP connection, so a changed "Use TCP/IP
  // for PSK Reporter" starts a new one (the old one sends what it holds)
  if (settings.value("PSKReporterTCPIP",false).toBool() != pskReporterTcpip_) {
    createPSKReporter(!pskReporterTcpip_);
    pskReporter_->setLocalStation(receiverCallsign, receiverLocator, "N/A", "N/A (EME65)");
  }
  //QRZ parameters (8)
  QString senderCallsign;
  QString senderLocator;
  double doubleFreq = 0.0; //(Hz)
  qint64 frequency = 0.0; 
  int sNR = -10;
  QString mode = ""; 
  bool ok = false;
  
  for (const QString &theLine : decodeList) {
    QStringList thePostLine = theLine.split(" ",SkipEmptyParts);
    if((thePostLine.at(5) == "CQ" || thePostLine.at(5) == "QRZ" || thePostLine.at(5) == "CQV" ||  thePostLine.at(5) == "CQH" || thePostLine.at(5) == "QRT") && receiverCallsign.length() >=3 && receiverLocator.length()>=4) {
      if(allDecodes2.filter(theLine.mid(0,53)).length() == 0) {
        allDecodes2.append(theLine);
        QString freq = thePostLine.at(0).trimmed();
        doubleFreq = freq.toDouble(&ok);
        frequency = qRound64(doubleFreq * 1000000);
        QString dF = thePostLine.at(1).trimmed();
        QString dB = thePostLine.at(4).trimmed();
        sNR = dB.toInt(&ok);
        QString msgType = thePostLine.at(5).trimmed().toUpper();
        senderCallsign = "";
        senderLocator = "--";
        mode="";
        sNR=-10;
        QString modeChar = "";     

        int m_TRperiod = 60;
        QString sTimeString = (thePostLine.at(3).trimmed() + "00");
        int sTime = sTimeString.toInt();
        int h=sTimeString.mid(0,2).toInt();
        int m=sTimeString.mid(2,2).toInt();
        int s=sTimeString.mid(4,2).toInt();
        QTime time2(h, m, s);
        QDateTime qSpotTime;
        if (sTime + m_TRperiod < 236000) {
      #if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
          qSpotTime = QDateTime(
              QDateTime::currentDateTimeUtc().date(),
              time2,
              QTimeZone::UTC
          );
      #else
          qSpotTime = QDateTime(
              QDateTime::currentDateTimeUtc().date(),
              time2,
              Qt::UTC
          );
      #endif
        }
        else {
      #if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
          qSpotTime = QDateTime(
              QDateTime::currentDateTimeUtc().addDays(-1).date(),
              time2,
              QTimeZone::UTC
          );
      #else
          qSpotTime = QDateTime(
              QDateTime::currentDateTimeUtc().addDays(-1).date(),
              time2,
              Qt::UTC
          );
      #endif
    }            
        
        // Handle CQ CALL but NO GRID -- dot at 7
      if(thePostLine.at(7).contains(".")) {
          senderCallsign = thePostLine.at(6).trimmed().toUpper();
          bool isCall = testCall(senderCallsign);
          if(!isCall) continue;
          modeChar = thePostLine.at(8).trimmed(); 
          if(modeChar.contains("#")) mode = QString("JT65") + modeChar.back();
          else if(modeChar.contains(":")) mode = QString("Q65-60") + modeChar.back();   
          
        // Handle CQ CALL GRID or CQ XXX CALL -- dot at 8
        } else if (thePostLine.at(8).contains(".")) {
          // Test for callsign at thePostLine(6)
          senderCallsign = thePostLine.at(6).trimmed().toUpper();
          bool isCall = testCall(senderCallsign);
          if(isCall) {
            senderLocator = thePostLine.at(7).trimmed();  
            // Handle CQ XXX CALL
          } else {
            senderCallsign = thePostLine.at(7).trimmed().toUpper();
            bool isCall = testCall(senderCallsign);
            if(!isCall) continue;
          }          
          modeChar = thePostLine.at(9).trimmed();
          if(modeChar.contains("#")) 
          {  
            mode = QString("JT65") + modeChar.back();   
          } else if(modeChar.contains(":")) {
            mode = QString("Q65-60") + modeChar.back();      
          }
        // Handle CQ XXX CALL GRID
        }  else if(thePostLine.at(9).contains(".")) {
            senderCallsign = thePostLine.at(7).trimmed().toUpper();
            bool isCall = testCall(senderCallsign);
            if(!isCall) continue;
            senderLocator = thePostLine.at(8).trimmed();  
             
            modeChar = thePostLine.at(10).trimmed();
            if(modeChar.contains("#")) 
            {  
              mode = QString("JT65") + modeChar.back();      
            } else if(modeChar.contains(":")) {
              mode = QString("Q65-60") + modeChar.back();     
            } 
        }
        else {
          continue; 
        }             
                
        if (pskReporter_) pskReporter_->addRemoteStation(senderCallsign, senderLocator, frequency, mode, sNR, qSpotTime);
      }
    }
  }
}

// Click anywhere on the line -- no more per-word picking. Whichever line
// the cursor lands on (regardless of x-position) is resolved as a whole
// in MainWindow::doubleClickOnMessages via parseMessageLine().
void Messages::selectCallsign2(bool ctrl, bool isDoubleClick)
{
  QString t = ui->messagesTextBrowser->toPlainText();
  int i = ui->messagesTextBrowser->textCursor().position();
  // Search strictly before i, not at-or-before: a double-click's default
  // word-selection can leave the cursor exactly on a line-separator
  // character (see the identical BandMap fix), which would otherwise
  // produce a start-past-end range and glob the rest of the buffer.
  int i0 = (i > 0) ? (t.lastIndexOf("\n", i - 1) + 1) : 0;
  int i1 = t.indexOf("\n", i);
  if (i1 < 0) i1 = t.length();
  QString t2 = t.mid(i0, i1 - i0);
  if (t2.trimmed().isEmpty() || t2.trimmed().startsWith(":")) return;  // no callsign on this line
  emit click2OnCallsign(t2, ctrl, isDoubleClick);
}

void Messages::setColors(QString t, bool bold)
{
  m_colorBackground = "#"+t.mid(0,6);
  m_color0 = "#"+t.mid(6,6);
  m_color1 = "#"+t.mid(12,6);
  m_color2 = "#"+t.mid(18,6);
  m_color3 = "#"+t.mid(24,6);
  m_boldText = bold;
  setText(m_t,m_t2);
}

void Messages::on_cbCQ_toggled(bool checked)
{
  m_cqOnly = checked;
  setText(m_t,m_t2);
}

void Messages::on_cbCQstar_toggled(bool checked)
{
  m_cqStarOnly = checked;
  setText(m_t,m_t2);
}
