#include "runtime_paths.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>

#include "RebrandingMigration.hpp"

namespace
{
  // Legacy install location of the old JTSDK-era WSJT-X bundle -- probed as a
  // second source when migrating a user's qmap.ini into the data dir. Same
  // probe the MAP65 port uses (map65/mainwindow.cpp).
  const QString legacyJtsdkDir {"C:/WSJT/wsjtx/bin"};

  // true when path lies at or beneath dir (normalized; case-insensitive
  // because these are Windows paths).
  bool underDir(QString const& path, QString const& dir)
  {
    if (path.isEmpty() || dir.isEmpty()) return false;
    const QString p = QDir::cleanPath(QDir {path}.absolutePath());
    const QString d = QDir::cleanPath(QDir {dir}.absolutePath());
    return 0 == p.compare(d, Qt::CaseInsensitive)
        || p.startsWith(d + '/', Qt::CaseInsensitive);
  }

  // One-time cleanup of a freshly-migrated ini: SaveDir/AzElDir carried over
  // from a legacy install keep pointing work files at the old (possibly
  // unwritable) bin tree even though everything else moved to the AppData
  // data dir. Drop them when they point inside a legacy install location --
  // readSettings' defaults then supply the new per-instance paths. Values
  // elsewhere are deliberate user locations and are kept. This runs only on
  // the migration copy, never again, so any path the user sets afterwards
  // (old-style or not) sticks.
  void dropLegacyDirKeys(QString const& settingsFile, QString const& legacyDir)
  {
    QSettings s {settingsFile, QSettings::IniFormat};
    for (auto const& key : {QStringLiteral("Common/SaveDir"),
                            QStringLiteral("Common/AzElDir")}) {
      const QString v = s.value(key).toString();
      if (underDir(v, legacyDir) || underDir(v, legacyJtsdkDir)) {
        qWarning() << "Migrated ini: dropping legacy" << key << "=" << v
                   << "(new data-dir default applies)";
        s.remove(key);
      }
    }
    s.sync();
  }
}

// The writable per-user data directory (Windows: AppData/Local/QMAP). Its
// root holds ONLY the ini files (wsmap.ini / wsmap-<rig>.ini); everything an
// instance writes lives under its instance-<ID>/ subtree, so a read-only
// install location (Program Files) works and concurrent instances cannot
// clobber each other's work files. Same scheme as upstream WSJT-X 3.x QMAP
// except for the per-instance split -- see QMAP-INI-DATADIR-PLAN.md.
QString qmapDataDir()
{
  QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
  if (dataDir.isEmpty()) {
    dataDir = QDir::home().absoluteFilePath(".qmap");
  }

  if (!QDir{}.mkpath(dataDir)) {
    qWarning() << "Unable to create QMAP data directory:" << dataDir;
  }
  return dataDir;
}

// The ini lives in the data dir, and a legacy one is carried across once.
// iniBaseName selects the multi-instance flavour ("wsmap.ini" or
// "wsmap-<rig>.ini"); legacyBaseName is the same file's pre-rebranding name
// ("qmap.ini" / "qmap-<rig>.ini"), so named rigs migrate like the default.
//
// Sources are probed newest first, and the first one found is used:
//   1. the old QMAP data dir, %LocalAppData%\QMAP -- the rebranding migration,
//      and the only one most users will ever hit;
//   2. beside the executable, and
//   3. the old JTSDK install dir -- both pre-date the move into AppData, kept
//      so a very old install still upgrades in a single step.
//
// The rules -- copy never move, a marker so it runs exactly once, a
// pre-release wsmap.ini set aside rather than kept -- are in
// RebrandingMigration.hpp, shared with WS and EME65.
QString qmapSettingsFile(QString const& appDir, QString const& dataDir,
                         QString const& iniBaseName,
                         QString const& legacyBaseName)
{
  QString settingsFile = QDir {dataDir}.absoluteFilePath(iniBaseName);
  if (rebranding_migration::done(settingsFile)) return settingsFile;

  // %LocalAppData%\QMAP is the sibling of our own %LocalAppData%\WSMAP.
  const QString oldDataDir = QDir {QFileInfo {dataDir}.absolutePath()}
                               .absoluteFilePath(QStringLiteral("QMAP"));

  QString legacySettingsFile;
  QString legacyDir;
  for (auto const& dir : {oldDataDir, appDir, legacyJtsdkDir}) {
    const QString candidate = QDir {dir}.absoluteFilePath(legacyBaseName);
    if (QFile::exists(candidate)) {
      legacySettingsFile = candidate;
      legacyDir = dir;
      break;
    }
  }

  // Runs even with nothing to copy, so the marker is written and a QMAP
  // installed later can never overwrite settings made in WS-MAP.
  const QList<bool> copied =
      rebranding_migration::run(settingsFile, {{legacySettingsFile, settingsFile}});
  if (copied.value(0)) {
    // Stored SaveDir/AzElDir still point into the old tree and would beat
    // the new defaults; drop them so the new per-instance paths apply.
    dropLegacyDirKeys(settingsFile, legacyDir);
  }
  return settingsFile;
}

// The instance ID (1-4) selects both the mem_qmap[_N] shared segment and the
// instance-<ID>/ work-file subtree. Same peek the MainWindow constructor uses
// for the segment key, so the two can never disagree.
int qmapInstanceId(QString const& settingsFile)
{
  QSettings peek {settingsFile, QSettings::IniFormat};
  int id = peek.value("Common/InstanceId", 1).toInt();
  if (id < 1 || id > 4) id = 1;
  return id;
}

// Per-instance work-file dir, used as the process cwd: the decoder's relative
// opens (timer.out / all_qmap.txt / red.dat in ftninit.f90) plus fadd.txt and
// wsjt.log land here, and the SaveDir / MRUdir / AzElDir defaults point here.
// Per-instance because none of these files carry a band/config/instance
// component: red.dat is the Fortran->plotter refresh channel, .qm recordings
// are named by bare UTC timestamp, and azel.dat's Doppler line scales with
// the instance's own band. Created (with its save/ subdir) on first use.
QString qmapInstanceDir(QString const& dataDir, int instanceId)
{
  QDir dir {dataDir};
  const QString sub = QString("instance-%1").arg(instanceId);
  if (!dir.mkpath(sub + "/save")) {
    qWarning() << "Unable to create QMAP instance directory:" << dir.absoluteFilePath(sub);
  }
  return dir.absoluteFilePath(sub);
}
