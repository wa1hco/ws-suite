#include <fftw3.h>
#ifdef QT5
#include <QtWidgets>
#else
#include <QtGui>
#endif
#include <QApplication>
#include <QIcon>
#include <QCoreApplication>
#include <QSettings>
#include <QTcpSocket>
#include <QHostAddress>
#include <QByteArray>
#include <QtEndian>
#include <QRegularExpression>
#include <cstdio>
#include <cstring>
#include <cstdint>

#include "revision_utils.hpp"
#include "mainwindow.h"
#include "qmap_runtime_config.h"
#include "runtime_paths.h"
#include "wsjtx_config.h"   // fortran_charlen_t

extern "C" {
  // Fortran procedures we need
  void four2a_ (_Complex float *, int * nfft, int * ndim, int * isign, int * iform, int len);

  void _gfortran_set_args(int argc, char *argv[]);
  void _gfortran_set_convert(int conv);
  void ftninit_(void);
  void fftbig_(float dd[], int* nfft);
  void jpl_setup_(char* fname, fortran_charlen_t len);
}

// Definitions of the runtime active variables declared in
// qmap_runtime_config.h. Defaults preserve the 96 kHz baseline; main()
// updates them via setActive() once --samplerate is parsed, BEFORE any
// consumer (mainwindow.cpp, widegraph.cpp, etc.) reads them.
namespace qmap_runtime {
int g_active_rate_hz = BASELINE_RATE;
int g_active_nfft    = BASELINE_NFFT;
int g_active_nsmax   = BASELINE_NSMAX;
int g_rate_source    = 0;
int g_bridge_advertised_rate_hz = 0;

void setActive(int rate_hz, int nfft) {
  g_active_rate_hz = rate_hz;
  g_active_nfft    = nfft;
  g_active_nsmax   = 60 * rate_hz;
}

void setRateSource(int source) {
  g_rate_source = source;
}

void setBridgeAdvertisedRateHz(int rate_hz) {
  g_bridge_advertised_rate_hz = rate_hz;
}
}

namespace {
// Round x up to the next power of two; used to map an arbitrary IQ
// sample rate to a sensible NFFT (NFFT scales with rate so the per-bin
// resolution df = rate/NFFT stays in the same ~3 Hz neighbourhood as
// upstream's 96 kHz / 32768 = 2.93 Hz).
int round_pow2(int x) {
  int p = 1;
  while (p < x) p <<= 1;
  return p;
}

// Linrad TCP parameter-server handshake: connect to the bridge's TCP
// port (default 49812 = Linrad CONNPORT), send a real Linrad
// SLAVE_MESSAGE { int32 type=NETMSG_MODE_REQUEST=41656; int32
// frequency=0 } (8 bytes, little-endian), read the 8×int32
// little-endian mode response, return rx_ad_speed (= bridge's
// configured Linrad output IQ rate in Hz). Returns 0 if the handshake
// fails for any reason (bridge not yet running, port wrong, timeout) —
// the caller falls back to the BASELINE_RATE default. The bridge
// advertises rx_ad_speed = output_rate_ in LinradServer::sendModeResponse.
//
// IMPORTANT: this MUST be the 8-byte SLAVE_MESSAGE form, not a single
// 0xB8 byte. Port 49812 is Linrad's CONNPORT, and if the probe lands
// on a real Linrad master instead of our bridge, a truncated message
// triggers Linrad's lirerr(886942) -> EVENT_KILL_ALL, which hangs
// Linrad. With the proper 8-byte form, real Linrad answers cleanly
// (network.c case NETMSG_MODE_REQUEST) and nothing crashes.
int detectBridgeRateViaLinradTcp(const QHostAddress& host, quint16 port,
                                 int timeout_ms = 800) {
  QTcpSocket sock;
  sock.connectToHost(host, port);
  if (!sock.waitForConnected(timeout_ms)) {
    return 0;
  }
  // Build SLAVE_MESSAGE { type=41656, frequency=0 } little-endian.
  static constexpr qint32 NETMSG_MODE_REQUEST = 41656;
  char request[8];
  const qint32 type_le = qToLittleEndian<qint32>(NETMSG_MODE_REQUEST);
  const qint32 freq_le = qToLittleEndian<qint32>(0);
  std::memcpy(request,     &type_le, 4);
  std::memcpy(request + 4, &freq_le, 4);
  if (sock.write(request, 8) != 8 || !sock.waitForBytesWritten(timeout_ms)) {
    return 0;
  }
  // Wait for the 32-byte (8×int32) reply.
  while (sock.bytesAvailable() < 32) {
    if (!sock.waitForReadyRead(timeout_ms)) {
      return 0;
    }
  }
  QByteArray reply = sock.read(32);
  if (reply.size() < 32) return 0;
  // First int32 LE = rx_ad_speed
  qint32 rate_le;
  std::memcpy(&rate_le, reply.constData(), 4);
  const int rate = qFromLittleEndian<qint32>(rate_le);
  if (rate <= 0 || rate > qmap_runtime::MAX_IQ_RATE_HZ) return 0;
  return rate;
}

// Parse --samplerate <hz> (and --samplerate=<hz>) from argv. Returns 0
// if not present or malformed; the caller falls back to the qmap.ini
// default (which itself defaults to BASELINE_RATE).
int extractSampleRateFromArgv(int& argc, char* argv[]) {
  int rate = 0;
  int write = 1;
  for (int read = 1; read < argc; ++read) {
    const char* arg = argv[read];
    if (std::strcmp(arg, "--samplerate") == 0 || std::strcmp(arg, "-samplerate") == 0) {
      if (read + 1 < argc) {
        rate = std::atoi(argv[read + 1]);
        ++read;
      }
      continue;
    }
    if (std::strncmp(arg, "--samplerate=", 13) == 0) {
      rate = std::atoi(arg + 13);
      continue;
    }
    if (std::strncmp(arg, "-samplerate=", 12) == 0) {
      rate = std::atoi(arg + 12);
      continue;
    }
    argv[write++] = argv[read];
  }
  argc = write;
  return rate;
}

// Generic flag-with-value extractor for --flag <val> / --flag=<val> /
// -flag <val> / -flag=<val>. Removes the flag + value from argv before
// QApplication parses the leftover. Used for --config and --decode-log.
QString extractFlagFromArgv(int& argc, char* argv[], const char* flag_long,
                            const char* flag_short) {
    QString val;
    int write = 1;
    const size_t lL = std::strlen(flag_long);
    const size_t lS = std::strlen(flag_short);
    for (int read = 1; read < argc; ++read) {
        const char* arg = argv[read];
        if (std::strcmp(arg, flag_long) == 0 || std::strcmp(arg, flag_short) == 0) {
            if (read + 1 < argc) {
                val = QString::fromLocal8Bit(argv[read + 1]);
                ++read;
            }
            continue;
        }
        if (std::strncmp(arg, flag_long, lL) == 0 && arg[lL] == '=') {
            val = QString::fromLocal8Bit(arg + lL + 1);
            continue;
        }
        if (std::strncmp(arg, flag_short, lS) == 0 && arg[lS] == '=') {
            val = QString::fromLocal8Bit(arg + lS + 1);
            continue;
        }
        argv[write++] = argv[read];
    }
    argc = write;
    return val;
}

// Parse --config <path> (and --config=<path>) from argv. Returns empty
// QString if not present. Lets the user run multiple QMAPs side-by-
// side, each with its own qmap.ini (and therefore its own UDP/TCP
// ports). Same shape as extractSampleRateFromArgv — strip from argv
// before QApplication so Qt doesn't reject the unknown option.
QString extractConfigPathFromArgv(int& argc, char* argv[]) {
  QString path;
  int write = 1;
  for (int read = 1; read < argc; ++read) {
    const char* arg = argv[read];
    if (std::strcmp(arg, "--config") == 0 || std::strcmp(arg, "-config") == 0) {
      if (read + 1 < argc) {
        path = QString::fromLocal8Bit(argv[read + 1]);
        ++read;
      }
      continue;
    }
    if (std::strncmp(arg, "--config=", 9) == 0) {
      path = QString::fromLocal8Bit(arg + 9);
      continue;
    }
    if (std::strncmp(arg, "-config=", 8) == 0) {
      path = QString::fromLocal8Bit(arg + 8);
      continue;
    }
    argv[write++] = argv[read];
  }
  argc = write;
  return path;
}
}

namespace qmap_runtime {
// Active --config path (empty when neither --config nor --rig-name was
// passed; --rig-name <name> is an alias that derives this same path —
// see extractFlagFromArgv("--rig-name", ...) in main()). Set once at
// main() entry, read by widegraph's auto-relaunch so the new instance
// lands in the same INI as the dying parent.
QString g_config_path;

// Active --decode-log path (empty = no logging). When set, each
// decode that lands in the WideGraph's decoded-text browser also
// appends a CSV-friendly line to this file. Used by the regression
// harness to compare QMAP's actual decodes against mapsim's known
// truth (see C:/dev/Q65/testfiles/manifest.csv).
QString g_decode_log_path;
QString g_open_path;
QString g_decoder_timing_log_path;
bool    g_exit_after_decode = false;
}

// bind(C) entry into qmap/libqmap/qmap_timer_init.f90.
extern "C" void qmap_init_timer_c(const char* path);

int main(int argc, char *argv[])
{
  // Pull --samplerate / --config out of argv BEFORE QApplication so
  // Qt doesn't complain about unknown options. Validate the rate
  // against the wide-mode upper bound.
  int requested_rate = extractSampleRateFromArgv(argc, argv);
  if (requested_rate < 0) requested_rate = 0;
  if (requested_rate > qmap_runtime::MAX_IQ_RATE_HZ) {
    std::fprintf(stderr, "[qmap] --samplerate %d exceeds MAX_IQ_RATE_HZ=%d, ignoring\n",
                 requested_rate, qmap_runtime::MAX_IQ_RATE_HZ);
    requested_rate = 0;
  }
  qmap_runtime::g_config_path     = extractConfigPathFromArgv(argc, argv);
  // --rig-name <name> is an alias for --config, spelled the way WSJT-X
  // spells its own multi-instance flag, so the two apps can be launched
  // with matching arguments (e.g. `wsjtx --rig-name A` / `qmap --rig-name A`).
  // Unlike --config it takes a short name, not a path — QMAP derives the
  // same kind of INI path a user would otherwise pick by hand (matches the
  // "wsmap-A.ini" / "wsmap-B.ini" naming already used for --config). Only
  // extracted (and stripped from argv) here; resolved into an actual path
  // further below, once QApplication exists — QCoreApplication::applicationDirPath()
  // returns empty before the app object is constructed.
  const QString rig_name_arg = extractFlagFromArgv(argc, argv, "--rig-name", "-rig-name");
  qmap_runtime::g_decode_log_path = extractFlagFromArgv(argc, argv,
                                                       "--decode-log",
                                                       "-decode-log");
  qmap_runtime::g_open_path       = extractFlagFromArgv(argc, argv,
                                                       "--open",
                                                       "-open");
  qmap_runtime::g_decoder_timing_log_path = extractFlagFromArgv(argc, argv,
                                                       "--decoder-timing-log",
                                                       "-decoder-timing-log");
  // --exit-after-decode is a presence flag; we extract by checking
  // whether it shows up in argv and stripping it. Reuse the same
  // helper by passing it as a key-value flag and discarding the value
  // — but since it has no value, just scan-and-strip directly here.
  {
    int write = 1;
    for (int read = 1; read < argc; ++read) {
      const char* arg = argv[read];
      if (std::strcmp(arg, "--exit-after-decode") == 0
          || std::strcmp(arg, "-exit-after-decode") == 0) {
        qmap_runtime::g_exit_after_decode = true;
        continue;
      }
      argv[write++] = argv[read];
    }
    argc = write;
  }

  // Read optional file to disable highDPI scaling. Anchored to the exe dir
  // (argv[0]): the probe used to be cwd-relative AND ran after the app object
  // was constructed, where the attribute is a no-op.
  {
    const QString probeDir = QFileInfo {argc > 0 ? QString::fromLocal8Bit(argv[0])
                                                 : QString {}}.absoluteDir().absolutePath();
    QFile f(QDir {probeDir}.absoluteFilePath("DisableHighDpiScaling"));
    if (!f.exists()) QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
  }

  QApplication a {argc, argv};

  // Override programs executable basename as application name. Must happen
  // BEFORE qmapDataDir(): AppLocalDataLocation is derived from this name.
  a.setApplicationName ("WSMAP");
  a.setApplicationVersion ("1.0");

  // Window, task bar and alt-tab icon. Set here rather than left to the
  // platform finding the executable's icon resource, which only Windows has.
  QIcon icon;
  for (auto size : {16, 32, 48, 128, 256})
    {
      icon.addFile (QStringLiteral (":/wsmap_icon_%1.png").arg (size));
    }
  a.setWindowIcon (icon);

  // Writable per-user data dir (AppData/Local/QMAP): ini files at its root,
  // per-instance work files under instance-<ID>/.
  const QString dataDir = qmapDataDir();

  // Resolve --rig-name into an actual --config path now that
  // QCoreApplication::applicationDirPath() is valid. --config wins if
  // both were given (it's the more specific, explicit form).
  if (!rig_name_arg.isEmpty()) {
    if (!qmap_runtime::g_config_path.isEmpty()) {
      std::fprintf(stderr, "[qmap] both --config and --rig-name given; --config wins (%s)\n",
                   qmap_runtime::g_config_path.toUtf8().constData());
    } else if (rig_name_arg.contains(QRegularExpression{R"([\\/,])"})) {
      std::fprintf(stderr, "[qmap] Invalid --rig-name - \\ & / not allowed, ignoring\n");
    } else {
      // Derives (and one-time migrates) dataDir/wsmap-<rig>.ini -- the same
      // migration path the default qmap.ini takes.
      qmap_runtime::g_config_path =
          qmapSettingsFile(QCoreApplication::applicationDirPath(), dataDir,
                           "wsmap-" + rig_name_arg + ".ini",
                           "qmap-" + rig_name_arg + ".ini");
      std::fprintf(stderr, "[qmap] --rig-name %s -> using --config INI: %s\n",
                   rig_name_arg.toUtf8().constData(),
                   qmap_runtime::g_config_path.toUtf8().constData());
    }
  }

  // Resolve the settings file BEFORE the Fortran init: the instance ID stored
  // inside it selects the instance-<ID>/ work dir that must be the cwd by the
  // time ftninit_() opens its relative work files (timer.out / all_qmap.txt /
  // red.dat). An explicit --config path is used verbatim (no migration).
  const QString settings_filename = !qmap_runtime::g_config_path.isEmpty()
      ? qmap_runtime::g_config_path
      : qmapSettingsFile(QCoreApplication::applicationDirPath(), dataDir);
  if (!qmap_runtime::g_config_path.isEmpty()) {
    std::fprintf(stderr, "[qmap] using --config INI: %s\n",
                 settings_filename.toUtf8().constData());
  }
  const QString instanceDir =
      qmapInstanceDir(dataDir, qmapInstanceId(settings_filename));
  {
    QFileInfo instanceDirInfo {instanceDir};
    if (!instanceDirInfo.exists() || !instanceDirInfo.isDir()
        || !instanceDirInfo.isWritable() || !QDir::setCurrent(instanceDir)) {
      QString message {"Unable to use WS-MAP working directory: " + instanceDir};
      qWarning() << message;
      QMessageBox::critical(nullptr, QObject::tr("WS-MAP Startup Error"), message);
      return 1;
    }
  }

// Initialize libgfortran (cwd is the instance work dir now, so ftninit's
// relative opens land there):
  _gfortran_set_args(argc, argv);
  _gfortran_set_convert(0);
  ftninit_();

  // Register the JPL ephemeris file for the astro Doppler chain: astro.f90
  // prefers MoonDopJPL over the legacy analytic MoonDop whenever this file
  // exists (the analytic chain is off by up to ~46 Hz 2-way at 10368 MHz).
  // Probe order: per-user data dir (droppable override), beside the exe,
  // the installed share tree (exe in <prefix>/bin), then the source tree
  // (dev builds from build/qmap/). No file -> legacy chain, unchanged.
  {
    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList jplCandidates {
      dataDir + "/JPLEPH",
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
      std::fprintf(stderr, "[qmap] JPLEPH: %s (JPL-ephemeris Doppler active)\n",
                   jpleph.toUtf8().constData());
    } else {
      std::fprintf(stderr, "[qmap] JPLEPH not found -- using legacy analytic Doppler\n");
    }
  }

  // Wire up K1JT's per-stage decoder timing if the harness asked for
  // it. Output file gets one summary block per decoder cycle (q65c
  // calls timer('decode0',101) at end of each cycle, which dumps and
  // resets). Deadlines worth knowing: cycle-2 (nhsym=330) has a 6 s
  // hard bail in qmapa.f90:87, beyond which q65c silently drops the
  // candidate loop.
  if (!qmap_runtime::g_decoder_timing_log_path.isEmpty()) {
    qmap_init_timer_c(qmap_runtime::g_decoder_timing_log_path
                      .toLocal8Bit().constData());
    std::fprintf(stderr, "[qmap] decoder timing -> %s\n",
                 qmap_runtime::g_decoder_timing_log_path
                 .toUtf8().constData());
  }

  // ── Sample-rate precedence ────────────────────────────────────────
  // 1. --samplerate <hz> CLI flag (highest)
  // 2. qmap.ini [Linrad] sample_rate_mode = <hz> (manual override)
  // 3. qmap.ini [Linrad] sample_rate_mode = "auto" (default) → query
  //    bridge via Linrad TCP handshake
  // 4. BASELINE_RATE (96 kHz) if every step above failed
  // The qmap.ini lives in the QMAP data dir (AppData/Local/QMAP), the same
  // settings_filename resolved above and used by mainwindow.cpp.
  // ─────────────────────────────────────────────────────────────────
  QSettings settings(settings_filename, QSettings::IniFormat);
  const QString mode_str = settings.value("Linrad/sample_rate_mode", "auto").toString().toLower();
  const quint16 linrad_tcp_port = static_cast<quint16>(settings.value("Linrad/tcp_port", 49812).toInt());
  const QHostAddress linrad_host(settings.value("Linrad/host", "127.0.0.1").toString());

  int rate_source = 0;   // 0=baseline, 1=cli, 2=ini-manual, 3=auto-detect
  int active_rate = qmap_runtime::BASELINE_RATE;

  // Always attempt the TCP handshake when the bridge is reachable, even
  // if a CLI/INI override is going to win. The advertised rate gets
  // captured in g_bridge_advertised_rate_hz so the WideGraph indicator
  // can warn about override-vs-bridge mismatches.
  const int detected = detectBridgeRateViaLinradTcp(linrad_host, linrad_tcp_port);
  qmap_runtime::setBridgeAdvertisedRateHz(detected);

  if (requested_rate > 0) {
    active_rate = requested_rate;
    // Treat as "auto" (rate_source=3) when --samplerate agrees with
    // what the bridge advertises — covers two cases that should NOT
    // wear the "cli" label:
    //   - WideGraph's auto-restart synthesises --samplerate after a
    //     late-bridge probe; semantically that IS auto-detect.
    //   - User typed --samplerate that happens to match the bridge.
    // Disagreement still surfaces as "cli" + the yellow mismatch
    // warning, so explicit-but-wrong overrides are loud.
    rate_source = (detected > 0 && detected == requested_rate) ? 3 : 1;
  } else if (mode_str != "auto") {
    int ini_rate = mode_str.toInt();
    if (ini_rate > 0 && ini_rate <= qmap_runtime::MAX_IQ_RATE_HZ) {
      active_rate = ini_rate;
      rate_source = 2;
    }
  } else if (detected > 0) {
    active_rate = detected;
    rate_source = 3;
    std::fprintf(stderr, "[qmap] auto-detected bridge IQ rate via Linrad TCP %s:%u: %d Hz\n",
                 linrad_host.toString().toUtf8().constData(), linrad_tcp_port, detected);
  } else {
    std::fprintf(stderr, "[qmap] auto-detect: no Linrad TCP reply from %s:%u (bridge not running?), falling back to baseline %d Hz\n",
                 linrad_host.toString().toUtf8().constData(), linrad_tcp_port,
                 qmap_runtime::BASELINE_RATE);
  }

  // Warn loudly if the user forced a rate (CLI or INI) that doesn't
  // match what the bridge says it's actually sending.
  if (detected > 0 && (rate_source == 1 || rate_source == 2) &&
      detected != active_rate) {
    std::fprintf(stderr,
                 "[qmap] WARNING: bridge advertises %d Hz but %s override forces %d Hz — "
                 "decoder will see garbled spectrum until the rates match.\n",
                 detected, (rate_source == 1) ? "CLI" : "INI", active_rate);
  }

  // Push runtime sample-rate / FFT-size into the qmap_params Fortran
  // module BEFORE any libqmap call. Defaults preserve baseline 96 kHz
  // / NFFT=32768 behaviour exactly when --samplerate isn't passed and
  // qmap.ini doesn't override.
  {
    // Cast to long long: BASELINE_NFFT * active_rate would overflow int32
    // for any active_rate >= ~65 kHz (= 2^31 / 32768).
    int active_nfft = round_pow2(static_cast<int>(
        static_cast<long long>(qmap_runtime::BASELINE_NFFT) * active_rate
        / qmap_runtime::BASELINE_RATE));
    if (active_nfft > qmap_runtime::MAX_NFFT) active_nfft = qmap_runtime::MAX_NFFT;
    qmap_runtime::setActive(active_rate, active_nfft);   // C++ side
    qmap_runtime::setRateSource(rate_source);
    qmap_set_runtime_config(active_rate, active_nfft);   // Fortran side
    const char* source_str =
        (rate_source == 1) ? "cli" :
        (rate_source == 2) ? "ini-manual" :
        (rate_source == 3) ? "auto-detect" : "baseline";
    std::fprintf(stderr, "[qmap] runtime config: sample_rate=%d Hz, nfft=%d, source=%s (baseline=%d/%d, max=%d/%d)\n",
                 active_rate, active_nfft, source_str,
                 qmap_runtime::BASELINE_RATE, qmap_runtime::BASELINE_NFFT,
                 qmap_runtime::MAX_IQ_RATE_HZ, qmap_runtime::MAX_NFFT);
  }

  // switch off as we share an Info.plist file with WSJT-X
  a.setAttribute (Qt::AA_DontUseNativeMenuBar);
  MainWindow w {qmap_runtime::g_config_path};
  w.show ();
  QObject::connect (&a, &QApplication::lastWindowClosed, &a, &QApplication::quit);
  auto result = a.exec ();

  // clean up lazily initialized FFTW3 resources
  {
    int nfft {-1};
    int ndim {1};
    int isign {1};
    int iform {1};
    // free FFT plan resources
    four2a_ (nullptr, &nfft, &ndim, &isign, &iform, 0);
    fftbig_(nullptr, &nfft);
  }
  fftwf_forget_wisdom ();
  fftwf_cleanup ();
  return result;
}
