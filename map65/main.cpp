#include <fftw3.h>
#ifdef QT5
#include <QtWidgets>
#else
#include <QtGui>
#endif
#include <QApplication>
#include <QIcon>
#include <QCoreApplication>
#include <QDir>
#include <QDebug>
#include <QFileInfo>
#include <QStandardPaths>
#ifdef _WIN32
#include <windows.h>
#endif

#include "revision_utils.hpp"
#include "mainwindow.h"
#include "fortran_mutex.hpp"
#include "globals.h"
#include "wsjtx_config.h"   // fortran_charlen_t

#include <cstdio>
#include <cstring>

extern "C" void set_runtime_params_(int rate_hz, int nfft, int nfft_big);

extern "C" {
  // Fortran procedures we need
  void four2a_ (_Complex float *, int * nfft, int * ndim, int * isign, int * iform, int len);
  void jpl_setup_(char* fname, fortran_charlen_t len);
}

extern int g_sampleRate;
extern int active_nfft;
int g_activeNfft = 32768;   // default

// --open <file>: load and decode this .iq/.tf2 at startup, same as
// File->Open. Exists so replay tests need no GUI automation -- QMAP has had
// the same flag since the Q65-30D investigation. Stripped from argv before
// QApplication so Qt does not complain about the unknown option.
QString g_map65_open_path;

int main(int argc, char *argv[])
{
  {
    int write = 1;
    for (int read = 1; read < argc; ++read) {
      if ((std::strcmp(argv[read], "--open") == 0
           || std::strcmp(argv[read], "-open") == 0) && read + 1 < argc) {
        g_map65_open_path = QString::fromLocal8Bit(argv[read + 1]);
        ++read;
        continue;
      }
      argv[write++] = argv[read];
    }
    argc = write;
  }
#ifdef _WIN32
#  ifdef MAP_GUI_SUBSYSTEM
    FreeConsole();
#  endif
#endif

#ifdef _WIN32
#  ifdef MAP_GUI_SUBSYSTEM
#    pragma message("MAP_GUI_SUBSYSTEM is defined in C++")
#  else
#    pragma message("MAP_GUI_SUBSYSTEM is NOT defined in C++")
#  endif
#endif
  
   // Add the WS bundle's plugin directory so Qt can find "cocoa", imageformats, etc.
    QCoreApplication::addLibraryPath(
        QCoreApplication::applicationDirPath()
        + "/../../ws.app/Contents/PlugIns"
    );
  
  QApplication a {argc, argv};

  // Set before anything derives paths from the app name: the data dir is
  // %LOCALAPPDATA%/MAP65, and without this it would resolve as ".../map65"
  // from the executable basename.
  a.setApplicationName ("EME65");
  a.setApplicationVersion ("4.0");

  // Window, task bar and alt-tab icon. Set here rather than left to the
  // platform finding the executable's icon resource, which only Windows has.
  QIcon icon;
  for (auto size : {16, 32, 48, 128, 256})
    {
      icon.addFile (QStringLiteral (":/eme65_icon_%1.png").arg (size));
    }
  a.setWindowIcon (icon);

  QString appDir = QApplication::applicationDirPath();
  // The sample-rate flag decides buffer and FFT sizing before MainWindow
  // exists, so it must come from the SAME ini MainWindow uses: the one in
  // the per-user data dir (where settings migrated 2026-07). Reading the
  // exe-dir copy -- which a normal install doesn't have -- silently forced
  // 96000 whatever the user chose: that is why "192 kHz doesn't work".
  // Fall back to the exe-dir ini only while the data-dir one doesn't exist
  // yet (pre-migration layout; MainWindow migrates it moments later).
  QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
  if (dataDir.isEmpty()) dataDir = QDir::home().absoluteFilePath(".map65");
  QString iniPath = QDir {dataDir}.absoluteFilePath("eme65.ini");
  if (!QFile::exists(iniPath)) iniPath = appDir + "/eme65.ini";
  QSettings settings(iniPath, QSettings::IniFormat);
  // Common/SampleRateHz is the authoritative rate: the literal 95238,
  // 96000 or 192000 (per DG2YCB it lives in the normal Common group).
  // When the key is absent, migrate once from whichever legacy scheme is
  // present -- and sync, so MainWindow's own QSettings sees the result.
  settings.beginGroup("Common");
  int rateHz = settings.value("SampleRateHz", 0).toInt();
  if (rateHz != 95238 && rateHz != 96000 && rateHz != 192000) {
    const QString s96 = settings.value("FSam96000").toString().toLower();
    const bool fs192 = settings.value("FSam192000", false).toBool();
    if (fs192 || s96 == "2") rateHz = 192000;         // 2-bool scheme, or int scheme's 2
    else if (s96 == "false" || s96 == "0") rateHz = 95238;
    else rateHz = 96000;                              // "true", "1", or nothing
    settings.setValue("SampleRateHz", rateHz);
  }
  settings.endGroup();
  settings.sync();
  g_sampleRate = (rateHz == 192000) ? 192000 : 96000;  // 95238 uses 96000-sized buffers

// ------------------------------------------------------------
// NEW: compute active FFT sizes for MAP65
// ------------------------------------------------------------
int active_rate = g_sampleRate;

// Keep ~3 Hz bin resolution like WSJT-X/QMAP
auto round_pow2 = [](int x) {
    int p = 1;
    while (p < x) p <<= 1;
    return p;
};

// symspec FFT size
active_nfft = round_pow2(
    static_cast<int>(
        static_cast<long long>(BASELINE_NFFT) * active_rate / BASELINE_RATE
    )
);

g_activeNfft = active_nfft;

// big FFT size (56 symbols × sample rate)
int active_nfft_big = 56 * active_rate;

// ------------------------------------------------------------
// Push runtime parameters into Fortran BEFORE MainWindow starts
// ------------------------------------------------------------
set_runtime_params_(active_rate, active_nfft, active_nfft_big);

// ------------------------------------------------------------
// allocate buffers now that sample rate is known
// ------------------------------------------------------------
id.resize(4 * 60 * g_sampleRate);

  // (application name/version are set right after QApplication above, before
  // the first data-dir path derivation)

  // Register the JPL ephemeris file for the astro Doppler chain: astro.f90
  // prefers MoonDopJPL over the legacy analytic MoonDop whenever this file
  // exists (the analytic chain is off by up to ~46 Hz 2-way at 10368 MHz).
  // Probe order: per-user data dir (droppable override; needs the
  // applicationName set just above), beside the exe, the installed share
  // tree (exe in <prefix>/bin), then the source tree (dev builds from
  // build/map65/). No file -> legacy chain, unchanged.
  {
    const QString mapDataDir =
        QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    const QStringList jplCandidates {
      mapDataDir + "/JPLEPH",
      appDir + "/JPLEPH",
      appDir + "/../share/ws/JPLEPH",
      appDir + "/../Resources/ws/JPLEPH",   // macOS: inside ws.app
      appDir + "/../../contrib/Ephemeris/JPLEPH",
    };
    QString jpleph;
    for (auto const& cand : jplCandidates) {
      if (QFileInfo::exists(cand)) { jpleph = QDir::cleanPath(cand); break; }
    }
    if (!jpleph.isEmpty()) {
      // Fortran character*256 dummy: blank-padded, no NUL terminator.
      QByteArray buf {jpleph.toLocal8Bit().leftJustified(256, ' ', true)};
      jpl_setup_(buf.data(), 256);
      std::fprintf(stderr, "[map65] JPLEPH: %s (JPL-ephemeris Doppler active)\n",
                   jpleph.toUtf8().constData());
    } else {
      std::fprintf(stderr, "[map65] JPLEPH not found -- using legacy analytic Doppler\n");
    }
  }
  // switch off as we share an Info.plist file with WSJT-X
  a.setAttribute (Qt::AA_DontUseNativeMenuBar);
  MainWindow w;
  
  w.show ();
  QObject::connect (&a, &QApplication::lastWindowClosed, &a, &QApplication::quit);
  auto result = a.exec ();

  // clean up lazily initialized FFTW3 resources
  {
    std::lock_guard<std::mutex> lock(g_fortran_decode_mutex);
    int nfft {-1};
    int ndim {1};
    int isign {1};
    int iform {1};
    // free FFT plan resources
    four2a_ (nullptr, &nfft, &ndim, &isign, &iform, 0);
  }
  fftwf_forget_wisdom ();
  fftwf_cleanup ();

  return result;
}

