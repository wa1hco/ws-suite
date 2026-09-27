#include "USStates.hpp"

#include <QtConcurrent/QtConcurrentRun>
#include <QFile>
#include <QFileInfo>
#include <QDataStream>
#include <QTextStream>
#include <QStandardPaths>
#include <QDir>
#include <QMutex>
#include <QMutexLocker>
#include <algorithm>
#include <QDebug>
#include <QDateTime>

// =======================
// State encoding
// =======================

quint8 USStates::encodeState(QString const& s)
{
    static const QHash<QString, quint8> map = {
      {"AL", 0}, {"AK", 1}, {"AZ", 2}, {"AR", 3}, {"CA", 4}, {"CO", 5},
      {"CT", 6}, {"DE", 7}, {"FL", 8}, {"GA", 9}, {"HI", 10}, {"ID", 11},
      {"IL", 12}, {"IN", 13}, {"IA", 14}, {"KS", 15}, {"KY", 16}, {"LA", 17},
      {"ME", 18}, {"MD", 19}, {"MA", 20}, {"MI", 21}, {"MN", 22}, {"MS", 23},
      {"MO", 24}, {"MT", 25}, {"NE", 26}, {"NV", 27}, {"NH", 28}, {"NJ", 29},
      {"NM", 30}, {"NY", 31}, {"NC", 32}, {"ND", 33}, {"OH", 34}, {"OK", 35},
      {"OR", 36}, {"PA", 37}, {"RI", 38}, {"SC", 39}, {"SD", 40}, {"TN", 41},
      {"TX", 42}, {"UT", 43}, {"VT", 44}, {"VA", 45}, {"WA", 46}, {"WV", 47},
      {"WI", 48}, {"WY", 49}, {"DC", 50}, {"PR", 51}, {"GU", 52}, {"VI", 53},
      {"AS", 54}, {"MP", 55}
    };

    return map.value(s.trimmed().toUpper(), 255);
}

// =======================
// Paths
// =======================

QString USStates::binaryPath()
{
    // Same directory as wsjtx.ini
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    QDir().mkpath(dir);
    return dir + "/callsign_states.bin";
}

// tsvPath() is assumed declared in USStates.hpp and defined elsewhere
// or you can add it here if needed.

// =======================
// Unsigned lexicographic compare (safe)
// =======================

int USStates::cmpUnsigned(QByteArray const& a, QByteArray const& b)
{
    const int len = std::min(a.size(), b.size());
    for (int i = 0; i < len; ++i) {
        const unsigned char ac = static_cast<unsigned char>(a[i]);
        const unsigned char bc = static_cast<unsigned char>(b[i]);
        if (ac < bc) return -1;
        if (ac > bc) return 1;
    }
    if (a.size() < b.size()) return -1;
    if (a.size() > b.size()) return 1;
    return 0;
}

// =======================
// BIN generation from TSV
// =======================

bool USStates::generateBinaryFromTSV(QString const& tsvPath,
                                     QString const& binPath)
{
    QFile inFile(tsvPath);
    if (!inFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qInfo() << "[USStates] Cannot open TSV:" << tsvPath;
        return false;
    }

    QFile outFile(binPath);
    if (!outFile.open(QIODevice::WriteOnly)) {
        qInfo() << "[USStates] Cannot open BIN for write:" << binPath;
        return false;
    }

    QTextStream in(&inFile);
    in.setCodec("UTF-8");

    QVector<QPair<QByteArray, quint8>> entries;
    entries.reserve(1600000);

    while (!in.atEnd()) {
        QString line = in.readLine();
        int tab = line.indexOf('\t');
        if (tab > 0) {
            QByteArray key = line.left(tab).toUtf8();
            quint8 state = encodeState(line.mid(tab + 1));
            entries.append({key, state});
        }
    }

    std::sort(entries.begin(), entries.end(),
              [](QPair<QByteArray, quint8> const& a,
                 QPair<QByteArray, quint8> const& b)
              {
                  return a.first < b.first;
              });

    QDataStream out(&outFile);
    out.setByteOrder(QDataStream::LittleEndian);

    out << quint32(entries.size());

    for (auto const& e : entries) {
        out << quint16(e.first.size());
        out.writeRawData(e.first.data(), e.first.size());
        out << e.second;
    }

    if (out.status() != QDataStream::Ok) {
        qInfo() << "[USStates] Error writing BIN:" << binPath;
        return false;
    }

    return true;
}

// =======================
// Ensure BIN is present and up‑to‑date
// =======================

void USStates::ensureBinaryUpToDate()
{
    qInfo() << "[USStates] ensureBinaryUpToDate() entered";
    qDebug() << "[USStates] ensureBinaryUpToDate() called";

    const QString tsvPath = USStates::tsvPath();
    const QString binPath = USStates::binaryPath();

    QFileInfo tsv(tsvPath);
    QFileInfo bin(binPath);

    qDebug() << "[USStates] TSV path =" << tsvPath << "exists?" << tsv.exists();
    qDebug() << "[USStates] BIN path =" << binPath << "exists?" << bin.exists();

    if (!tsv.exists()) {
        qInfo() << "[USStates] TSV missing, cannot regenerate BIN";
        return;
    }

    if (!bin.exists() || bin.lastModified() < tsv.lastModified()) {
        qDebug() << "[USStates] Regenerating BIN from TSV";

        if (!USStates::generateBinaryFromTSV(tsvPath, binPath)) {
            qInfo() << "[USStates] Failed to generate BIN";
            return;
        }

        qDebug() << "[USStates] BIN successfully regenerated at" << binPath;
    }
}

// =======================
// Backing storage (no locking here)
// =======================

static QVector<USStates::Entry>& dbStorage()
{
    static QVector<USStates::Entry> db;
    return db;
}

// =======================
// Raw loader (no locking; caller must hold mutex)
// =======================

static bool loadDbFromBinary(QVector<USStates::Entry>& db)
{
    qInfo() << "[USStates] loadDbFromBinary() entered";
    qDebug() << "[USStates] loadDbFromBinary() starting";
    db.clear();

    QFile file(USStates::binaryPath());
    if (!file.open(QIODevice::ReadOnly)) {
        qInfo() << "USStates: failed to open" << USStates::binaryPath();
        return false;
    }

    QDataStream in(&file);
    in.setByteOrder(QDataStream::LittleEndian);

    quint32 count = 0;
    in >> count;

    if (in.status() != QDataStream::Ok || count > 5000000) {
        qInfo() << "USStates: invalid BIN header";
        return false;
    }

    db.reserve(count);

    for (quint32 i = 0; i < count; ++i) {
        quint16 len = 0;
        in >> len;

        if (in.status() != QDataStream::Ok || len > 32) {
            qInfo() << "USStates: invalid key length";
            return false;
        }

        QByteArray key(len, Qt::Uninitialized);
        if (len > 0) {
            if (in.readRawData(key.data(), len) != len) {
                qInfo() << "USStates: truncated key data";
                return false;
            }
        }

        quint8 stateCode = 255;
        in >> stateCode;

        if (in.status() != QDataStream::Ok) {
            qInfo() << "USStates: truncated state code";
            return false;
        }

        db.append(USStates::Entry{key, stateCode});
    }

    std::sort(db.begin(), db.end(),
              [](USStates::Entry const& a, USStates::Entry const& b) {
                  return USStates::cmpUnsigned(a.key, b.key) < 0;
              });

    return true;
}

// =======================
// Helper singletons
// =======================

QMutex& USStates::databaseMutex()
{
    static QMutex m;
    return m;
}

bool& USStates::callsignDatabaseLoaded()
{
    static bool loaded = false;
    return loaded;
}

bool& USStates::preloadStarted()
{
    static bool started = false;
    return started;
}

// =======================
// Public accessor: single load under mutex
// =======================

QVector<USStates::Entry>& USStates::callsignDb()
{
    QMutexLocker lock(&databaseMutex());
    auto &db = dbStorage();

    if (!callsignDatabaseLoaded()) {
        if (!loadDbFromBinary(db)) {
            qInfo() << "USStates: BIN corrupt, regenerating";
            USStates::ensureBinaryUpToDate();
            loadDbFromBinary(db);
        }
        callsignDatabaseLoaded() = true;
    }

    return db;
}

// =======================
// Async preload: no tryLock, no manual unlock
// =======================

void USStates::preloadDatabaseAsync()
{
    if (callsignDatabaseLoaded() || preloadStarted())
        return;

    preloadStarted() = true;

    QtConcurrent::run([]() {
        qInfo() << "[USStates] preloadDatabaseAsync() worker started";

        // Ensure BIN is valid on disk
        USStates::ensureBinaryUpToDate();

        // Trigger load under the same mutex logic as everyone else
        (void)USStates::callsignDb();

        qInfo() << "[USStates] preloadDatabaseAsync() worker finished";
        USStates::preloadStarted() = false;
    });
}

void USStates::reloadCallsignDatabase()
{
    QMutexLocker lock(&databaseMutex());
    auto &db = dbStorage();
    db.clear();
    callsignDatabaseLoaded() = false;
}

bool USStates::callsignDatabaseIsLoaded()
{
    QMutexLocker lock(&databaseMutex());
    return callsignDatabaseLoaded();
}


