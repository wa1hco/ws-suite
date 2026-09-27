#include "Cloudlog.hpp"

#include <future>
#include <chrono>

#include <QHash>
#include <QString>
#include <QDate>
#include <QFile>
#include <QTextStream>
#include <QDir>
#include <QFileInfo>
#include <QPointer>
#include <QSaveFile>
#include <QUrl>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QDebug>
#include <QMessageBox>
#include <QIcon>
#include <QJsonDocument>
#include <QJsonObject>

#include "pimpl_impl.hpp"

#include "moc_Cloudlog.cpp"

#include "Configuration.hpp"
#include "revision_utils.hpp"

namespace
{
  // Dictionary mapping call sign to date of last upload to LotW
  using dictionary = QHash<QString, QDate>;
}

class Cloudlog::impl final
  : public QObject
{
  Q_OBJECT

public:
  impl (Cloudlog * self, Configuration const * config, QNetworkAccessManager * network_manager)
    : self_ {self}
    , config_ {config}
    , network_manager_ {network_manager}
  {
  }

  void logQso (QByteArray ADIF)
  {
    QByteArray data;

    QString str = QString("") +
      "{" +
        "\"key\":\""+ config_->cloudlog_api_key() +"\"," +
        "\"station_profile_id\":\""+ QVariant(config_->cloudlog_api_station_id()).toString() +"\"," +
        "\"type\":\"adif\"," +
        "\"string\":\"" + ADIF + "<eor>" + "\"" +
      "}";
    data = str.toUtf8();

    QUrl u = QUrl(config_->cloudlog_api_url()+qsoPath_);

    QNetworkRequest request(u);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QVariant("application/json"));
    request.setRawHeader ("User-Agent", http_user_agent ().toUtf8 ());
    reply_ = network_manager_->post(request, data);
    connect (reply_.data (), &QNetworkReply::finished, this, &Cloudlog::impl::reply_logqso);

  }

  void postTestRequest (QString const& endpoint)
  {
    QNetworkRequest request {QUrl(endpoint)};
    request.setHeader (QNetworkRequest::ContentTypeHeader, QVariant("application/json"));
    // request.setRawHeader ("User-Agent", "WSJT-X Cloudlog API");
    request.setRawHeader ("User-Agent", http_user_agent ().toUtf8 ());
    request.setOriginatingObject (this);
    reply_ = network_manager_->post (request, testBody_);
    connect (reply_.data (), &QNetworkReply::finished, this, &Cloudlog::impl::reply_apitest);
  }

  void testApi (QString const& url, QString const& apiKey, int stationId)
  {
    QString apiUrl = url;
#if QT_VERSION < QT_VERSION_CHECK(5, 15, 0)
    if (QNetworkAccessManager::Accessible != network_manager_->networkAccessible ())
      {
        // try and recover network access for QNAM
        network_manager_->setNetworkAccessible (QNetworkAccessManager::Accessible);
      }
#endif

    // Remove trailing slash if given
    if (apiUrl.endsWith('/')) {
      apiUrl.chop(1);
    }
    // Remove full path to API if given
    if (apiUrl.endsWith("/index.php/api/qso")) {
      apiUrl.chop(18);
    }

    // Dry-run: POST a demo ADIF to /api/qso/true (Wavelog validates but does not save)
    static const char demoADIF[] =
      "<call:5>DJ7NT <gridsquare:4>JO30 <mode:3>FT8 "
      "<rst_sent:3>-15 <rst_rcvd:2>33 "
      "<qso_date:8>20240110 <time_on:6>051855 "
      "<qso_date_off:8>20240110 <time_off:6>051855 "
      "<band:3>40m <freq:8>7.155783 "
      "<station_callsign:5>TE1ST <my_gridsquare:6>JO30OO <eor>";

    testBaseUrl_ = apiUrl;
    testRetried_ = false;
    testBody_ = QString(
      "{\"key\":\"%1\","
      "\"station_profile_id\":\"%2\","
      "\"type\":\"adif\","
      "\"string\":\"%3\"}"
    ).arg(apiKey, QString::number(stationId), demoADIF).toUtf8();

    postTestRequest (testBaseUrl_ + "/api/qso/true");
  }

  void reply_apitest()
  {
    if (reply_ && reply_->isFinished ())
      {
        int httpStatus = reply_->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (httpStatus == 201)
          {
            // Remember which path worked for real QSO uploads
            qsoPath_ = testRetried_ ? "/index.php/api/qso" : "/api/qso";
            Q_EMIT self_->apikey_ok ();
          }
        else if (!testRetried_)
          {
            // First attempt failed — retry with /index.php/ prefix (Cloudlog-style URL)
            testRetried_ = true;
            postTestRequest (testBaseUrl_ + "/index.php/api/qso/true");
          }
        else
          {
            Q_EMIT self_->apikey_invalid ();
          }
      }
  }

  void reply_logqso()
  {
    QString result;
    if (reply_ && reply_->isFinished ())
      {
        result = reply_->readAll();
        QJsonDocument data = QJsonDocument::fromJson(result.toUtf8());
        QJsonObject obj = data.object();
        if (obj["status"] == "failed") {
          QMessageBox msgBox;
          msgBox.setIcon(QMessageBox::Warning);
	  msgBox.setWindowTitle("Cloudlog Error!");
          msgBox.setText("QSO could not be sent to Cloudlog!\nPlease check your log.");
          msgBox.setDetailedText("Reason: "+obj["reason"].toString());
          msgBox.exec();
        }
      }
  }

  void abort ()
  {
    if (reply_ && reply_->isRunning ())
      {
        reply_->abort ();
      }
  }

  Cloudlog * self_;
  Configuration const * config_;
  QNetworkAccessManager * network_manager_;
  QPointer<QNetworkReply> reply_;
  QByteArray testBody_;
  QString    testBaseUrl_;
  bool       testRetried_ {false};
  QString    qsoPath_ {"/index.php/api/qso"};  // updated by successful test
};

#include "Cloudlog.moc"

Cloudlog::Cloudlog (Configuration const * config, QNetworkAccessManager * network_manager, QObject * parent)
  : QObject {parent}
  , m_ {this, config, network_manager}
{
}

Cloudlog::~Cloudlog ()
{
}

void Cloudlog::testApi (QString const& url, QString const& apiKey, int stationId)
{
  m_->testApi (url, apiKey, stationId);
}

void Cloudlog::logQso (QByteArray ADIF)
{
  m_->logQso(ADIF);
}
