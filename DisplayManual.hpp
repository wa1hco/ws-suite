#ifndef DISPLAY_MANUAL_HPP__
#define DISPLAY_MANUAL_HPP__

#include <QObject>

#include "pimpl_h.hpp"

class QNetworkAccessManager;
class QDir;
class QUrl;
class QString;

class DisplayManual
  : public QObject
{
public:
  DisplayManual (QNetworkAccessManager *, QObject * = nullptr);
  ~DisplayManual ();
  // open <name_we>_<lang>.html, falling back to <name_we>_<language>.html
  // (lang without the country) and then to <name_we>_en.html
  void display_html_url (QUrl const& url, QString const& name_we, QString const& lang);
  void display_html_file (QDir const& dir, QString const& name_we, QString const& lang);

private:
  class impl;
  pimpl<impl> m_;
};

#endif
