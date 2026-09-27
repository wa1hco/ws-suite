#ifndef LANGUAGE_MENU_HPP__
#define LANGUAGE_MENU_HPP__

#include <functional>

#include <QCoreApplication>
#include <QMenu>

class MultiSettings;

//
// LanguageMenu - pick the user interface language
//
//  Lists English plus every translation built into the resources
//  (:/Translations/ws_<lang>.qm), each in its own language. The choice
//  is stored for all configurations and applied by recreating the main
//  window, the same way a configuration switch does. The --language
//  command line option still takes precedence.
//
class LanguageMenu final
  : public QMenu
{
  Q_DECLARE_TR_FUNCTIONS (LanguageMenu)

public:
  // can_restart returns false while the main window must not be
  // recreated (transmitting or tuning)
  LanguageMenu (MultiSettings *, std::function<bool ()> can_restart, QWidget * parent);

  static QString const settings_key;
};

#endif
