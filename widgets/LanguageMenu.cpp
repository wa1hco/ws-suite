#include "LanguageMenu.hpp"

#include <QAction>
#include <QActionGroup>
#include <QDir>
#include <QLocale>
#include <QMap>
#include <QPair>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>

#include "MultiSettings.hpp"

QString const LanguageMenu::settings_key {"UILanguage"};

namespace
{
  // "Deutsch (German)", "English". Qt's native names carry the region
  // ("American English", "español de España"), so the shipped languages
  // have their own names; anything else falls back to Qt's.
  QString language_label (QString const& code)
  {
    static QMap<QString, QPair<QString, QString>> const names {
      {"en", {"English", "English"}},
      {"ca", {QString::fromUtf8 ("Català"), "Catalan"}},
      {"da", {"Dansk", "Danish"}},
      {"de", {"Deutsch", "German"}},
      {"es", {QString::fromUtf8 ("Español"), "Spanish"}},
      {"fr", {QString::fromUtf8 ("Français"), "French"}},
      {"hu", {"Magyar", "Hungarian"}},
      {"it", {"Italiano", "Italian"}},
      {"ja", {QString::fromUtf8 ("日本語"), "Japanese"}},
      {"ru", {QString::fromUtf8 ("Русский"), "Russian"}},
      {"zh", {QString::fromUtf8 ("简体中文"), "Chinese, Simplified"}},
      {"zh_TW", {QString::fromUtf8 ("繁體中文"), "Chinese, Traditional"}},
    };
    QString native, english;
    if (names.contains (code))
      {
        native = names[code].first;
        english = names[code].second;
      }
    else
      {
        QLocale const locale {code};
        native = locale.nativeLanguageName ();
        english = QLocale::languageToString (locale.language ());
        if (native.size ()) native[0] = native[0].toUpper ();
      }
    return native.isEmpty () || native == english ? english : QString {"%1 (%2)"}.arg (native, english);
  }
}

LanguageMenu::LanguageMenu (MultiSettings * multi_settings, std::function<bool ()> can_restart, QWidget * parent)
  : QMenu {tr ("&Language"), parent}
{
  setObjectName ("menuLanguage");
  QStringList codes {"en"};
  for (auto const& file : QDir {":/Translations"}.entryList ({"ws_*.qm"}, QDir::Files, QDir::Name))
    {
      auto const code = file.mid (3, file.size () - 6); // ws_de.qm -> de
      if (!code.startsWith ("en")) codes << code;        // en and en_GB carry no translations
    }

  auto const current = multi_settings->common_value (settings_key, "en").toString ();
  auto * group = new QActionGroup {this};
  for (auto const& code : codes)
    {
      auto * action = addAction (language_label (code));
      action->setCheckable (true);
      action->setChecked (code == current);
      group->addAction (action);
      connect (action, &QAction::triggered, [this, multi_settings, can_restart, code] {
          if (code == multi_settings->common_value (settings_key, "en").toString ()) return;
          multi_settings->set_common_value (settings_key, code);
          multi_settings->settings ()->sync ();

          QMessageBox box {QMessageBox::Question, tr ("Language")
              , tr ("Restart the WS window now to show it in %1?\n\n"
                    "Otherwise the new language is used the next time WS starts.").arg (language_label (code))
              , QMessageBox::NoButton, parentWidget ()};
          auto * now = box.addButton (tr ("Restart now"), QMessageBox::AcceptRole);
          box.addButton (tr ("Later"), QMessageBox::RejectRole);
          box.setDefaultButton (now);
          box.exec ();
          if (box.clickedButton () != now) return;
          if (can_restart ())
            {
              multi_settings->restart ();
            }
          else
            {
              QMessageBox::information (parentWidget (), tr ("Language")
                                        , tr ("WS is transmitting or tuning. The new language is used the next time WS starts."));
            }
        });
    }
}
