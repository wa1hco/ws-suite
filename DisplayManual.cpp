#include "DisplayManual.hpp"

#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QUrl>
#include <QString>
#include <QDir>
#include <QFileInfo>
#include <QDesktopServices>
#include <QLocale>

#include "revision_utils.hpp"

#include "pimpl_impl.hpp"

namespace 
{
  class token
    : public QObject
  {
    Q_OBJECT
  public:
    token (QUrl const& url, QString const& lang, QString const& name_we, QObject * parent = nullptr)
      : QObject {parent}
      , url_ {url}
      , lang_ {lang}
      , name_we_ {name_we}
    {
    }

    QUrl url_;
    QString lang_;
    QString name_we_;
  };
}
      
class DisplayManual::impl final
  : public QObject
{
  Q_OBJECT
public:
  impl (QNetworkAccessManager * qnam)
    : qnam_ {qnam}
  {
    connect (qnam_, &QNetworkAccessManager::finished, this, &DisplayManual::impl::reply_finished);
  }

  void display (QUrl const& url, QString const& name_we, QString const& lang)
  {
#if QT_VERSION < QT_VERSION_CHECK(5, 15, 0)
    if (QNetworkAccessManager::Accessible != qnam_->networkAccessible ()) {
      // try and recover network access for QNAM
      qnam_->setNetworkAccessible (QNetworkAccessManager::Accessible);
    }
#endif

    // try and find a localized manual, language and country first
    auto file = name_we + '_' + lang + ".html";
    auto target = url.resolved (file);
    QNetworkRequest request {target};
    request.setRawHeader ("User-Agent", "WS Manual Checker");
    request.setOriginatingObject (new token {url, lang, name_we, this});
    auto * reply = qnam_->head (request);
    outstanding_requests_ << reply;
  }

  void reply_finished (QNetworkReply * reply)
  {
    if (outstanding_requests_.contains (reply))
      {
        QUrl target;
        if (reply->error ())
          {
            if (auto * tok = qobject_cast<token *> (reply->request ().originatingObject ()))
              {
                auto pos = tok->lang_.lastIndexOf ('_');
                QString file;
                if (pos >= 0 || (tok->lang_.size () && tok->lang_ != "en"))
                  {
                    // drop the country, then the language
                    tok->lang_.truncate (pos >= 0 ? pos : 0);
                    file = tok->name_we_ + '_' + (tok->lang_.size () ? tok->lang_ : QString {"en"}) + ".html";
                    target = tok->url_.resolved (file);
                    QNetworkRequest request {target};
                    request.setRawHeader ("User-Agent", "WS Manual Checker");
                    request.setOriginatingObject (tok);
                    auto * reply = qnam_->head (request);
                    outstanding_requests_ << reply;
                  }
                else
                  {
                    // give up looking and request the English one
                    file = tok->name_we_ + "_en.html";
                    target = tok->url_.resolved (file);
                    QDesktopServices::openUrl (target);
                    delete tok;
                  }
              }
          }
        else
          {
            // found it
            if (auto * tok = qobject_cast<token *> (reply->request ().originatingObject ()))
              {
                delete tok;
              }
            QDesktopServices::openUrl (reply->request ().url ());
          }

        outstanding_requests_.removeOne (reply);
        reply->deleteLater ();
      }
  }

  QNetworkAccessManager * qnam_;
  QList<QNetworkReply *> outstanding_requests_;
};

#include "DisplayManual.moc"

DisplayManual::DisplayManual (QNetworkAccessManager * qnam, QObject * parent)
  : QObject {parent}
  , m_ {qnam}
{
}

DisplayManual::~DisplayManual ()
{
}

void DisplayManual::display_html_url (QUrl const& url, QString const& name_we, QString const& lang)
{
  m_->display (url, name_we, lang);
}

void DisplayManual::display_html_file (QDir const& dir, QString const& name_we, QString const& lang)
{
  // try and find a localized manual, language and country first
  auto file = dir.absoluteFilePath (name_we + '_' + lang + ".html");
  if (!QFileInfo::exists (file))
    {
      // try for language
      auto language = lang;
      language.truncate (language.lastIndexOf ('_'));
      file = dir.absoluteFilePath (name_we + '_' + language + ".html");
      if (language.isEmpty () || !QFileInfo::exists (file))
        {
          // use the English one
          file = dir.absoluteFilePath (name_we + "_en.html");
        }
    }
  // may fail but browser 404 error is a good as anything
  QDesktopServices::openUrl (QUrl {"file:///" + file});
}

