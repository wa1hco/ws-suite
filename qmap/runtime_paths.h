#ifndef QMAP_RUNTIME_PATHS_H
#define QMAP_RUNTIME_PATHS_H

#include <QString>

QString qmapDataDir();
// iniBaseName is the current name, legacyBaseName the pre-rebranding one that
// migration looks for. Both take the multi-instance flavour for named rigs.
QString qmapSettingsFile(QString const& appDir, QString const& dataDir,
                         QString const& iniBaseName = QStringLiteral("wsmap.ini"),
                         QString const& legacyBaseName = QStringLiteral("qmap.ini"));
int     qmapInstanceId(QString const& settingsFile);
QString qmapInstanceDir(QString const& dataDir, int instanceId);

#endif // QMAP_RUNTIME_PATHS_H
