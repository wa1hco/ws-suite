// Render the illustrations for the WS User Guide from the real Designer
// forms: every Settings tab (full size) and every main-window menu with its
// submenus, in the stock look. The menus are the same in the standard, AL
// and widescreen main windows, so the standard form is used.
//
// usage: ws-guide-shots <source dir> <image dir> [file.qm]
//   with a .qm file only the Settings tabs are drawn, translated.
//
// build (MSYS2 mingw64):
//   g++ -std=c++17 -O1 ws-guide-shots.cpp -o ws-guide-shots $(pkg-config --cflags --libs Qt5UiTools Qt5Widgets)
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFont>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QScrollArea>
#include <QTabBar>
#include <QTabWidget>
#include <QTranslator>
#include <QUiLoader>

static QWidget * load (QString const& path)
{
  QUiLoader loader;
  QFile f {path};
  if (!f.open (QFile::ReadOnly)) { qWarning () << "cannot open" << path; return nullptr; }
  QWidget * w = loader.load (&f);
  if (!w) qWarning () << "load failed" << path << loader.errorString ();
  return w;
}

static void settle (QWidget * w)
{
  w->setAttribute (Qt::WA_DontShowOnScreen);
  w->show ();
  for (int i = 0; i < 5; ++i) QApplication::processEvents ();
}

static void render_settings (QString const& source, QString const& outdir)
{
  QWidget * top = load (source + "/Configuration.ui");
  if (!top) return;
  if (auto * e = top->findChild<QLineEdit *> ("callsign_line_edit")) e->setText ("N6NU");
  if (auto * e = top->findChild<QLineEdit *> ("grid_line_edit")) e->setText ("CM87");
  auto * tabs = top->findChild<QTabWidget *> ();
  if (!tabs) return;
  // the tabs live in a scroll area; draw them at full size instead of the clipped dialog
  auto * scroll = top->findChild<QScrollArea *> ("scrollArea");
  if (scroll) scroll->setWidgetResizable (false);
  settle (top);
  top->resize (1200, 900);
  for (int i = 0; i < tabs->count (); ++i) {
    tabs->setCurrentIndex (i);
    auto * page = tabs->widget (i);
    QSize const bar = tabs->tabBar ()->sizeHint ();
    QSize const want {qMax (page->sizeHint ().width () + 8, bar.width ()), page->sizeHint ().height () + bar.height () + 8};
    tabs->setMinimumSize (want);
    tabs->setMaximumSize (want);
    if (scroll) scroll->widget ()->resize (scroll->widget ()->sizeHint ());
    for (int k = 0; k < 5; ++k) QApplication::processEvents ();
    QString const out = outdir + "/settings-" + page->objectName ().remove ("_tab") + ".png";
    tabs->grab ().save (out);
    qInfo ().noquote () << "  wrote" << out;
  }
}

// a menu drawn on its own, as it looks when dropped down
static void render_menu (QMenu * menu, QString const& out)
{
  menu->setAttribute (Qt::WA_DontShowOnScreen);
  menu->popup (QPoint {0, 0});
  for (int i = 0; i < 5; ++i) QApplication::processEvents ();
  menu->grab ().save (out);
  qInfo ().noquote () << "  wrote" << out;
  menu->hide ();
  for (auto * a : menu->actions ())
    if (a->menu ())
      render_menu (a->menu (), out.chopped (4) + "-" + a->menu ()->objectName ().remove ("menu").remove (QRegExp {"^_"}) + ".png");
}

static void render_menus (QString const& source, QString const& outdir)
{
  QWidget * top = load (source + "/widgets/mainwindow.ui");
  if (!top) return;
  settle (top);
  auto * bar = top->findChild<QMenuBar *> ();
  if (!bar) { qWarning () << "no menu bar"; return; }
  // the Language menu is added in code (widgets/LanguageMenu.cpp); keep these names in step with it
  if (auto * help = top->findChild<QMenu *> ("menuHelp")) {
    auto * language = new QMenu {"Language", bar};
    language->setObjectName ("menuLanguage");
    auto * group = new QActionGroup {language};
    for (auto const * name : {"English", "Català (Catalan)", "Dansk (Danish)", "Deutsch (German)", "Español (Spanish)", "Français (French)"
          , "Magyar (Hungarian)", "Italiano (Italian)", "日本語 (Japanese)", "Русский (Russian)"
          , "简体中文 (Chinese, Simplified)", "繁體中文 (Chinese, Traditional)"}) {
      auto * action = language->addAction (QString::fromUtf8 (name));
      action->setCheckable (true);
      action->setChecked (group->actions ().isEmpty ());
      group->addAction (action);
    }
    bar->insertMenu (help->menuAction (), language);
    settle (top);
  }
  bar->grab ().save (outdir + "/menubar.png");
  // Help > WS Download Page is added in code too (widgets/mainwindow.cpp)
  if (auto * help = top->findChild<QMenu *> ("menuHelp"))
    if (auto * home = top->findChild<QAction *> ("actionWS_Home_Page")) {
      auto const& actions = help->actions ();
      auto const next = actions.indexOf (home) + 1;
      help->insertAction (next > 0 && next < actions.size () ? actions.at (next) : nullptr, new QAction {"WS Download Page", help});
    }
  // the Configurations menu is built at run time (MultiSettings.cpp); show two sample configurations
  if (auto * config = top->findChild<QMenu *> ("menuConfig")) {
    auto * group = new QActionGroup {config};
    auto add = [&] (QString const& title, char const * name, bool current) {
      auto * sub = config->addMenu (title);
      sub->setObjectName (name);
      sub->menuAction ()->setCheckable (true);
      sub->menuAction ()->setChecked (current);
      group->addAction (sub->menuAction ());
      if (!current) {
        sub->addAction ("&Switch To");
        sub->addSeparator ();
      }
      sub->addAction ("&Clone");
      sub->addAction ("Clone &Into ...");
      sub->addAction ("R&eset");
      sub->addAction ("&Rename ...");
      if (!current) sub->addAction ("&Delete");
    };
    add ("Default", "menuCurrent", true);
    add ("EME 10 GHz", "menuOther", false);
  }
  for (auto * a : bar->actions ())
    if (a->menu ())
      render_menu (a->menu (), outdir + "/menu-" + a->menu ()->objectName ().remove ("menu") + ".png");
}

int main (int argc, char * argv[])
{
  if (argc < 3) { qWarning () << "usage: ws-guide-shots <source dir> <image dir> [file.qm]"; return 2; }
  QApplication app {argc, argv};
  app.setFont (QFont {"MS Shell Dlg 2", 8});
  QString const source {argv[1]};
  QString const outdir {argv[2]};
  QDir {}.mkpath (outdir);
  if (argc > 3) {
    static QTranslator tr;
    if (!tr.load (argv[3])) { qWarning () << "cannot load" << argv[3]; return 1; }
    app.installTranslator (&tr);
    render_settings (source, outdir);
    return 0;
  }
  render_settings (source, outdir);
  render_menus (source, outdir);
  return 0;
}
