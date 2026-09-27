//------------------------------------------------------------------ MainWindow
#include "mainwindow.h"
#include <fftw3.h>
#include <QDir>
#include <QRegularExpression>
#include <QSettings>
#include <QFileInfo>
#include <QStandardPaths>
#include <QStringList>
#include <QTimer>
#include <QToolTip>
#include "revision_utils.hpp"
#include "qt_helpers.hpp"
#include "SettingsGroup.hpp"
#include "widgets/MessageBox.hpp"
#include "RebrandingMigration.hpp"
#include "ui_mainwindow.h"
#include "devsetup.h"
#include "plotter.h"
#include "about.h"
#include "astro.h"
#include "widegraph.h"
#include "messages.h"
#include "bandmap.h"
#include "txtune.h"
#include "sleep.h"
#include "commons.h"
#include "soundin.h"
#include <portaudio.h>
#include <iostream>

#include <QApplication>
#include <QDebug>
#include <QDateTime>
#include <QFile>
#include <QTextStream>
#include <QString>
#include <QByteArray>

//#include <io.h>
#include <stdio.h>

#include <cstdio>
#include <cstdlib>
#include <unistd.h>   // for pipe(), dup2()
#include <math.h>
#include <cmath>      // std::floor
#include <cstring>    // std::memcpy / std::memset
#include <algorithm>  // std::min
#include <thread>
#include <fcntl.h>

#include "stdout_channel.h"
#include "fortran_mutex.hpp"

#if !defined(Q_OS_WIN)
extern "C" {
    void ptt_set_override(const char *path);
}
#endif

extern "C" {
    int ptt_(int* nport, int* itx, int* iptt);
}

#ifdef __unix__
extern "C" void ptt_close(void);
#endif

#ifdef MessageBox
#undef MessageBox
#endif

QByteArray g_TxTuneGeometry;

int g_sampleRate = 96000;
int active_nfft;
std::vector<qint16> id;

short int iwave[2*60*12000];          //Wave file for Tx audio
int nwave;                            //Length of Tx waveform
bool btxok;                           //True if OK to transmit
bool bTune;
bool bIQxt;
double outputLatency;                 //Latency in seconds
int txPower;
int iqAmp;
int iqPhase;
int pipefd[2];  // pipefd[0] = read end, pipefd[1] = write end

TxTune*    g_pTxTune = NULL;

std::atomic<bool> stop_m65{false};

QString guiDate;         //liveCQ
QStringList allDecodes;  //liveCQ
QStringList allDecodes2;  //liveCQ
QString m_otherUrl;
bool m_spot_to_psk_reporter;

bool isValidCallsign(QString const& cs)
{
  static const QRegularExpression callsignRegex{
      R"(^[A-Z0-9]{1,2}[A-Z0-9]?[0-9][A-Z]{1,3}(/?[A-Z0-9]{1,6})?$)"
  };

  QString s = cs.trimmed().toUpper();
  QRegularExpressionMatch m = callsignRegex.match(
      s,
      0,
      QRegularExpression::NormalMatch
      );

  return m.hasMatch();
}

struct MainWindow::DecoderContext 
{ 
  StdoutChannel* stdoutChan; 
  DecoderContext(); 
  ~DecoderContext(); 
};

MainWindow::DecoderContext::DecoderContext()
{
    stdoutChan = new StdoutChannel(
        L"MAP65_STDOUT_MAPPING",
        L"MAP65_STDOUT_EVENT",
        64 * 1024
    );
}

MainWindow::DecoderContext::~DecoderContext()
{
    delete stdoutChan;
}

namespace
{
  // Legacy install location of the old JTSDK-era MAP65 -- probed as a second
  // source when migrating a user's map65.ini / CALL3.TXT into the data dir.
  const QString legacyJtsdkDir {"C:/WSJT/wsjtx/bin"};

  // The writable per-user data directory (Windows: AppData/Local/MAP65) that
  // holds map65.ini, CALL3.TXT and everything the app or decoder writes, so a
  // read-only install location (Program Files) works. Same scheme as upstream
  // WSJT-X 3.x MAP65. Created (with its save/ subdir) on first use.
  QString writableMap65DataDir()
  {
    QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (dataDir.isEmpty()) {
      dataDir = QDir::home().absoluteFilePath(".map65");
    }

    if (!QDir{}.mkpath(dataDir)) {
      qWarning() << "Unable to create MAP65 data directory:" << dataDir;
    }

    QDir dir {dataDir};
    if (!dir.mkpath("save")) {
      qWarning() << "Unable to create MAP65 save directory:" << dir.absoluteFilePath("save");
    }
    return dataDir;
  }

  // map65.ini lives in the data dir. If it is not there yet, migrate a legacy
  // one silently, once: first from beside the exe, then from the old JTSDK
  // install dir. Existing settings survive the location switch unnoticed.
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
  // readSettings' defaults then supply the new data-dir paths. Values
  // elsewhere are deliberate user locations and are kept. This runs only on
  // the migration copy, never again, so any path the user sets afterwards
  // (old-style or not) sticks. Same logic as QMAP's runtime_paths.cpp.
  void dropLegacyDirKeys(QString const& settingsFile, QString const& legacyDir)
  {
    QSettings s {settingsFile, QSettings::IniFormat};
    // Call3Path too: a user-picked CALL3.TXT inside the old MAP65 folder would
    // keep EME65 reading and writing MAP65's copy instead of its own.
    for (auto const& key : {QStringLiteral("Common/SaveDir"),
                            QStringLiteral("Common/AzElDir"),
                            QStringLiteral("Common/Call3Path")}) {
      const QString v = s.value(key).toString();
      if (underDir(v, legacyDir) || underDir(v, legacyJtsdkDir)) {
        qWarning() << "Migrated ini: dropping legacy" << key << "=" << v
                   << "(new data-dir default applies)";
        s.remove(key);
      }
    }
    s.sync();
  }

  QString map65SettingsFile(QString const& appDir, QString const& dataDir)
  {
    QString settingsFile = QDir {dataDir}.absoluteFilePath("eme65.ini");
    if (rebranding_migration::done(settingsFile)) return settingsFile;

    // Carry a legacy ini across once, looking for its PRE-REBRANDING name:
    //   1. the old MAP65 data dir, %LocalAppData%\MAP65 -- the rebranding
    //      migration, and the only source most users will hit;
    //   2. beside the executable, and
    //   3. the old JTSDK install dir -- both pre-date the move into AppData.
    // CALL3.TXT, the operator's callsign database, comes along from the old
    // data dir in the same step; call3Path() still seeds it from the older
    // locations when this finds nothing. The rules -- copy never move, a
    // marker so it runs exactly once, pre-release files set aside rather than
    // kept -- are in RebrandingMigration.hpp, shared with WS and WS-MAP.
    const QString oldDataDir = QDir {QFileInfo {dataDir}.absolutePath()}
                                 .absoluteFilePath(QStringLiteral("MAP65"));

    QString legacySettingsFile;
    QString legacyDir;
    for (auto const& dir : {oldDataDir, appDir, legacyJtsdkDir}) {
      const QString candidate = QDir {dir}.absoluteFilePath("map65.ini");
      if (QFile::exists(candidate)) {
        legacySettingsFile = candidate;
        legacyDir = dir;
        break;
      }
    }

    // Runs even with nothing to copy, so the marker is written and a MAP65
    // installed later can never overwrite settings made in EME65.
    const QList<bool> copied = rebranding_migration::run(settingsFile, {
        {legacySettingsFile, settingsFile},
        {QDir {oldDataDir}.absoluteFilePath("CALL3.TXT"),
         QDir {dataDir}.absoluteFilePath("CALL3.TXT"), true},
      });
    if (copied.value(0)) {
      // Stored paths point into the old tree and would beat the new
      // defaults; drop them so the new data-dir paths apply.
      dropLegacyDirKeys(settingsFile, legacyDir);
    }
    return settingsFile;
  }

  // A MAP65 narrowband decode line ("!"), as written by the Fortran side once
  // per quick-decode pass at fQSO +/- ntol:
  //   JT65  map65a.f90:473  ("!",I3,I5,I4,I6.4,F5.1,I5,1X,A1,1X,A22,...)
  //   Q65   q65b.F90:316    ("!",I3.3,I5,I4,I6.4,F5.1,I5," : ",A28,...)
  // The leading six fields align in both formats, so only the marker column
  // and the message width differ:
  //   col  2- 4 nkHz | 5- 9 ndf | 10-13 npol | 14-19 nutc | 20-24 dt |
  //        25-29 snr | 30-32 marker | 33.. message
  // The marker is " # " for JT65 (map65a's cm is the constant '#',
  // map65a.f90:94) and " : " for Q65 -- the same two characters WSJT-X's
  // processMessage expects in a DecodedText line.
  //
  // Read by fixed columns, not by whitespace tokens: a Fortran field that
  // overflows its width prints as "*****" and would silently shift every
  // token behind it. The message field also holds spaces, which a token
  // split would have to reassemble anyway.
  struct BangDecode
  {
    QString hhmm;
    QString msg;
    int     snr {0};
    double  kHz {0.0};        // kHz within the MHz, as recordCallFreqs wants
    char    modeChar {0};
  };

  bool parseBangLine (QString const& raw, BangDecode& d)
  {
    if (!raw.startsWith ('!') || raw.size () <= 32) return false;
    const QString marker = raw.mid (29, 3);
    if (marker == " : ")      d.modeChar = ':';
    else if (marker == " # ") d.modeChar = '#';
    else return false;   // not a decode -- e.g. the bare marker line at map65a.f90:531
    bool ok = false;
    const int nkHz = raw.mid (1, 3).trimmed ().toInt (&ok);  if (!ok) return false;
    const int ndf  = raw.mid (4, 5).trimmed ().toInt (&ok);  if (!ok) return false;
    d.snr = raw.mid (24, 5).trimmed ().toInt (&ok);          if (!ok) return false;
    d.hhmm = raw.mid (13, 6).trimmed ();
    if (d.hhmm.isEmpty ()) return false;
    d.msg = raw.mid (32, d.modeChar == ':' ? 28 : 22).trimmed ();
    if (d.msg.isEmpty ()) return false;
    d.kHz = nkHz + ndf / 1000.0;
    return true;
  }
}  // namespace

//-------------------------------------------------- MainWindow constructor
MainWindow::MainWindow(QWidget *parent) :
  QMainWindow(parent),
  ui(new Ui::MainWindow),
  m_appDir {QApplication::applicationDirPath ()},
  m_dataDir {writableMap65DataDir ()},
  m_settings_filename {map65SettingsFile (m_appDir, m_dataDir)},
  m_astro_window {new Astro {m_settings_filename}},
  m_band_map_window {new BandMap {m_settings_filename}},
  m_messages_window(nullptr),
  m_wide_graph_window {new WideGraph {m_settings_filename}},
  m_gui_timer {new QTimer {this}}
{
  qDebug() << "IN MainWindow Constructor active_nfft IS: " << active_nfft;

  // Run out of the data dir: the decoder's relative opens (CALL3.TXT and the
  // ftninit work files) then land there even before set_wsjtx_dir_ is set.
  if (!QDir::setCurrent(m_dataDir)) {
    qWarning() << "Unable to set MAP65 working directory:" << m_dataDir;
  }
  // Seed CALL3.TXT into the data dir NOW: the Fortran opens it with
  // status='unknown', which would create an empty file on a decode that runs
  // before the first GUI lookup -- and an existing empty file defeats the
  // lazy-copy migration for good.
  call3Path();
  // One-time wsjt.log migration: the QSO log (Log QSO appends, worked-call
  // seed below, Edit wsjt.log menu) is cwd-relative, so without this a fresh
  // log would silently start in the data dir while the user's QSO history sat
  // stranded beside the old exe. Same probe order and zero-byte handling as
  // the ini/CALL3 migration; never touches a data-dir log that has entries.
  {
    const QString logPath = QDir {m_dataDir}.absoluteFilePath("wsjt.log");
    if (QFileInfo {logPath}.size() <= 0) {
      for (auto const& legacyDir : {m_appDir, legacyJtsdkDir}) {
        const QString legacyLog = QDir {legacyDir}.absoluteFilePath("wsjt.log");
        if (QFileInfo {legacyLog}.size() > 0) {
          QFile::remove(logPath);   // clear an empty stray; copy won't overwrite
          QFile::copy(legacyLog, logPath);
          break;
        }
      }
    }
  }

  constexpr int baseSeconds  = 56;
  const int sampleRate = g_sampleRate;
  constexpr int channels     = 4;   // dd(1..4, t)

  decoderCtx = new DecoderContext();
  startSharedMemoryStdoutReader(decoderCtx);
  
  // Worst-case: xpol = true ? 2 * baseSeconds * sampleRate I/Q pairs
  const int pairsWorst = 2 * baseSeconds * sampleRate;
  const int ddSize     = pairsWorst * channels;
  this->ddSize = ddSize;
  dd = new float[ddSize];
  
  // Tell Fortran about the maximum shape (channels × pairsWorst)
  set_dd_ptr(dd, channels, pairsWorst);
  
  qDebug() << "g_sampleRate:" << g_sampleRate
    << "sampleRate:" << sampleRate
    << "id.size():" << id.size()
    << "bytes:" << id.size() * sizeof(id[0]);

  std::cout << "dd pointer set to: " << dd
            << " size: " << ddSize
            << " (channels=" << channels
            << ", pairsWorst=" << pairsWorst << ")\n";
   
  ss = new float[4 * 322 * active_nfft]; 
  savg = new float[4 * active_nfft];
  set_ss_ptr(ss, 4, 322, active_nfft);   
  std::cout << "ss pointer set to: " << ss << std::endl;
  set_savg_ptr(savg, 4, active_nfft); 
  std::cout << "savg pointer set to: " << savg << std::endl; 

  // Decoder-side snapshot buffers, same shapes as the live ones. The decode
  // chain reads exclusively through these (datcom_ptrs_mod dd_dec/ss_dec/
  // savg_dec); decode() refreshes them with a GUI-thread memcpy before each
  // decoder_ready, so a decode running past the minute wrap can no longer
  // read live buffers that symspec is overwriting -- the in-process
  // equivalent of the old two-process mem_m65 copy. ~340 MB at 96 kHz.
  dd_snap   = new float[ddSize];
  ss_snap   = new float[4 * 322 * active_nfft];
  savg_snap = new float[4 * active_nfft];
  memset(dd_snap,   0, sizeof(float) * ddSize);
  memset(ss_snap,   0, sizeof(float) * 4 * 322 * active_nfft);
  memset(savg_snap, 0, sizeof(float) * 4 * active_nfft);
  set_dd_dec_ptr(dd_snap, channels, pairsWorst);
  set_ss_dec_ptr(ss_snap, 4, 322, active_nfft);
  set_savg_dec_ptr(savg_snap, 4, active_nfft);
  std::cout << "decoder snapshot buffers allocated (dd_snap=" << dd_snap
            << ", ss_snap=" << ss_snap << ", savg_snap=" << savg_snap
            << ")" << std::endl;

  qDebug() << "MAINWINDOW created dd ss savg ";

  ui->setupUi(this);
//  on_EraseButton_clicked();  //placing this here is a bug that will sometimes produce a crash on startup.
  ui->labUTC->setStyleSheet( \
        "QLabel { background-color : black; color : yellow; }");
  ui->labTol1->setStyleSheet( \
        "QLabel { background-color : white; color : black; }");
  ui->labTol1->setFrameStyle(QFrame::Panel | QFrame::Sunken);
  ui->dxStationGroupBox->setStyleSheet("QFrame{border: 5px groove red}");

  QActionGroup* paletteGroup = new QActionGroup(this);
  ui->actionCuteSDR->setActionGroup(paletteGroup);
  ui->actionLinrad->setActionGroup(paletteGroup);
  ui->actionAFMHot->setActionGroup(paletteGroup);
  ui->actionBlue->setActionGroup(paletteGroup);

  QActionGroup* modeGroup = new QActionGroup(this);
  ui->actionNoJT65->setActionGroup(modeGroup);
  ui->actionJT65A->setActionGroup(modeGroup);
  ui->actionJT65B->setActionGroup(modeGroup);
  ui->actionJT65C->setActionGroup(modeGroup);

  QActionGroup* modeGroup2 = new QActionGroup(this);
  ui->actionNoQ65->setActionGroup(modeGroup2);
  ui->actionQ65A->setActionGroup(modeGroup2);
  ui->actionQ65B->setActionGroup(modeGroup2);
  ui->actionQ65C->setActionGroup(modeGroup2);
  ui->actionQ65D->setActionGroup(modeGroup2);
  ui->actionQ65E->setActionGroup(modeGroup2);

  QActionGroup* saveGroup = new QActionGroup(this);
  ui->actionSave_all->setActionGroup(saveGroup);
  ui->actionNone->setActionGroup(saveGroup);

  QActionGroup* DepthGroup = new QActionGroup(this);
  ui->actionNo_Deep_Search->setActionGroup(DepthGroup);
  ui->actionNormal_Deep_Search->setActionGroup(DepthGroup);
  ui->actionAggressive_Deep_Search->setActionGroup(DepthGroup);

  QActionGroup* Q65DepthGroup = new QActionGroup(this);
  ui->actionQ65_Fast->setActionGroup(Q65DepthGroup);
  ui->actionQ65_Normal->setActionGroup(Q65DepthGroup);
  ui->actionQ65_Deep->setActionGroup(Q65DepthGroup);

  QButtonGroup* txMsgButtonGroup = new QButtonGroup;
  txMsgButtonGroup->addButton(ui->txrb1,1);
  txMsgButtonGroup->addButton(ui->txrb2,2);
  txMsgButtonGroup->addButton(ui->txrb3,3);
  txMsgButtonGroup->addButton(ui->txrb4,4);
  txMsgButtonGroup->addButton(ui->txrb5,5);
  txMsgButtonGroup->addButton(ui->txrb6,6);
  connect(txMsgButtonGroup,
        QOverload<QAbstractButton *>::of(&QButtonGroup::buttonClicked),
        this,
        [this, txMsgButtonGroup](QAbstractButton *btn) {
            int id = txMsgButtonGroup->id(btn);
            set_ntx(id);
        });

  connect(ui->decodedTextBrowser,SIGNAL(selectCallsign(bool,bool)),this,
          SLOT(selectCall2(bool,bool)));

  // Callsign-overlay toggle (N6NU 2026-05-12, port of QMAP feature).
  // View menu action ↔ WideGraph state, two-way mirror via signal.
  if (m_wide_graph_window) {
    ui->actionShow_callsigns_on_Waterfall->setChecked(
        m_wide_graph_window->decodeLabelsEnabled());
    connect(ui->actionShow_callsigns_on_Waterfall, &QAction::toggled,
            m_wide_graph_window.data(), &WideGraph::setDecodeLabelsEnabled);
    connect(m_wide_graph_window.data(), &WideGraph::decodeLabelsEnabledChanged,
            ui->actionShow_callsigns_on_Waterfall, &QAction::setChecked);

    // Waterfall callsign click -> populate DX Call. The overlay hit-test is
    // always active (a single click transfers regardless of the toggle);
    // "Enable click-to-work" now only gates the double-click Tx-enable,
    // handled in handleCallsignClick alongside the other click surfaces.
    connect(m_wide_graph_window.data(), &WideGraph::callsignClicked,
            this, &MainWindow::handleCallsignClick);

    // Past-period decode: waterfall row click -> arm/disarm the Decode
    // button for a saved past period (View -> Show/Decode past decodes
    // on Wide Graph).
    connect(m_wide_graph_window.data(), &WideGraph::pastPeriodClicked,
            this, &MainWindow::onPastPeriodClicked);

    // Decoded-callsign overlay transparency — exclusive action group
    // (View → Callsign transparency). None=255 / Low=220 / Medium=200 /
    // High=175. Persisted under [WideGraph]/decode_label_alpha.
    QActionGroup* transparencyGroup = new QActionGroup(this);
    ui->actionTransparency_None  ->setActionGroup(transparencyGroup);
    ui->actionTransparency_Low   ->setActionGroup(transparencyGroup);
    ui->actionTransparency_Medium->setActionGroup(transparencyGroup);
    ui->actionTransparency_High  ->setActionGroup(transparencyGroup);
    {
      const int a = m_wide_graph_window->decodeLabelAlpha();
      if      (a == 175) ui->actionTransparency_High  ->setChecked(true);
      else if (a == 200) ui->actionTransparency_Medium->setChecked(true);
      else if (a == 220) ui->actionTransparency_Low   ->setChecked(true);
      else               ui->actionTransparency_None  ->setChecked(true);
    }
    auto* wg = m_wide_graph_window.data();
    connect(ui->actionTransparency_None,   &QAction::triggered,
            wg, [wg]{ wg->setDecodeLabelAlpha(255); });
    connect(ui->actionTransparency_Low,    &QAction::triggered,
            wg, [wg]{ wg->setDecodeLabelAlpha(220); });
    connect(ui->actionTransparency_Medium, &QAction::triggered,
            wg, [wg]{ wg->setDecodeLabelAlpha(200); });
    connect(ui->actionTransparency_High,   &QAction::triggered,
            wg, [wg]{ wg->setDecodeLabelAlpha(175); });

    // Decoded-callsign overlay font-size — exclusive action group
    // (View → Callsign font size). Small=7 / Normal=8 (default) /
    // Medium=10 / Large=12. Persisted via WideGraph::setDecodeLabelFontSize.
    QActionGroup* fontGroup = new QActionGroup(this);
    ui->actionCallsign_font_small ->setActionGroup(fontGroup);
    ui->actionCallsign_font_normal->setActionGroup(fontGroup);
    ui->actionCallsign_font_medium->setActionGroup(fontGroup);
    ui->actionCallsign_font_large ->setActionGroup(fontGroup);
    switch (wg->decodeLabelFontSize()) {
      case DecodeLabelFontSize::Small:
        ui->actionCallsign_font_small ->setChecked(true); break;
      case DecodeLabelFontSize::Medium:
        ui->actionCallsign_font_medium->setChecked(true); break;
      case DecodeLabelFontSize::Large:
        ui->actionCallsign_font_large ->setChecked(true); break;
      case DecodeLabelFontSize::Normal:
      default:
        ui->actionCallsign_font_normal->setChecked(true); break;
    }
    connect(ui->actionCallsign_font_small,  &QAction::triggered,
            wg, [wg]{ wg->setDecodeLabelFontSize(DecodeLabelFontSize::Small);  });
    connect(ui->actionCallsign_font_normal, &QAction::triggered,
            wg, [wg]{ wg->setDecodeLabelFontSize(DecodeLabelFontSize::Normal); });
    connect(ui->actionCallsign_font_medium, &QAction::triggered,
            wg, [wg]{ wg->setDecodeLabelFontSize(DecodeLabelFontSize::Medium); });
    connect(ui->actionCallsign_font_large,  &QAction::triggered,
            wg, [wg]{ wg->setDecodeLabelFontSize(DecodeLabelFontSize::Large);  });

    // Callsign-overlay anchor position. Top (legacy) or Bottom (sit
    // above the divider so fresh signals at the top of the waterfall
    // remain visible). Persisted under [WideGraph]/decode_label_position
    // via WideGraph::setDecodeLabelPosition.
    QActionGroup* positionGroup = new QActionGroup(this);
    ui->actionCallsign_position_top   ->setActionGroup(positionGroup);
    ui->actionCallsign_position_bottom->setActionGroup(positionGroup);
    if (wg->decodeLabelPosition() == DecodeLabelPosition::Bottom) {
      ui->actionCallsign_position_bottom->setChecked(true);
    } else {
      ui->actionCallsign_position_top   ->setChecked(true);
    }
    connect(ui->actionCallsign_position_top,    &QAction::triggered,
            wg, [wg]{ wg->setDecodeLabelPosition(DecodeLabelPosition::Top);    });
    connect(ui->actionCallsign_position_bottom, &QAction::triggered,
            wg, [wg]{ wg->setDecodeLabelPosition(DecodeLabelPosition::Bottom); });
  }

  setWindowTitle (program_title ());
  qDebug() << "MAINWINDOW about to start soundInThread SIGNAL/SLOT connections";

  connect(&soundInThread, SIGNAL(readyForFFT(int)),
             this, SLOT(dataSink(int)));
  connect(&soundInThread, SIGNAL(error(QString)), this,
          SLOT(showSoundInError(QString)));
  connect(&soundInThread, SIGNAL(status(QString)), this,
          SLOT(showStatusMessage(QString)));
  createStatusBar();
  qDebug() << "MAINWINDOW created soundInThread SIGNAL/SLOT connections";

  connect(&proc_editor, &QProcess::errorOccurred, this, &MainWindow::editor_error);

  connect(m_gui_timer, &QTimer::timeout, this, &MainWindow::guiUpdate);

  m_auto=false;
  m_waterfallAvg = 1;
  m_network = true;
  m_txFirst=false;
  m_txMute=false;
  btxok=false;
  m_restart=false;
  m_transmitting=false;
  m_widebandDecode=false;
  m_ntx=1;
  m_myCall="K1JT";
  m_myGrid="FN20qi";
  m_saveDir="/users/joe/map65/install/save";
  m_azelDir="/users/joe/map65/install/";
  m_editorCommand="notepad";
  m_txFreq=125;
  m_setftx=0;
  m_loopall=false;
  m_saveAll=false;
  m_onlyEME=false;
  m_sec0=-1;
  m_hsym0=-1;
  m_palette="CuteSDR";
  m_map65RxLog=1;                     //Write Date and Time to all65.txt
  m_nutc0=9999;
  m_kb8rq=false;
  m_NB=false;
  m_mode="JT65B";
  m_mode65=2;
  m_fs96000=1;
  m_udpPort=50004;
  m_adjustIQ=0;
  m_applyIQcal=0;
  m_colors="000080ffffffc8c8e68c8cb45a5a82";
  m_colorPreset="White on Blue";
  m_nsave=0;
  m_modeJT65=0;
  m_modeQ65=0;
  m_TRperiod=60;
  m_modeTx="JT65";
  bTune=false;
  txPower=100;
  iqAmp=0;
  iqPhase=0;

  xSignalMeter = new SignalMeter(ui->xMeterFrame);
  xSignalMeter->resize(50, 160);
  ySignalMeter = new SignalMeter(ui->yMeterFrame);
  ySignalMeter->resize(50, 160);

  fftwf_import_wisdom_from_filename (QDir {m_dataDir}.absoluteFilePath ("map65_wisdom.dat").toLocal8Bit ());

  readSettings();		             //Restore user's setup params

  // Attach (or create) the shared segment WSJT-X polls for click-to-work.
  // Key scheme matches QMAP exactly: instance 1 uses the legacy "mem_qmap"
  // name, 2..4 use "mem_qmap_N". MAP65 impersonates a QMAP client, so
  // WSJT-X can't tell them apart. Only the click_* / dxcall fields are used
  // here (MAP65 is not the decode source); the segment size must match the
  // 4096 WSJT-X/QMAP allocate.
  {
    const QString key = (m_instanceId == 1) ? QStringLiteral("mem_qmap")
                                             : QString("mem_qmap_%1").arg(m_instanceId);
    m_memQmap.setKey(key);
    std::fprintf(stderr, "[map65] mem_qmap key: %s (instance ID %d)\n",
                 key.toUtf8().constData(), m_instanceId);
    constexpr int memSize = 4096;
    if (!m_memQmap.attach()) m_memQmap.create(memSize);
    if (m_memQmap.isAttached()) {
      m_qmapShm = static_cast<QmapShared*>(m_memQmap.data());
    } else {
      std::fprintf(stderr, "[map65] WARNING: could not attach/create mem_qmap\n");
    }
  }

  // Item 4: attach/create the forward-decode segment MAP65 pushes its full
  // decode stream into for WSJT-X's processMessage(). Zero only on create so
  // an already-running WSJT-X reader keeps its cursor.
  {
    const QString key = (m_instanceId == 1) ? QStringLiteral("mem_qmap_fwd")
                                             : QString("mem_qmap_fwd_%1").arg(m_instanceId);
    m_memFwd.setKey(key);
    if (!m_memFwd.attach()) {
      if (m_memFwd.create(sizeof(QmapFwd))) {
        m_memFwd.lock(); std::memset(m_memFwd.data(), 0, sizeof(QmapFwd)); m_memFwd.unlock();
      }
    }
    if (m_memFwd.isAttached()) m_fwdShm = static_cast<QmapFwd*>(m_memFwd.data());
    std::fprintf(stderr, "[map65] mem_qmap_fwd key: %s attached=%d\n",
                 key.toUtf8().constData(), m_memFwd.isAttached());
  }

  // WSJT-X integration menu (defined in mainwindow.ui). Controls the
  // click-to-work relay to WSJT-X and the shared-segment Instance ID.
  {
    ui->actionSync_WSJTX->setChecked(m_syncWsjtx);
    connect(ui->actionSync_WSJTX, &QAction::toggled, this,
            [this](bool on){ m_syncWsjtx = on; writeSettings(); });

    connect(ui->actionContinuous_waterfall, &QAction::toggled, this,
            [this](bool){ writeSettings(); });

    ui->actionTx_via_WSJTX->setChecked(m_txViaWsjtx);
    connect(ui->actionTx_via_WSJTX, &QAction::toggled, this,
            [this](bool on){ m_txViaWsjtx = on; writeSettings(); });

    // Attaching the inferred callsign to a forwarded shorthand is an
    // inference, not a decode -- off by default, its own View-menu toggle.
    // (Shorthand itself is always forwarded when Send-data is on.)
    ui->actionAdd_Call_to_SH->setChecked(m_addCallToSh);
    connect(ui->actionAdd_Call_to_SH, &QAction::toggled, this,
            [this](bool on){ m_addCallToSh = on; writeSettings(); });

    QActionGroup* idGroup = new QActionGroup(this);
    QAction* const idActs[4] = {ui->actionInstance_ID_1, ui->actionInstance_ID_2,
                                ui->actionInstance_ID_3, ui->actionInstance_ID_4};
    for (int i = 1; i <= 4; ++i) {
      QAction* a = idActs[i-1];
      a->setActionGroup(idGroup);
      a->setChecked(i == m_instanceId);
      connect(a, &QAction::triggered, this, [this, i]{
        if (i == m_instanceId) return;
        m_instanceId = i;
        writeSettings();
        msgBox(tr("Instance ID set to %1. Restart EME65 to apply "
                  "(the shared segment is chosen at startup).").arg(i));
      });
    }
  }
  PaError paerr=Pa_Initialize();                    //Initialize Portaudio
  if(paerr!=paNoError) {
    msgBox("Unable to initialize PortAudio.");
  }
  QFile quitFile(m_dataDir + "/.quit");
  quitFile.remove();
    
  m_pbdecoding_style1="QPushButton{background-color: cyan; \
      border-style: outset; border-width: 1px; border-radius: 5px; \
      border-color: black; min-width: 5em; padding: 3px;}";
  m_pbmonitor_style="QPushButton{background-color: #00ff00; \
      border-style: outset; border-width: 1px; border-radius: 5px; \
      border-color: black; min-width: 5em; padding: 3px;}";
  m_pbAutoOn_style="QPushButton{background-color: red; color: white; \
      border-style: outset; border-width: 1px; border-radius: 5px; \
      border-color: black; min-width: 5em; padding: 3px;}";

  genStdMsgs("");

  // The satellite windows all exist (ctor init list); show only the ones
  // that were open when MAP65 last exited.
  on_actionWide_Waterfall_triggered();   // always open the Wide Graph window
  if(m_astroOpen) on_actionAstro_Data_triggered();
  if(m_bandMapOpen) on_actionBand_Map_triggered();
  
  m_band_map_window->setColors(m_colors, m_colorPreset != "Classic");
  if (m_astro_window) m_astro_window->setFontSize (m_astroFont);

  if(m_modeQ65==0) on_actionNoQ65_triggered();
  if(m_modeQ65==1) on_actionQ65A_triggered();
  if(m_modeQ65==2) on_actionQ65B_triggered();
  if(m_modeQ65==3) on_actionQ65C_triggered();
  if(m_modeQ65==4) on_actionQ65D_triggered();
  if(m_modeQ65==5) on_actionQ65E_triggered();

  if(m_modeJT65==0) on_actionNoJT65_triggered();
  if(m_modeJT65==1) on_actionJT65A_triggered();
  if(m_modeJT65==2) on_actionJT65B_triggered();
  if(m_modeJT65==3) on_actionJT65C_triggered();
  future1 = new QFuture<void>;
  watcher1 = new QFutureWatcher<void>;
  connect(watcher1, SIGNAL(finished()),this,SLOT(diskDat()));
  bool ok = connect(watcher1, SIGNAL(finished()), this, SLOT(onDiskDecodeFinished()));
  qDebug() << "onDiskDecodeFinished watcher connected" << ok;

  future2 = new QFuture<void>;
  watcher2 = new QFutureWatcher<void>;
  connect(watcher2, SIGNAL(finished()),this,SLOT(diskWriteFinished()));

// Assign input device and start input thread
  soundInThread.setInputDevice(m_paInDevice);
  if(m_fs96000 == 1) soundInThread.setRate(96000.0);
  else if(m_fs96000 ==0) soundInThread.setRate(95238.1);
  else if(m_fs96000 ==2) soundInThread.setRate(192000.0);
  soundInThread.setBufSize(10*7056);
  soundInThread.setNetwork(m_network);
  soundInThread.setPort(m_udpPort);
  if(!m_xpol) soundInThread.setNrx(1);
  if(m_xpol) soundInThread.setNrx(2);
  soundInThread.start(QThread::HighestPriority);

  // Assign output device and start output thread
  soundOutThread.setOutputDevice(m_paOutDevice);

  m_monitoring=true;                           // Start with Monitoring ON
  qDebug() << "m_monitoring set to" << m_monitoring << "at" << Q_FUNC_INFO;

  soundInThread.setMonitoring(m_monitoring);
  m_diskData=false;
  m_wide_graph_window->setFcal(m_fCal);
  m_wide_graph_window->m_mult570=m_mult570;
  m_wide_graph_window->m_mult570Tx=m_mult570Tx;
  m_wide_graph_window->m_cal570=m_cal570;
  m_wide_graph_window->m_TxOffset=m_TxOffset;
  if(m_initIQplus) m_wide_graph_window->initIQplus();

// Create "m_worked", a dictionary of all calls in wsjt.log
  QFile f("wsjt.log");
  qDebug() << "MainWindow Constructor File open result:" << f.open(QFileDevice::ReadOnly);
  if(f.isOpen()) {
    QTextStream in(&f);
    QString line,t,callsign;
    for(int i=0; i<99999; i++) {
      line=in.readLine();
      if(line.length()<=0) break;
      t=line.mid(18,12);
      callsign=t.mid(0,t.indexOf(","));
      m_worked[callsign]=true;
    }
    f.close();
  }

  if(ui->actionLinrad->isChecked()) on_actionLinrad_triggered();
  if(ui->actionCuteSDR->isChecked()) on_actionCuteSDR_triggered();
  if(ui->actionAFMHot->isChecked()) on_actionAFMHot_triggered();
  if(ui->actionBlue->isChecked()) on_actionBlue_triggered();

  connect (m_wide_graph_window.get (), &WideGraph::freezeDecode2, this, &MainWindow::freezeDecode);
  connect (m_wide_graph_window.get (), &WideGraph::f11f12, this, &MainWindow::bumpDF);

  if (m_band_map_window)
    connect (m_band_map_window.data(), &BandMap::callsignClicked, this, &MainWindow::handleBandMapCallsignClick);

  QTimer::singleShot (0, this,[this]() {
    // Independent top-level window (NO parent), mirroring Astro/BandMap/
    // WideGraph. Owned by MainWindow via QScopedPointer, so ~Messages()
    // still runs at MainWindow teardown -- that destructor's worker-thread
    // cleanup is the exit-hang fix and must be preserved. MessagesGeom is
    // persisted separately by the shutdown path (setClosingForShutdown()
    // + close() -> Messages::closeEvent), so no parent is needed for that
    // either. Being parentless gives it its own taskbar button and lets it
    // minimize independently, instead of minimizing with the main window
    // the way a parented dialog does on Windows.
    m_messages_window.reset(new Messages(m_settings_filename));
    if(m_messagesOpen) on_actionMessages_triggered();
    connect (m_messages_window.data(), &Messages::click2OnCallsign, this, &MainWindow::doubleClickOnMessages);
    if (m_messages_window) m_messages_window->setColors(m_colors, m_colorPreset != "Classic");
  });
  
   QTimer::singleShot (2000, [=] {    
    setNhsym(1);
   });
   QTimer::singleShot (4000, [=] {       
    qDebug() << "MAINWINDOW reads Fortran fcenter as: " << getFcenter();
   });
  
  //default freq at startup for Doppler and Tsky  
  setFcenter(m_wide_graph_window->m_dForceCenterFreq);
  if( getFcenter() == 0) setFcenter(144.125);
  
  qDebug() << "MAINWINDOW reached end of Constructor";
  // only start the guiUpdate timer after this constructor has finished
  QTimer::singleShot (0, [=] {
           m_gui_timer->start(100); //Don't change the 100 ms!
         });
         
  // Start Run_m65       
  QTimer::singleShot(0, this, SLOT(startDecoder()));

  // --open <file>: load it once the decoder is up, exactly as File->Open
  // would. Delay matches QMAP's startup open.
  extern QString g_map65_open_path;
  if (!g_map65_open_path.isEmpty()) {
    const QString open_path = g_map65_open_path;
    // 7 s, not QMAP's 1 s: MAP65's decoder thread and symspec take several
    // seconds to come up, and a file loaded before that decodes nothing.
    QTimer::singleShot(7000, this, [this, open_path] { openIQFile(open_path); });
  }
}

  //--------------------------------------------------- MainWindow destructor
MainWindow::~MainWindow()
{
  // Stop stdout reader thread
  stdoutReaderStop.store(true);

  // Wake event so thread exits immediately
  if (decoderCtx && decoderCtx->stdoutChan) {
      void* ev = decoderCtx->stdoutChan->eventHandle;
      if (ev)
          win_set_event(ev);
  }

  if (stdoutReaderThread.joinable())
      stdoutReaderThread.join();
  writeSettings();
  if (soundInThread.isRunning()) {
    soundInThread.quit();
    soundInThread.wait(3000);
  }
  if (soundOutThread.isRunning()) {
    soundOutThread.quitExecution=true;
    soundOutThread.wait(3000);
  }
  Pa_Terminate();
  fftwf_export_wisdom_to_filename (QDir {m_dataDir}.absoluteFilePath ("map65_wisdom.dat").toLocal8Bit ());
  delete ui;
}

void MainWindow::startDecoder()
{
    qDebug() << "MAINWINDOW calling run_m65_ ";

    // Hand the data dir to Fortran: m65a uses it as the ftninit dir, so the
    // decoder's work files and CALL3.TXT resolve there regardless of cwd.
    QByteArray runtimeDir = QDir::toNativeSeparators(m_dataDir).toLocal8Bit();
    set_wsjtx_dir_(runtimeDir.constData(), runtimeDir.size());
    pushCall3PathToDecoder();

    QFutureWatcher<void>* watcher_m65 = new QFutureWatcher<void>(this);
    connect(watcher_m65, &QFutureWatcher<void>::finished,
            this, &MainWindow::onRunM65Finished);

watcher_m65->setFuture(QtConcurrent::run([=]() {

    qDebug() << "DECODE: starting run_m65_() with m_fs96000=" << m_fs96000;

    // Hook up shared stdout channel for Fortran
    StdoutSharedRegion* region =
        decoderCtx->stdoutChan->shared.getRegion();

    void*    bufPtr  = static_cast<void*>(region->buffer);
    void*    hdrPtr  = static_cast<void*>(&region->header);
    int      bufSize = static_cast<int>(decoderCtx->stdoutChan->shared.getBufferSize());
    intptr_t eventH  = reinterpret_cast<intptr_t>(decoderCtx->stdoutChan->eventHandle);

    set_stdout_channel(bufPtr, hdrPtr, bufSize, eventH);

    // Now run the decoder
    std::lock_guard<std::mutex> lock(g_fortran_decode_mutex);
    int xpol_flag = m_xpol ? 1 : 0;
    run_m65_(&xpol_flag, &m_fs96000);

    qDebug() << "DECODE: run_m65_() returned";
}));

    if (m_fs96000 == 0) {
      lab8->setStyleSheet("QLabel{background-color: #ffc783}");
      lab8->setText("95.238 kHz");
    }
    if (m_fs96000 == 1) {
      lab8->setStyleSheet("QLabel{background-color: #b4ffb4}");
      lab8->setText("96 kHz");
    }
    if (m_fs96000 == 2) {
      lab8->setStyleSheet("QLabel{background-color: #ffccff}");
      lab8->setText("192 kHz");
    }
}

void MainWindow::startSharedMemoryStdoutReader(DecoderContext* ctx)
{
    stdoutReaderThread = std::thread([this, ctx]() {

      StdoutSharedRegion* region =
          ctx->stdoutChan->shared.getRegion();

      char* bufferBase =
          reinterpret_cast<char*>(region->buffer);

      std::size_t bufSize =
          ctx->stdoutChan->shared.getBufferSize();

        // Linux: eventHandle is a void* pointing to posix_event_t
        void* ev = ctx->stdoutChan->eventHandle;

      // NEW: start reading from the current writeIndex
      StdoutSharedHeader h0 = region->header;
      std::uint32_t readIndex = h0.writeIndex;
      if (readIndex >= bufSize)
          readIndex = 0;

      std::string lineBuffer;

      while (!stdoutReaderStop.load()) {

            // Linux replacement for WaitForSingleObject(ev, INFINITE)
            win_wait_for_single_object(ev);

            // After wakeup, read new data
          StdoutSharedHeader h = region->header;
          std::uint32_t writeIndex = h.writeIndex;
          if (writeIndex >= bufSize)
              writeIndex = 0;

          while (readIndex != writeIndex) {
              char c = bufferBase[readIndex];
              readIndex++;
              if (readIndex >= bufSize)
                  readIndex = 0;

              lineBuffer.push_back(c);
              if (c == '\n') {
                  std::string line = lineBuffer;
                  lineBuffer.clear();

                  QString text = QString::fromStdString(line);
                  QMetaObject::invokeMethod(
                      this,
                      [this, text]() { processStdOut(text); },
                      Qt::QueuedConnection
                  );
              }
          }
      }

    });
}

void MainWindow::processStdOut(QString t)
{
  // === FDR: filter false decodes before any parsing ===
  if (ui->actionReduce_false_decodes->isChecked()) {
    QString s = t.trimmed();

    // Always pass short-format messages (RO, RRR, 73)
    if (s.contains(" RO ") || s.contains(" RRR ") || s.contains(" 73 ")) {
      // always pass
    } else if (t.startsWith("!")) {
      // Narrowband decode lines carry a '#' (JT65) or ':' (Q65) marker too, so
      // they fall into the wideband branch below -- where the fixed column
      // indices land on nutc/dt/snr instead of the message and NO genuine
      // decode can ever produce a valid callsign there. With FDR enabled that
      // silently discarded every narrowband decode. Test the message field.
      BangDecode d;
      if (parseBangLine(t, d)) {
        bool hasValidCall = false;
        const QStringList mw = d.msg.toUpper().split(QRegularExpression("\\s+"), SkipEmptyParts);
        for (const QString& w : mw) if (isValidCallsign(w)) { hasValidCall = true; break; }
        if (!hasValidCall) {
          qDebug() << "FDR rejected MSG:" << s;
          return;
        }
      }
    } else if (s.startsWith("@") || s.startsWith("&")) {
      // Wideband decode ("@") and bandmap ("&") lines only -- decoder status
      // text also carries ':' and '#' and must pass untouched. No fixed
      // column holds the message: an f8.3 frequency below 1000 MHz keeps its
      // leading space so "@" stands alone as a token, while at four MHz
      // digits it glues to the frequency and shifts every later token by
      // one. So scan all tokens. The numeric fields, grids, and the #/:
      // markers can never match the callsign shape, so a line survives
      // exactly when its message carries a callsign somewhere.
      const QStringList cols = s.split(QRegularExpression("\\s+"), SkipEmptyParts);
      if (cols.size() >= 6) {
        bool hasValidCall = false;
        for (const QString& w : cols) {
          if (isValidCallsign(w.toUpper())) { hasValidCall = true; break; }
        }
        if (!hasValidCall) {
          qDebug() << "FDR rejected MSG:" << s;
          return;   // block false decode BEFORE it reaches any parser
        }
      }
    }
  }

  // cache + (DX-filtered) forward each decode to WSJT-X. Both streams: "@" is
  // the wideband list, "!" the narrowband decodes at fQSO -- they are very
  // nearly disjoint, so keying off "@" alone lost every decode the wideband
  // pass did not repeat (see forwardDecodeToWsjtx).
  if (t.startsWith("@") || t.startsWith("!")) forwardDecodeToWsjtx(t);

//  qDebug().noquote() << QDateTime::currentMSecsSinceEpoch() << "PROCESS STDOUT:" << t;

  //qDebug() << "in processStdOut STDOUT:" << t;
  if (t.indexOf("<QuickDecodeDone>") >= 0) {

  // Any decoder output means "not idle" — reset idle timer in auto/disk mode
 
      m_nsum  = t.mid(17,4).toInt();
      m_nsave = t.mid(21,4).toInt();
      lab7->setText(QString{"Avg: %1"}.arg(m_nsum));
    if (m_modeQ65 > 0)
        m_wide_graph_window->setDecodeFinished();
  }

  // --- <EarlyFinished> / <DecodeFinished> ---
  if (t.indexOf("<EarlyFinished>") >= 0 || t.indexOf("<DecodeFinished>") >= 0) {
    // A pass has left the decoder: the snapshot is free to be refreshed.
    m_decoderRunning = false;

    if (m_widebandDecode) {
        if (m_messages_window) {
            m_messages_window->setDiskMode(m_diskData);
            m_messages_window->setText(m_messagesText, m_bandmapText);
        }
        if (m_band_map_window)
            m_band_map_window->setText(m_bandmapText);
        m_widebandDecode = false;
    }
    if (t.indexOf("<DecodeFinished>") >= 0) {
        ++m_decodeFinishedCount;

          decodeBusy(false); 
    //  qDebug().noquote() << QDateTime::currentMSecsSinceEpoch() << "decodeBusy(false)";
      if (m_diskData) onDiskDecodeFinished();

        int ndecodes = t.mid(40,5).toInt();
        lab5->setText(QString::number(ndecodes));
        m_map65RxLog   = 0;        
    }

    ui->DecodeButton->setStyleSheet("");
    return;
  }

  // --- same position as legacy ---
  read_log();

  // --- "!" decoded text lines ---
  if (t.startsWith("!")) {
    int n = t.length();
    int m = 2;
#ifdef WIN32
    m = 3;
#endif
    const QString decode_line = t.mid(1, n - m);
    if (n >= 30 || t.indexOf("Best-fit") >= 0) {
      // "!" columns: nkHz ndf npol nutc dt nsync2 cm decoded...
      // Shorthand (RO/RRR/73) carries no callsign, so when forwarding is on we
      // append the callsign we INFERRED for it, rendered INVERTED so it can
      // never be mistaken for something actually received. Gated on the
      // forwarding feature: a legacy MAP65 user sees the line exactly as before.
      QString shCall;
      const QStringList ft = decode_line.split(QRegularExpression("\\s+"), SkipEmptyParts);
      if (ft.size() >= 8) {
        bool ok0=false, ok1=false;
        const double k0 = ft[0].toDouble(&ok0);
        const double d1 = ft[1].toDouble(&ok1);
        if (ok0 && ok1) {
          const double kHz = k0 + d1 / 1000.0;
          // Learn normal-decode frequencies here as well as from the "@" lines:
          // "!" lines are emitted first, so without this a shorthand in the very
          // same decode batch would have nothing to match against.
          QStringList mw; for (const QString& w : ft.mid(7)) mw << w.toUpper();
          recordCallFreqs(mw, kHz);
          const QString w0 = ft[7].toUpper();
          if (m_syncWsjtx && m_addCallToSh && (w0 == "RO" || w0 == "RRR" || w0 == "73"))
            shCall = inferShorthandCall(kHz);
        }
      }
      if (shCall.isEmpty()) {
        ui->decodedTextBrowser->append(decode_line);
      } else {
        QTextCursor c {ui->decodedTextBrowser->document()};
        c.movePosition(QTextCursor::End);
        if (!ui->decodedTextBrowser->document()->isEmpty()) c.insertBlock();
        c.insertText(decode_line + "  (" + shCall + ")");
/*
        // Invert fg/bg colors
        const QPalette& pal = ui->decodedTextBrowser->palette();
        QTextCharFormat plain;                    // the view's normal colours
        plain.setForeground(pal.color(QPalette::Text));
        plain.clearBackground();
        QTextCharFormat inv;                      // inverted = the two swapped
        inv.setBackground(pal.color(QPalette::Text));
        inv.setForeground(pal.color(QPalette::Base));
        c.insertText(decode_line + "  ", plain);
        c.insertText(shCall, inv);
        // Restore the plain format, otherwise every later append() inherits the
        // inverted run and the whole window turns over.
        c.insertText(" ", plain);
        ui->decodedTextBrowser->setCurrentCharFormat(plain);
*/
      }
    }
    int max = ui->decodedTextBrowser->verticalScrollBar()->maximum();
    ui->decodedTextBrowser->verticalScrollBar()->setValue(max);

    // clear snapshots for this decode run, just like legacy
    m_messagesText.clear();
    m_bandmapText.clear();
  }

  // --- "@" message lines ---
  if (t.startsWith("@")) {
    m_messagesText += t.mid(1);
    m_widebandDecode = true;
  }

  // --- "&" bandmap lines ---
  if (t.startsWith("&")) {

    QString s = t.mid(1).trimmed();   // "61 -183 KD5FZX 0 CQ W1ABC FN20"
    QStringList cols = s.split(QRegularExpression("\\s+"), SkipEmptyParts);
    if (cols.size() < 3) return;

    bool ok1=false;
    int nkHz = cols[0].toInt(&ok1);
    if (!ok1) return;

    QString callsign = cols[2];
    callsign.remove('#');

    // Remember this call's audio-offset frequency (kHz = nkHz + ndf/1000)
    // so a BandMap click can relay the signal frequency to WSJT-X.
    { bool okdf=false; int ndf = cols[1].toInt(&okdf);
      m_bandmapFreq[callsign] = nkHz + (okdf ? ndf / 1000.0 : 0.0); }

    // cols[3] is the iage/color-index marker; cols[4:] (if present) is the
    // raw decoded message text followed by a trailing 2-char cmode marker
    // ("#A/#B/#C" JT65, ":A/:B/:C" Q65) (display.f90 change 2026-07-13) --
    // lets BandMap clicks get a real grid + mode instead of only ever
    // falling back to the CALL3.TXT callbook lookup.
    if (cols.size() > 4) {
      const QString cmode = cols.last();
      m_bandmapMode[callsign] = cmode.startsWith("#");
      QString sender, grid;
      senderAndGridFromMsgCols(cols.mid(4, cols.size() - 5), sender, grid);
      if (!grid.isEmpty()) m_bandmapGrid[callsign] = grid;
    }

    QString freqStr = QString("%1").arg(nkHz, 3, 10, QChar('0'));

    QString q;
    if (m_worked[callsign]) {
      q = QString("%1  %2").arg(freqStr).arg(callsign);
    } else {
      q = QString("%1  *%2").arg(freqStr).arg(callsign);
    }

    m_bandmapText += q + "\n";
  }

  // --- "=" debug lines ---
  if (t.startsWith("=")) {
    int n = t.size();
    qDebug() << t.mid(1, n - 3).trimmed();
  }

  // --- UNIVERSAL DECODE LABEL PARSER (JT65 + Q65) ---
  if (ui->actionShow_callsigns_on_Waterfall->isChecked()) {
    QString t2 = t.trimmed();

    // Control characters at line start:
    //  - '!'  : main JT65 decode line
    //  - '@'  : Q65 (or MAP65-style "frequency header" line, also used for adaptive JT65)
    bool is_main_jt65 = t2.startsWith("!");
    bool is_q65       = t2.startsWith("@");

    // JT65 messages window line:
    //  - no leading '!' or '@'
    //  - starts with 3 digits (index or kHz)
    //  - contains a '#' marker (#, #H, #V)
    bool is_msg_jt65 =
        !t2.startsWith("!") &&
        !t2.startsWith("@") &&
        t2.size() > 3 &&
        t2[0].isDigit() && t2[1].isDigit() && t2[2].isDigit() &&
        t2.contains("#");

    if (is_main_jt65 || is_q65 || is_msg_jt65) {

      // Strip control characters and non-printable chars, work on cleaned string s
      QString s = t;
      for (int i = s.size() - 1; i >= 0; --i) {
        ushort u = s.at(i).unicode();
        if ((u < 0x20) || (u == 0x7F)) s.remove(i, 1);
      }
      s = s.trimmed();
      if (s.startsWith("!") || s.startsWith("@"))
        s = s.mid(1).trimmed();

      const QStringList cols = s.split(QRegularExpression("\\s+"), SkipEmptyParts);
      if (cols.size() < 6) return;

      // JT65 flag for addDecodeLabel:
      //  - JT65 lines contain " #"
      //  - Q65 lines contain ':' in the header part
      bool is_jt65 = s.contains(" #") && !s.contains(" : ");

      // Frequency calculation (kHz, relative to the waterfall baseband)
      double freq_khz = -1.0;

      if (is_main_jt65) {
        // Main JT65 decode: "! nkHz ndf ..."
        // nkHz: nominal JT65 sub-band in kHz (0..1000)
        // ndf : fine frequency offset in Hz
        bool ok1=false, ok2=false;
        int nkHz = cols[0].toInt(&ok1);
        int ndf  = cols[1].toInt(&ok2);
        if (!ok1 || !ok2) return;

        freq_khz = nkHz + ndf / 1000.0;

        // Adaptive polarization uses a wrapped frequency scale from -500 to +500 kHz.
        // MAP65 reports nkHz in 0..1000, so wrap values past +500 down by 1000.
        // Wrap the SUM, not the bare nkHz: at nkHz=500 with a positive ndf the
        // total sits above +500 and must wrap, which the Q65 branch below and
        // relayClickToWsjtx() already do -- this branch was the odd one out.
        if (m_xpol && freq_khz > 500.0) freq_khz -= 1000.0;

      } else if (is_q65) {
        // Q65 (and MAP65-style frequency header lines):
        // "@144.124 -270 0 0000 ..."
        //  - f_mhz : absolute RF frequency in MHz (e.g. 144.124)
        //  - ndf_hz: fine offset in Hz (e.g. -270)
        // We convert this to a local kHz offset for the waterfall.
        QString raw = t2.mid(1).trimmed();   // "144.124 -270 0 0000 ..."
        int sp = raw.indexOf(' ');
        if (sp < 0) return;
        QString fstr = raw.left(sp);               // "144.124"
        QString rest = raw.mid(sp + 1).trimmed();  // "-270 0 0000 ..."
        int sp2 = rest.indexOf(' ');
        if (sp2 < 0) return;
        QString ndf_str = rest.left(sp2);          // "-270"
        fstr.replace(",", ".");
        ndf_str.replace(",", ".");
        bool ok1=false, ok2=false;
        double f_mhz  = QLocale::c().toDouble(fstr, &ok1);
        double ndf_hz = QLocale::c().toDouble(ndf_str, &ok2);
        if (!ok1 || !ok2) return;
        double frac_mhz = f_mhz - std::floor(f_mhz);   // e.g. 144.124 -> 0.124
        freq_khz = frac_mhz * 1000.0 + (ndf_hz / 1000.0);

        // Adaptive polarization uses a wrapped frequency scale from -500 to +500 kHz.
        // Some adaptive JT65 lines are routed through the '@' header path (is_q65),
        // so the wrap logic must exist in both main JT65 and Q65 blocks.
        if (m_xpol && freq_khz > 500.0) freq_khz -= 1000.0;
      }

      // Extract message body (calls + grid + text)
      QString body;

      int sep_q65  = s.indexOf(':');
      int sep_main_jt65 = s.mid(30,4).indexOf(" # ");
      int sep_msg_jt65 = s.indexOf("#");

      if (sep_q65 >= 0) {
        // Q65: everything left of ':' is the interesting part (decoded message)
        QString left = s.left(sep_q65).trimmed();
        QStringList leftCols = left.split(QRegularExpression("\\s+"), SkipEmptyParts);
        if (leftCols.size() > 5) {
          QStringList msg;
          for (int i = 5; i < leftCols.size(); ++i) msg << leftCols[i];
          body = msg.join(" ");
        } else {
          body = left;
        }

      } else if (sep_msg_jt65 >= 0 && !is_main_jt65) {
        // Lines with "#" that are not main JT65 decodes:
        // either main-window JT65 (with "#" before calls) or messages-window JT65 (#, #H, #V)

        // Find the column that contains the "#" marker
        int hashCol = -1;
        for (int i = 0; i < cols.size(); ++i) {
          if (cols[i].startsWith("#")) {
            hashCol = i;
            break;
          }
        }

        if (hashCol >= 0 && hashCol + 1 < cols.size()) {
          QString afterHash = cols[hashCol + 1];

          static const QRegularExpression call_re("^([A-Z0-9]{1,4}/)?[A-Z0-9]{1,3}[0-9][A-Z0-9]{0,3}[A-Z](/[A-Z0-9]+)?$");

          if (call_re.match(afterHash).hasMatch()) {
            // JT65 line from main window: callsign directly after "#"
            // Example: 0  -1  86  1804  2.5  -16 # W1XAA K2XBB EM10 ...
            QStringList msg;
            for (int i = hashCol + 1; i < cols.size(); ++i)
              msg << cols[i];
            body = msg.join(" ");
          } else {
            // JT65 message line from messages window (with "#", "#H", "#V")
            // Example: 980  1   1 1804  -16  W1AAA K2BBB EM00  #H  2.5  0#B
            QStringList msg;
            // Calls + grid are always in columns 5,6,7 for MAP65 JT65 messages
            for (int i = 5; i <= 7 && i < cols.size(); ++i)
              msg << cols[i];
            body = msg.join(" ");
          }
        } else {
          // Fallback: treat as main-window style, message right of "#"
          body = s.mid(sep_msg_jt65 + 3).trimmed();
        }

      } else if (sep_main_jt65 >= 0 && !m_xpol) {
        // Main JT65 decode: everything right of "#..."
        body = s.mid(sep_main_jt65 + 3).trimmed();

      } else if (s.size() > 31) {
        // Generic fallback: skip fixed-width header, take the tail as message
        body = s.mid(31).trimmed();

      } else {
        body = s;
      }

      // Split message into fields
      QStringList msg_cols = body.split(QRegularExpression("\\s+"), SkipEmptyParts);

      // Callsign + grid + CQ
      QString sender;
      QString grid;
      bool is_cq = false;
      static const QRegularExpression call_re("^([A-Z0-9]{1,4}/)?[A-Z0-9]{1,3}[0-9][A-Z0-9]{0,3}[A-Z](/[A-Z0-9]+)?$");

      if (msg_cols.size() >= 2) {
        if (msg_cols[0] == "CQ") {
          is_cq = true;
          int senderIdx = 1;
          if (msg_cols.size() >= 3 && msg_cols[1] == "DX") senderIdx = 2;
          if (senderIdx < msg_cols.size()) {
            sender = msg_cols[senderIdx].toUpper();
            if (msg_cols.size() > senderIdx + 1) {
              QString g = msg_cols[senderIdx + 1].toUpper();
              if (isGrid4(g)) grid = g;
            }
          }
        } else {
          QString cand = msg_cols[1].toUpper();
          if (call_re.match(cand).hasMatch()) {
            sender = cand;
            if (msg_cols.size() > 2) {
              QString g = msg_cols[2].toUpper();
              if (isGrid4(g)) grid = g;
            }
          }
        }
      }

      if (sender.isEmpty()) return;

      bool is_active = (sender == ui->dxCallEntry->text());

      QString key = QString("%1:%2").arg(freq_khz).arg(sender);
      if (m_seenLabels.contains(key) && !is_cq) return;
      m_seenLabels.insert(key);

      if (m_wide_graph_window) {
        m_wide_graph_window->addDecodeLabel(
            freq_khz, sender,
            is_jt65,
            true,
            true,
            is_cq,
            true,
            is_active,
            grid);
      }
    }
  }
}

void MainWindow::onRunM65Finished() {
    // Do something when run_m65 finishes
  //   qDebug() << "DECODE: onRunM65Finished()";
}

void MainWindow::onDiskDecodeFinished()
{
    if (!m_path.isEmpty())
        setWindowTitle("EME65  -  " + QFileInfo(m_path).fileName());

    // Past-period decode: once the replayed period is fully decoded,
    // leave disk mode and resume Monitor if it was on before the replay.
    // (This slot also fires when getfile()'s read worker completes, while
    // the decoder is still busy -- resume only on the final call.)
    if (m_pastReplayActive && !m_decoderBusy) {
        m_pastReplayActive=false;
        m_diskData=false;
        if (m_wasMonitoring) {
            m_monitoring=true;
            soundInThread.setMonitoring(m_monitoring);
        }
    }

    if (m_loopall)
        on_actionOpen_next_in_directory_triggered();
}

// Snapshot which satellite windows are open. Called from closeEvent()
// before the windows are close()d, and lazily from writeSettings() for
// mid-session saves.
void MainWindow::captureWindowOpenStates()
{
  m_astroOpen = m_astro_window && m_astro_window->isVisible();
  m_wideGraphOpen = m_wide_graph_window && m_wide_graph_window->isVisible();
  m_bandMapOpen = m_band_map_window && m_band_map_window->isVisible();
  m_messagesOpen = m_messages_window && m_messages_window->isVisible();
}

//-------------------------------------------------------- writeSettings()
void MainWindow::writeSettings()
{
  QSettings settings(m_settings_filename, QSettings::IniFormat);
  {
    SettingsGroup g {&settings, "MainWindow"};
    settings.setValue("geometry", saveGeometry());
    settings.setValue("MRUdir", m_path);
    settings.setValue("TxFirst",m_txFirst);
    settings.setValue("DXcall",ui->dxCallEntry->text());
    settings.setValue("DXgrid",ui->dxGridEntry->text());
    if (!m_windowOpenStatesCaptured) captureWindowOpenStates();
    settings.setValue("AstroOpen",m_astroOpen);
    settings.setValue("WideGraphOpen",m_wideGraphOpen);
    settings.setValue("BandMapOpen",m_bandMapOpen);
    settings.setValue("MessagesOpen",m_messagesOpen);
  }

  {
  SettingsGroup g {&settings, "Common"};
  settings.setValue("MyCall",m_myCall);
  settings.setValue("MyGrid",m_myGrid);
  settings.setValue("InstanceId",m_instanceId);
  settings.setValue("SyncWSJTX",m_syncWsjtx);
  settings.setValue("TxViaWSJTX",m_txViaWsjtx);
  settings.setValue("ContinuousWaterfall",ui->actionContinuous_waterfall->isChecked());
  settings.setValue("AddCallToSH",m_addCallToSh);
  settings.setValue("IDint",m_idInt);
  settings.setValue("PTTpath",m_pttPath);
  settings.setValue("PTTPortNumber",m_pttPortNumber);
  settings.setValue("AstroFont",m_astroFont);
  settings.setValue("Xpol",m_xpol);
  settings.setValue("XpolX",m_xpolx);
  settings.setValue("SaveDir",m_saveDir);
  settings.setValue("AzElDir",m_azelDir);
  settings.setValue("Call3Path",m_call3PathUser);
  settings.setValue("Editor",m_editorCommand);
  settings.setValue("DXCCpfx",m_dxccPfx);
  settings.setValue("Timeout",m_timeout);
  settings.setValue("ApplyIQcal",m_applyIQcal);
  settings.setValue("dPhi",m_dPhi);
  settings.setValue("Fcal",m_fCal);
  settings.setValue("Fadd",m_fAdd);
  settings.setValue("NetworkInput", m_network);
  settings.setValue("SampleRateHz",
                    m_fs96000 == 2 ? 192000 : (m_fs96000 == 0 ? 95238 : 96000));
  settings.setValue("SoundInIndex",m_nDevIn);
  settings.setValue("paInDevice",m_paInDevice);
  settings.setValue("SoundOutIndex",m_nDevOut);
  settings.setValue("paOutDevice",m_paOutDevice);
  settings.setValue("IQswap",m_IQswap);
  settings.setValue("Scale_dB",m_dB);
  settings.setValue("IQxt",m_bIQxt);
  settings.setValue("InitIQplus",m_initIQplus);
  settings.setValue("UDPport",m_udpPort);
  settings.setValue("PaletteCuteSDR",ui->actionCuteSDR->isChecked());
  settings.setValue("PaletteLinrad",ui->actionLinrad->isChecked());
  settings.setValue("PaletteAFMHot",ui->actionAFMHot->isChecked());
  settings.setValue("PaletteBlue",ui->actionBlue->isChecked());
  settings.setValue("Mode",m_mode);
  settings.setValue("nModeJT65",m_modeJT65);
  settings.setValue("nModeQ65",m_modeQ65);
  settings.setValue("TxMode",m_modeTx);
  settings.setValue("SaveNone",ui->actionNone->isChecked());
  settings.setValue("SaveAll",ui->actionSave_all->isChecked());
  settings.setValue("KeepLastPeriod",ui->actionKeep_last_period->isChecked());
  settings.setValue("PastPeriodsOnWideGraph",ui->actionPast_periods_on_Wide_Graph->isChecked());
  settings.setValue("ReduceFalseDecodes",ui->actionReduce_false_decodes->isChecked());
  settings.setValue("NDepth",m_ndepth);
  settings.setValue("Q65Depth",m_q65depth);
  settings.setValue("NEME",m_onlyEME);
  settings.setValue("ClickToWork",ui->actionEnable_Click_to_Work->isChecked());
  settings.setValue("KB8RQ",m_kb8rq);
  settings.setValue("NB",m_NB);
  settings.setValue("NBslider",m_NBslider);
  settings.setValue("GainX",(double)m_gainx);
  settings.setValue("GainY",(double)m_gainy);
  settings.setValue("PhaseX",(double)m_phasex);
  settings.setValue("PhaseY",(double)m_phasey);
  settings.setValue("Mult570",m_mult570);
  settings.setValue("Mult570Tx",m_mult570Tx);
  settings.setValue("Cal570",m_cal570);
  settings.setValue("TxOffset",m_TxOffset);
  settings.setValue("Colors",m_colors);
  settings.setValue("ColorPreset",m_colorPreset);
  settings.setValue("MaxDrift",ui->sbMaxDrift->value());
  settings.setValue("LiveCQEnabled",m_livecqEnabled);        //liveCQ
  settings.setValue("LiveCQDestOfficial",m_livecqOfficial);  //liveCQ
  settings.setValue("LiveCQDestN6NU",m_livecqN6NU);          //liveCQ
  settings.setValue("LiveCQDestCustom",m_livecqCustom);      //liveCQ
  settings.setValue("LiveCQW3SZAppId",m_livecqW3szAppId);    //liveCQ
  settings.setValue("otherUrl",m_otherUrl);                  //liveCQ
  settings.setValue("spotPSK",m_spot_to_psk_reporter);
  settings.setValue("PSKReporterTCPIP",m_psk_reporter_tcpip);
  settings.setValue("FTol",m_tol);
	settings.endGroup();
  }

  {
	settings.beginGroup("TxTune");
	settings.setValue("geometry", g_TxTuneGeometry);
	settings.setValue("TxPower",txPower);
	settings.setValue("IQamp",iqAmp);
	settings.setValue("IQphase",iqPhase);
	settings.endGroup();  
  }
}

//---------------------------------------------------------- readSettings()
void MainWindow::readSettings()
{
  QSettings settings(m_settings_filename, QSettings::IniFormat);
  {
    SettingsGroup g {&settings, "MainWindow"};
    restoreGeometry(settings.value("geometry").toByteArray());
    ui->dxCallEntry->setText(settings.value("DXcall","").toString());
    ui->dxGridEntry->setText(settings.value("DXgrid","").toString());
    m_path = settings.value("MRUdir", m_dataDir + "/save").toString();
    m_txFirst = settings.value("TxFirst",false).toBool();
    ui->txFirstCheckBox->setChecked(m_txFirst);
    m_astroOpen = settings.value("AstroOpen",true).toBool();
    m_wideGraphOpen = settings.value("WideGraphOpen",true).toBool();
    m_bandMapOpen = settings.value("BandMapOpen",true).toBool();
    m_messagesOpen = settings.value("MessagesOpen",true).toBool();
  }

  {
  SettingsGroup g {&settings, "Common"};
  m_myCall=settings.value("MyCall","").toString();
  m_myGrid=settings.value("MyGrid","").toString();
  m_instanceId=settings.value("InstanceId",1).toInt();
  if (m_instanceId < 1 || m_instanceId > 4) m_instanceId = 1;
  m_syncWsjtx=settings.value("SyncWSJTX",false).toBool();
  m_txViaWsjtx=settings.value("TxViaWSJTX",false).toBool();
  ui->actionContinuous_waterfall->setChecked(settings.value("ContinuousWaterfall",false).toBool());
  // Deliberately NOT read from the retired "FwdShorthand" key: its semantics
  // were forward-or-not, this one is attach-call-or-not. A stale saved value
  // must not resurrect the old gating.
  m_addCallToSh=settings.value("AddCallToSH",false).toBool();
  m_idInt=settings.value("IDint",0).toInt();
  m_pttPath=settings.value("PTTpath",0).toString();
  m_pttPortNumber = settings.value("PTTPortNumber",0).toInt();
  #if !defined(Q_OS_WIN)
    ptt_set_override(m_pttPath.toUtf8().constData());
  #endif
  m_astroFont=settings.value("AstroFont",20).toInt();
  m_xpol=settings.value("Xpol",false).toBool();
  ui->actionFind_Delta_Phi->setEnabled(m_xpol);
  m_xpolx=settings.value("XpolX",false).toBool();
  m_saveDir=settings.value("SaveDir",m_dataDir + "/save").toString();
  m_azelDir=settings.value("AzElDir",m_dataDir).toString();
  m_call3PathUser=settings.value("Call3Path","").toString();
  m_editorCommand=settings.value("Editor","notepad").toString();
  m_dxccPfx=settings.value("DXCCpfx","").toString();
  m_timeout=settings.value("Timeout",20).toInt();
  m_applyIQcal=settings.value("ApplyIQcal",0).toInt();
  ui->actionApply_IQ_Calibration->setChecked(m_applyIQcal!=0);
  m_dPhi=settings.value("dPhi",0).toInt();
  m_fCal=settings.value("Fcal",0).toInt();
  m_fAdd=settings.value("Fadd",0).toDouble();
  soundInThread.setFadd(m_fAdd);
  m_network = settings.value("NetworkInput",true).toBool();
  const int rateHz = settings.value("SampleRateHz", 1).toInt();
  if (rateHz == 192000)     m_fs96000 = 2;
  else if (rateHz == 96000) m_fs96000 = 1;
  else if (rateHz == 95238) m_fs96000 = 0;
  else {
    bool fs96000 = settings.value("FSam96000",true).toBool();
    bool fs192000 = settings.value("FSam192000",false).toBool();
    m_fs96000 = fs192000 ? 2 : (fs96000 ? 1 : 0);
  }
  m_nDevIn = settings.value("SoundInIndex", 0).toInt();
  m_paInDevice = settings.value("paInDevice",0).toInt();
  m_nDevOut = settings.value("SoundOutIndex", 0).toInt();
  m_paOutDevice = settings.value("paOutDevice",0).toInt();
  m_IQswap = settings.value("IQswap",false).toBool();
  m_dB = settings.value("Scale_dB",0).toInt();
  m_initIQplus = settings.value("InitIQplus",false).toBool();
  m_bIQxt = settings.value("IQxt",false).toBool();
  m_udpPort = settings.value("UDPport",50004).toInt();
  soundInThread.setSwapIQ(m_IQswap);
  soundInThread.setScale(m_dB);
  soundInThread.setPort(m_udpPort);
  ui->actionCuteSDR->setChecked(settings.value(
                                  "PaletteCuteSDR",true).toBool());
  ui->actionLinrad->setChecked(settings.value(
                                 "PaletteLinrad",false).toBool());
  m_mode=settings.value("Mode","JT65B").toString();
  m_modeJT65=settings.value("nModeJT65",2).toInt();
  if(m_modeJT65==0) ui->actionNoJT65->setChecked(true);
  if(m_modeJT65==1) ui->actionJT65A->setChecked(true);
  if(m_modeJT65==2) ui->actionJT65B->setChecked(true);
  if(m_modeJT65==3) ui->actionJT65C->setChecked(true);

  m_modeQ65=settings.value("nModeQ65",2).toInt();
  m_modeTx=settings.value("TxMode","JT65").toString();
  if(m_modeQ65==0) ui->actionNoQ65->setChecked(true);
  if(m_modeQ65==1) ui->actionQ65A->setChecked(true);
  if(m_modeQ65==2) ui->actionQ65B->setChecked(true);
  if(m_modeQ65==3) ui->actionQ65C->setChecked(true);
  if(m_modeQ65==4) ui->actionQ65D->setChecked(true);
  if(m_modeQ65==5) ui->actionQ65E->setChecked(true);
  if(m_modeTx=="JT65")  ui->pbTxMode->setText("Tx JT65   #");
  if(m_modeTx=="Q65") ui->pbTxMode->setText("Tx Q65  :");

  ui->actionNone->setChecked(settings.value("SaveNone",true).toBool());
  ui->actionSave_all->setChecked(settings.value("SaveAll",false).toBool());
  m_saveAll=ui->actionSave_all->isChecked();
  ui->actionKeep_last_period->setChecked(settings.value("KeepLastPeriod",false).toBool());
  ui->actionPast_periods_on_Wide_Graph->setChecked(settings.value("PastPeriodsOnWideGraph",false).toBool());
  ui->actionReduce_false_decodes->setChecked(settings.value("ReduceFalseDecodes",false).toBool());
  m_ndepth=settings.value("NDepth",0).toInt();
  m_q65depth=settings.value("Q65Depth",3).toInt();
  m_onlyEME=settings.value("NEME",false).toBool();
  ui->actionOnly_EME_calls->setChecked(m_onlyEME);
  ui->actionEnable_Click_to_Work->setChecked(settings.value("ClickToWork",false).toBool());
  m_kb8rq=settings.value("KB8RQ",false).toBool();
  ui->actionF4_sets_Tx6->setChecked(m_kb8rq);
  m_NB=settings.value("NB",false).toBool();
  ui->NBcheckBox->setChecked(m_NB);
  ui->sbMaxDrift->setValue(settings.value("MaxDrift",0).toInt());
  m_NBslider=settings.value("NBslider",40).toInt();
  ui->NBslider->setValue(m_NBslider);
  m_gainx=settings.value("GainX",1.0).toFloat();
  m_gainy=settings.value("GainY",1.0).toFloat();
  m_phasex=settings.value("PhaseX",0.0).toFloat();
  m_phasey=settings.value("PhaseY",0.0).toFloat();
  m_mult570=settings.value("Mult570",2).toInt();
  m_mult570Tx=settings.value("Mult570Tx",1).toInt();
  m_cal570=settings.value("Cal570",0.0).toDouble();
  m_TxOffset=settings.value("TxOffset",130.9).toDouble();
  m_colors=settings.value("Colors","000080ffffffc8c8e68c8cb45a5a82").toString();
  m_colorPreset=settings.value("ColorPreset","White on Blue").toString();
  // A named preset is authoritative for its colours: re-derive m_colors from
  // the preset name so a stale/legacy Colors string (e.g. an old default
  // saved before the preset system existed) can't leave bandmap/Messages text
  // in the wrong scheme. Must match DevSetup::applyColorPreset. "Custom"
  // keeps whatever colours were saved.
  if      (m_colorPreset == "Classic")        m_colors = "000066ff0000ffff00969696646464";
  else if (m_colorPreset == "Black on white") m_colors = "ffffff000000404040808080b4b4b4";
  else if (m_colorPreset == "White on Blue")  m_colors = "000080ffffffc8c8e68c8cb45a5a82";

  if(!ui->actionLinrad->isChecked() && !ui->actionCuteSDR->isChecked() &&
    !ui->actionAFMHot->isChecked() && !ui->actionBlue->isChecked()) {
    on_actionLinrad_triggered();
    ui->actionLinrad->setChecked(true);
  }
  if(m_ndepth==0) ui->actionNo_Deep_Search->setChecked(true);
  if(m_ndepth==1) ui->actionNormal_Deep_Search->setChecked(true);
  if(m_ndepth==2) ui->actionAggressive_Deep_Search->setChecked(true);
  if(m_q65depth==1) ui->actionQ65_Fast->setChecked(true);
  if(m_q65depth==2) ui->actionQ65_Normal->setChecked(true);
  if(m_q65depth==3) ui->actionQ65_Deep->setChecked(true);
  // LiveCQ destinations are independent checkboxes (matches WSJT-X
  // 2026-08-12). A new install starts with W3SZ (w3sz.com) off and N6NU
  // on (Andreas, 2026-09-12); an existing ini keeps the operator's choice.
  m_livecqEnabled=settings.value("LiveCQEnabled",true).toBool();
  m_otherUrl=settings.value("otherUrl","").toString();
  // Program name reported to w3sz.com: MAP65, its tag there before the
  // re-branding, or EME65. N6NU and Custom always receive EME65.
  m_livecqW3szAppId=settings.value("LiveCQW3SZAppId","MAP65").toString();
  if(m_livecqW3szAppId!="EME65") m_livecqW3szAppId="MAP65";
  // Carry an older ini over. Before the checkboxes there was one destination
  // at a time: "livecqUrlSel" 0/1/2 = Official/N6NU/Other, and before that a
  // plain "w3szUrl" bool with false meaning Other. Without this an operator
  // who had picked Other -- a third-party LiveCQ such as EA8DBM's -- came up
  // after the upgrade with the URL still in the file but the destination
  // switched off, and no indication why the spots had stopped (G4SWX, 2026-08-15).
  if(!settings.contains("LiveCQDestN6NU") &&
     (settings.contains("livecqUrlSel") || settings.contains("w3szUrl"))) {
    bool const w3sz=settings.value("w3szUrl",true).toBool();
    int const sel=settings.value("livecqUrlSel",w3sz ? 0 : 2).toInt();
    m_livecqOfficial=(sel==0);
    m_livecqN6NU=(sel==1);
    m_livecqCustom=(sel==2);
  } else {
    m_livecqOfficial=settings.value("LiveCQDestOfficial",false).toBool();
    m_livecqN6NU=settings.value("LiveCQDestN6NU",true).toBool();
    m_livecqCustom=settings.value("LiveCQDestCustom",false).toBool();
  }
  m_spot_to_psk_reporter=settings.value("spotPSK",true).toBool();
  m_psk_reporter_tcpip=settings.value("PSKReporterTCPIP",false).toBool();

  m_tol=settings.value("FTol",500).toInt();
  m_wide_graph_window->setTol(m_tol);
  int i = 5;
  if(m_tol==20) i=1;
  if(m_tol==50) i=2;
  if(m_tol==100) i=3;
  if(m_tol==200) i=4;
//  if(m_tol==500) i=5;
  if(m_tol==1000) i=6;
  ui->labTol1->setText(QString::number(m_tol));
  ui->tolSpinBox->setValue(i);

  qDebug() << "In mainwindow m_spot_to_psk_reporter is: " << m_spot_to_psk_reporter;

  qDebug() << "In mainwindow m_modeTx is: " << m_modeTx;
  qDebug() << "In mainwindow m_mode is: " << m_mode;
  qDebug() << "In mainwindow n_modeJT65 is: " << m_modeJT65;
  qDebug() << "In mainwindow n_modeQ65 is: " << m_modeQ65;
	settings.endGroup();
  }
  
  {
	settings.beginGroup("TxTune");
	g_TxTuneGeometry = settings.value("geometry").toByteArray();
	txPower=settings.value("TxPower",100).toInt();
	iqAmp=settings.value("IQamp",0).toInt();
	iqPhase=settings.value("IQphase",0).toInt();
	settings.endGroup();
  }
}

//-------------------------------------------------------------- dataSink()
void MainWindow::dataSink(int k)
{
  static float* s = nullptr;
  static float* splot = nullptr;
  if (!s) {
      s     = new float[active_nfft];
      splot = new float[active_nfft];
  }
  static int n=0;
  static int ihsym=0;
  static int nzap=0;
  static int ntrz=0;
  static int nkhz;
  int nfsample = 96000; 
  if (m_fs96000 == 0) nfsample = 95238;
  else if(m_fs96000 == 2) nfsample = 192000;
  setNfsample(nfsample);
  int nxpol= m_xpol ? 1 : 0;
  setNxpol(nxpol);
  static int nsec0=0;
  static int nsum=0;
  static int ndiskdat;
  static int nb;
  static int nadj=0;
  static float px=0.0,py=0.0;
  static uchar lstrong[1024];
  static float rejectx;
  static float rejecty;
  static float slimit;
  static double xsum=0.0;
  
  if(m_diskData) {
    ndiskdat=1;
      setNdiskdat(1);
  } else {
    ndiskdat=0;
      setNdiskdat(0);
  }

  setIdphi(m_dPhi);
// Get x and y power, polarized spectrum, nkhz, and ihsym
  nb=0;
  if(m_NB) nb=1;
  nfsample=96000;
  if(m_fs96000 == 0) nfsample=95238;
  else if (m_fs96000 == 2) nfsample=192000;
  nxpol=0;
  if(m_xpol) nxpol=1;
  nadj++;
  if(m_adjustIQ==0) nadj=0;
    
  // Samples that arrived while WSJT-X was keyed carry our own transmitter,
  // not signals: zero them before the spectrum and the decoder see them
  // (QMAP: zaptx). k0 remembers where the previous call left off.
  static int k0=0;
  if(rxMutedByWsjtx() && !m_diskData) zapWsjtxTx(k0, k);
  k0=k;

  symspec_(&k, &nxpol, &ndiskdat, &nb, &m_NBslider, &m_dPhi,
           &nfsample, &m_adjustIQ, &m_applyIQcal,
           &m_gainx, &m_gainy, &m_phasex, &m_phasey, &rejectx, &rejecty,
           &px, &py, s, &nkhz, &ihsym, &nzap, &slimit, lstrong);

  int nsec=QDateTime::currentSecsSinceEpoch();
  if(nsec==nsec0) {
    xsum+=pow(10.0,0.1*px);
    nsum+=1;
  } else {
    m_xavg=0.0;
    if(nsum>0) m_xavg=xsum/nsum;
    xsum=pow(10.0,0.1*px);
    nsum=1;
  }
  nsec0=nsec;

  if(rxMutedByWsjtx() && !m_diskData) { px=0.0; py=0.0; }   // meters read nothing while keyed

  QString t;
  m_pctZap=nzap/178.3;
  ui->yMeterFrame->setVisible(m_xpol);
  if(m_xpol) {
    lab4->setText (
                  QString {" Rx noise: %1  %2 %3 %% "}
                     .arg (px, 5, 'f', 1)
                     .arg (py, 5, 'f', 1)
                     .arg (m_pctZap, 5, 'f', 1)
                  );
  } else {
    lab4->setText (
                  QString {" Rx noise: %1  %2 %% "}
                  .arg (px, 5, 'f', 1)
                  .arg (m_pctZap, 5, 'f', 1)
                  );
  }
  xSignalMeter->setValue(px);                   // Update the signal meters
  ySignalMeter->setValue(py);

  //Hold the waterfall while WSJT-X transmits (unless Continuous Waterfall)
  if((m_monitoring && !rxMutedByWsjtx()) || m_diskData) {
    m_wide_graph_window->dataSink2(s,nkhz,ihsym,m_diskData,lstrong);
  }

  if(nadj == 10) {
    if(m_xpol) {
      ui->decodedTextBrowser->append (
                                      QString {"Amp: %1 %2   Phase: %3 %4"}
                                         .arg (m_gainx, 6, 'f', 4).arg (m_gainy, 6, 'f', 4)
                                         .arg (m_phasex, 6, 'f', 4)
                                         .arg (m_phasey, 6, 'f', 4)
                                      );
    } else {
      ui->decodedTextBrowser->append(
                                     QString {"Amp: %1   Phase: %2"}
                                        .arg (m_gainx, 6, 'f', 4)
                                        .arg (m_phasex, 6, 'f', 4)
                                     );
    }
    ui->decodedTextBrowser->append(t);
    m_adjustIQ=0;
  }

  //Average over specified number of spectra
  if (n==0) {
    for (int i=0; i<active_nfft; i++)
      splot[i]=s[i];
  } else {
    for (int i=0; i<active_nfft; i++)
      splot[i] += s[i];
  }
  n++;

  if (n>=m_waterfallAvg) {
    for (int i=0; i<active_nfft; i++) {
        splot[i] /= n;                           //Normalize the average
    }

// Time according to this computer
    qint64 ms = QDateTime::currentMSecsSinceEpoch() % 86400000;
    int ntr1 = (ms/1000) % m_TRperiod;
    if((m_diskData && ihsym <= m_waterfallAvg) || (!m_diskData && ntr1<ntrz)) {
      for (int i=0; i<active_nfft; i++) {
        splot[i] = 1.e30;
      }
      // Past-period decode: a NEW live period has started arriving --
      // reset the armed Decode button back to normal (the operator's
      // attention window has moved on; the ring may also be about to
      // rotate the armed file away).
      if(!m_diskData and m_pastPeriods and !m_pastArmedPath.isEmpty()
         and !m_pastReplayActive) {
        disarmPastPeriod();
      }
    }
    ntrz=ntr1;
    n=0;
  }

  if(ihsym<280) m_RxState=0;

  if(m_RxState==0 and ihsym>=280 and !m_diskData) {   //Early decode, t=52 s
    m_RxState=1;
    setDecoderReady(0);
    setNewdat(1);
    setNagain(0);
    setNhsym(ihsym);
    QDateTime t = QDateTime::currentDateTimeUtc();
    m_dateTime=t.toString("yyyy-MMM-dd hh:mm");
    if (m_wide_graph_window && ui->actionShow_callsigns_on_Waterfall->isChecked()) m_seenLabels.clear();  // Reset m_seenLabels every new period
    decode();                                           //Start the decoder
  //  qDebug() << "decoding at t=52s in dataSink in mainwindow.cpp";
  }

  if(m_RxState<=1 and ihsym>=302) {   //Decode at t=56 s (for Q65 and data from disk)
    m_RxState=2;
    setDecoderReady(0);
    setNewdat(1);
    setNagain(0);
    setNhsym(ihsym);
    QDateTime t = QDateTime::currentDateTimeUtc();
    m_dateTime=t.toString("yyyy-MMM-dd hh:mm");
    if (m_wide_graph_window && ui->actionShow_callsigns_on_Waterfall->isChecked()) m_seenLabels.clear();  // Reset m_seenLabels every new period
    decode();                                           //Start the decoder
    if(m_saveAll and !m_diskData) {
      QString fname=m_saveDir + "/" + t.date().toString("yyMMdd") + "_" +
          t.time().toString("hhmm");
      if(m_xpol) fname += ".tf2";
      if(!m_xpol) fname += ".iq";
      *future2 = QtConcurrent::run([this](QString fname, bool xpol) {
        this->savetf2(fname, xpol);
        }, fname, m_xpol);        

      qDebug() << "saving to file " << fname << " in dataSink in mainwindow.cpp";
      watcher2->setFuture(*future2);
    }
    // Past-period decode: keep a 3-deep rotating ring of raw periods in
    // save/ring/ while the feature is enabled, independent of Save All
    // (own subdir, so rotation can never delete a user's own captures).
    if(m_pastPeriods and !m_diskData) {
      QString ringDir=m_saveDir + "/ring";
      QString tag=t.date().toString("yyMMdd") + "_" + t.time().toString("hhmm");
      QString fnameR=ringDir + "/" + tag + (m_xpol ? ".tf2" : ".iq");
      bool xpolR=m_xpol;
      QtConcurrent::run([this,fnameR,ringDir,xpolR]() {
        QDir().mkpath(ringDir);
        this->savetf2(fnameR, xpolR);
        QDir d(ringDir);
        QStringList files=d.entryList(QStringList() << "*.iq" << "*.tf2",
                                      QDir::Files, QDir::Name);
        while(files.size()>3) {          // rotate: keep the 3 newest
          d.remove(files.first());
          files.removeFirst();
        }
      });
    }
  }
  soundInThread.m_dataSinkBusy=false;
}

/* Generate gaussian random float with mean=0 and std_dev=1 */
float gran()
{
  float fac,rsq,v1,v2;
  static float gset;
  static int iset;

  if(iset){
    /* Already got one */
    iset = 0;
    return gset;
  }
  /* Generate two evenly distributed numbers between -1 and +1
   * that are inside the unit circle
   */
  do {
    v1 = 2.0 * (float)rand() / RAND_MAX - 1;
    v2 = 2.0 * (float)rand() / RAND_MAX - 1;
    rsq = v1*v1 + v2*v2;
  } while(rsq >= 1.0 || rsq == 0.0);
  fac = sqrt(-2.0*log(rsq)/rsq);
  gset = v1*fac;
  iset++;
  return v2*fac;
}

float* MainWindow::getDd() const
{
    return dd;
}

void MainWindow::savetf2(QString fname, bool xpol)
{
  int npts=2*56*g_sampleRate;
  if(xpol) npts=2*npts;
  FILE* fp = fopen(fname.toUtf8().constData(), "wb");

  if (!fp) {
    qDebug() << "FAILED TO OPEN FILE:" << fname;
    perror("fopen");
    return;
  }

  qint16* buf = static_cast<qint16*>(malloc(npts * sizeof(*buf)));

  if(fp != NULL) {
    double fcenter = getFcenter();
      //fcenter = 1296.125;
    fwrite(&fcenter, sizeof(fcenter), 1, fp);  // Write fcenter to file
    uint64_t zero = 0;
    fwrite(&zero, sizeof(zero), 1, fp);         // another 8 bytes
    qDebug() << "in savetf2 fcenter is: " << fcenter;

    int j = 0;
    for (int i = 0; i < npts; i += 2) {
      buf[i]     = static_cast<qint16>(dd[j++]);
      buf[i + 1] = static_cast<qint16>(dd[j++]);
      if (!xpol) j += 2;  // Skip over dd(3,x) and dd(4,x)
    }

    fwrite(buf, sizeof(buf[0]), npts, fp);
    qDebug() << "saving file " << fname << " to disk in savetf2 in mainwindow.cpp";
    fclose(fp);
  }
  free(buf);
}

void MainWindow::getfile(QString fname, bool xpol, int dbDgrd)
{
    setDecoderReady(0);
    int npts = 2 * 56 * g_sampleRate;
    if (xpol) npts = 2 * npts;

    // j indexes dd[], which has 4 channels per pair
    int j = 0;

    // Clear id[] properly
    memset(id.data(), 0, npts * sizeof(id[0]));

    // Open file
    FILE* fp = fopen(fname.toUtf8().constData(), "rb");
    if (!fp) {
        qWarning() << "Failed to open file:" << fname;
        return;
    }

  qDebug().noquote() << QString("NEWFILE: nfsample=%1 nrate_active=%2")
                      .arg(active_nfft)
                      .arg(g_sampleRate);
  

    // Read fcenter
    double fcenter = 0.0;
    if (fread(&fcenter, sizeof(fcenter), 1, fp) != 1) {
		fclose(fp);
		return;
	}

    setFcenter(fcenter);

    // Skip the 8-byte zero padding
    uint64_t pad = 0;
    if (fread(&pad, sizeof(pad), 1, fp) != 1) {
		fclose(fp);
		return;
	}


    // Read raw samples
    size_t nRead = fread(id.data(), sizeof(id[0]), npts, fp);
    qDebug() << "fread read" << nRead << "samples, expected" << npts;

    fclose(fp);
    
    // Compute degradation factors
    float dgrd = 0.0;
    if (dbDgrd < 0)
        dgrd = 23.0 * sqrt(pow(10.0, -0.1 * (double)dbDgrd) - 1.0);

    float fac = 23.0 / sqrt(dgrd * dgrd + 23.0 * 23.0);

    // Fill dd[]
    j = 0;
    for (int i = 0; i < npts; i += 2) {

        if (dbDgrd < 0) {
            dd[j++] = fac * ((float)id[i]     + dgrd * gran());
            dd[j++] = fac * ((float)id[i + 1] + dgrd * gran());
        } else {
            dd[j++] = id[i];
            dd[j++] = id[i + 1];
        }

        if (!xpol) {
            // Skip channels 3 and 4
            j += 2;

        }
    }
     
  //  qDebug() << "GETFILE: starting decode for:" << fname;
    setNdiskdat(1);
    int nfreq = getFcenter();
    if (nfreq > 9998) setFcenter(9990.100);

    int i0 = fname.indexOf(".tf2");
    if (i0 < 0) i0 = fname.indexOf(".iq");

    setNutc(0);
    if (i0 > 0) {
        setNutc(100 * fname.mid(i0 - 4, 2).toInt() +
                fname.mid(i0 - 2, 2).toInt());
    }
}

void MainWindow::showSoundInError(const QString& errorMsg)
 {QMessageBox::critical(this, tr("Error in SoundIn"), errorMsg);}

void MainWindow::showStatusMessage(const QString& statusMsg)
 {statusBar()->showMessage(statusMsg);}

void MainWindow::on_actionDeviceSetup_triggered()
{
  DevSetup dlg(this);

  connect(&dlg, &DevSetup::sampleRateChanged,
      this, &MainWindow::onSampleRateChanged);

  dlg.initDlg();

    if (dlg.exec() == QDialog::Accepted)
    {
        //
        // Apply runtime effects for SoundIn
        //
        if (dlg.m_restartSoundIn)
        {
      soundInThread.quit();
      soundInThread.wait(1000);

            soundInThread.setInputDevice(m_paInDevice);
      soundInThread.setNetwork(m_network);
      soundInThread.setFadd(m_fAdd);
            if (m_fs96000 == 1) soundInThread.setRate(96000.0);
            else if (m_fs96000 == 0) soundInThread.setRate(95238.1);
            else if (m_fs96000 == 2) soundInThread.setRate(192000.0);
            soundInThread.setSwapIQ(m_IQswap);
            soundInThread.setScale(m_dB);
            soundInThread.setPort(m_udpPort);
            soundInThread.setNrx(m_xpol ? 2 : 1);

      soundInThread.start(QThread::HighestPriority);
    }

        //
        // Apply runtime effects for SoundOut
        //
        if (dlg.m_restartSoundOut)
        {
      soundOutThread.quitExecution=true;
      soundOutThread.wait(1000);

      soundOutThread.setOutputDevice(m_paOutDevice);
            soundOutThread.start();
    }

        //
        // GUI updates
        //
        if (m_astro_window && m_astro_window->isVisible())
            m_astro_window->setFontSize(m_astroFont);

        ui->actionFind_Delta_Phi->setEnabled(m_xpol);

        m_messages_window->setColors(m_colors, m_colorPreset != "Classic");
        m_band_map_window->setColors(m_colors, m_colorPreset != "Classic");

        //
        // WideGraph updates
        //
        m_wide_graph_window->m_mult570   = m_mult570;
        m_wide_graph_window->m_mult570Tx = m_mult570Tx;
        m_wide_graph_window->m_cal570    = m_cal570;
        m_wide_graph_window->setFcal(m_fCal);

        //
        // Save to disk
        //
        writeSettings();
  }
}

void MainWindow::onSampleRateChanged(int newRate)
{
    if (!m_wide_graph_window)
        return;

    // Same floor as the WideGraph constructor (50, G4SWX request). This
    // runs after startup restored the saved span, so a higher floor here
    // silently clamped a saved 50 back to 60 -- and the next settings
    // save made that permanent ("span setting is not persistent").
    if (newRate == 96000 || newRate == 95238)
        m_wide_graph_window->setFreqSpanLimits(50, 90);
    else
        m_wide_graph_window->setFreqSpanLimits(50, 190);

    m_wide_graph_window->updateSpanFromSpinbox();

    if (newRate == 95238) {
      lab8->setStyleSheet("QLabel{background-color: #ffc783}");
      lab8->setText("95.238 kHz");
    }
    if (newRate == 96000) {
      lab8->setStyleSheet("QLabel{background-color: #b4ffb4}");
      lab8->setText("96 kHz");
    }
    if (newRate == 192000) {
      lab8->setStyleSheet("QLabel{background-color: #ffccff}");
      lab8->setText("192 kHz");
    }
}


void MainWindow::on_monitorButton_clicked()                  //Monitor
{
  if (m_monitoring) {
    m_monitoring=false;
    soundInThread.setMonitoring(m_monitoring);
    m_loopall=false;
  } else {
    m_monitoring=true;
    qDebug() << "m_monitoring set to" << m_monitoring << "at" << Q_FUNC_INFO;
    soundInThread.setMonitoring(true);
    m_diskData=false;
  }
}

void MainWindow::on_actionLinrad_triggered()                 //Linrad palette
{
  if(m_wide_graph_window) m_wide_graph_window->setPalette("Linrad");
}

void MainWindow::on_actionCuteSDR_triggered()                //CuteSDR palette
{
  if(m_wide_graph_window) m_wide_graph_window->setPalette("CuteSDR");
}

void MainWindow::on_actionAFMHot_triggered()
{
  if(m_wide_graph_window) m_wide_graph_window->setPalette("AFMHot");
}

void MainWindow::on_actionBlue_triggered()
{
  if(m_wide_graph_window) m_wide_graph_window->setPalette("Blue");
}

void MainWindow::on_actionAbout_triggered()                  //Display "About"
{
  CAboutDlg dlg(this);
  dlg.exec();
}

void MainWindow::on_autoButton_clicked()                     //Auto
{
  m_auto = !m_auto;
  if(m_auto) {
    ui->autoButton->setStyleSheet(m_pbAutoOn_style);
//    ui->autoButton->setText("Auto is ON");
  } else {
    btxok=false;
    ui->autoButton->setStyleSheet("");
//    ui->autoButton->setText("Auto is OFF");
    on_monitorButton_clicked();
  }
}

void MainWindow::on_stopTxButton_clicked()                    //Stop Tx
{
  if(m_auto) on_autoButton_clicked();
  btxok=false;
}

void MainWindow::keyPressEvent( QKeyEvent *e )                //keyPressEvent
{
  switch(e->key())
  {
  case Qt::Key_F3:
    m_txMute=!m_txMute;
    break;
  case Qt::Key_F4:
    ui->dxCallEntry->setText("");
    ui->dxGridEntry->setText("");
    if(m_kb8rq) {
      m_ntx=6;
      ui->txrb6->setChecked(true);
    }
    break;
  case Qt::Key_F6:
    if(e->modifiers() & Qt::ShiftModifier) {
      on_actionDecode_remaining_files_in_directory_triggered();
    }
    break;
  case Qt::Key_F11:
    if(e->modifiers() & Qt::ShiftModifier) {
    } else {
      int n0=m_wide_graph_window->DF();
      int n=(n0 + 10000) % 5;
      if(n==0) n=5;
      m_wide_graph_window->setDF(n0-n);
    }
    break;
  case Qt::Key_F12:
    if(e->modifiers() & Qt::ShiftModifier) {
    } else {
      int n0=m_wide_graph_window->DF();
      int n=(n0 + 10000) % 5;
      if(n==0) n=5;
      m_wide_graph_window->setDF(n0+n);
    }
    break;
  case Qt::Key_G:
    if(e->modifiers() & Qt::AltModifier) {
      genStdMsgs("");
    }
    break;
  case Qt::Key_L:
    if(e->modifiers() & Qt::ControlModifier) {
      lookup();
      genStdMsgs("");
      break;
    }
  }
}

void MainWindow::bumpDF(int n)                                  //bumpDF()
{
  if(n==11) {
    int n0=m_wide_graph_window->DF();
    int n=(n0 + 10000) % 5;
    if(n==0) n=5;
    m_wide_graph_window->setDF(n0-n);
  }
  if(n==12) {
    int n0=m_wide_graph_window->DF();
    int n=(n0 + 10000) % 5;
    if(n==0) n=5;
    m_wide_graph_window->setDF(n0+n);
  }
}

bool MainWindow::eventFilter(QObject *object, QEvent *event)  //eventFilter()
{
  if (event->type() == QEvent::KeyPress) {
    //Use the event in parent using its keyPressEvent()
    QKeyEvent *keyEvent = static_cast<QKeyEvent *>(event);
    MainWindow::keyPressEvent(keyEvent);
    return QObject::eventFilter(object, event);
  }
  return QObject::eventFilter(object, event);
}

void MainWindow::createStatusBar()                           //createStatusBar
{
  lab1 = new QLabel("Receiving");
  lab1->setAlignment(Qt::AlignHCenter);
  lab1->setMinimumSize(QSize(80,10));
  lab1->setStyleSheet("QLabel{background-color: #00ff00}");
  lab1->setFrameStyle(QFrame::Panel | QFrame::Sunken);
  statusBar()->addWidget(lab1);

  lab2 = new QLabel("QSO freq:  125");
  lab2->setAlignment(Qt::AlignHCenter);
  lab2->setMinimumSize(QSize(90,10));
  lab2->setFrameStyle(QFrame::Panel | QFrame::Sunken);
  statusBar()->addWidget(lab2);

  lab3 = new QLabel("QSO DF:   0");
  lab3->setAlignment(Qt::AlignHCenter);
  lab3->setMinimumSize(QSize(80,10));
  lab3->setFrameStyle(QFrame::Panel | QFrame::Sunken);
  statusBar()->addWidget(lab3);

  lab4 = new QLabel("");
  lab4->setAlignment(Qt::AlignHCenter);
  lab4->setMinimumSize(QSize(80,10));
  lab4->setFrameStyle(QFrame::Panel | QFrame::Sunken);
  statusBar()->addWidget(lab4);

  lab5 = new QLabel("");
  lab5->setAlignment(Qt::AlignHCenter);
  lab5->setMinimumSize(QSize(50,10));
  lab5->setFrameStyle(QFrame::Panel | QFrame::Sunken);
  statusBar()->addWidget(lab5);

  lab6 = new QLabel("");
  lab6->setAlignment(Qt::AlignHCenter);
  lab6->setMinimumSize(QSize(50,10));
  lab6->setFrameStyle(QFrame::Panel | QFrame::Sunken);
  statusBar()->addWidget(lab6);

  lab7 = new QLabel("Avg: 0");
  lab7->setAlignment(Qt::AlignHCenter);
  lab7->setMinimumSize(QSize(50,10));
  lab7->setFrameStyle(QFrame::Panel | QFrame::Sunken);
  statusBar()->addWidget(lab7);

  lab8 = new QLabel("");
  lab8->setAlignment(Qt::AlignHCenter);
  lab8->setMinimumSize(QSize(70,10));
  lab8->setFrameStyle(QFrame::Panel | QFrame::Sunken);
  statusBar()->addWidget(lab8);
}

void MainWindow::on_tolSpinBox_valueChanged(int i)             //tolSpinBox
{
  static int ntol[] = {10,20,50,100,200,500,1000};
  m_tol=ntol[i];
  m_wide_graph_window->setTol(m_tol);
  ui->labTol1->setText(QString::number(ntol[i]));
}

void MainWindow::on_actionExit_triggered()                     //Exit()
{
  close ();
}

void MainWindow::closeEvent (QCloseEvent * e)
{
  captureWindowOpenStates();
  m_windowOpenStatesCaptured = true;
  set_stop_m65(1);
  if (m_gui_timer) m_gui_timer->stop ();
  m_wide_graph_window->saveSettings();

  QFile quitFile(m_dataDir + "/.quit");
    (void)quitFile.open(QFileDevice::ReadWrite);
    setQuitID(quitFile.handle());

  if (m_astro_window) m_astro_window->close ();
  if (m_band_map_window) m_band_map_window->close ();
  if (m_messages_window) {
    m_messages_window->setClosingForShutdown(true);
        m_messages_window->close();
  }
  if (m_wide_graph_window) m_wide_graph_window->close ();

#ifdef __unix__
    ptt_close();   // close persistent Linux serial port
#endif

    if (g_pTxTune) {
        g_pTxTune->close();
        delete g_pTxTune;
        g_pTxTune = nullptr;
    }

  quitFile.remove();
  QMainWindow::closeEvent (e);
}


void MainWindow::on_stopButton_clicked()                       //stopButton
{
  m_monitoring=false;
  qDebug() << "m_monitoring set to" << m_monitoring << "at" << Q_FUNC_INFO;
  soundInThread.setMonitoring(m_monitoring);

  m_loopall=false;  
}

void MainWindow::msgBox(QString t)                             //msgBox
{
  msgBox0.setText(t);
  msgBox0.exec();
}

void MainWindow::stub()                                        //stub()
{
  msgBox("Not yet implemented.");
}

void MainWindow::on_actionRelease_Notes_triggered()
{
  QDesktopServices::openUrl(QUrl(
  "https://wsjt.sourceforge.io/Release_Notes.txt",
                              QUrl::TolerantMode));
}

void MainWindow::on_actionOnline_Users_Guide_triggered()      //Display manual
{
  QDesktopServices::openUrl(QUrl(
  "https://wsjt.sourceforge.io/MAP65_Users_Guide.pdf",
                              QUrl::TolerantMode));
}

void MainWindow::on_actionQSG_Q65_triggered()
{
  QDesktopServices::openUrl (QUrl {"https://wsjt.sourceforge.io/Q65_Quick_Start.pdf"});
}

void MainWindow::on_actionQSG_MAP65_v3_triggered()
{
  QDesktopServices::openUrl (QUrl {"https://wsjt.sourceforge.io/WSJTX_2.5.0_MAP65_3.0_Quick_Start.pdf"});
}

void MainWindow::on_actionQ65_Sensitivity_in_MAP65_3_0_triggered()
{
  QDesktopServices::openUrl (QUrl {"https://wsjt.sourceforge.io/Q65_Sensitivity_in_MAP65.pdf"});
}

void MainWindow::on_actionAstro_Data_triggered()             //Display Astro
{
  if (m_astro_window ) m_astro_window->show();
}

void MainWindow::on_actionWide_Waterfall_triggered()      //Display Waterfalls
{
  m_wide_graph_window->show();
}

void MainWindow::on_actionBand_Map_triggered()              //Display BandMap
{
  m_band_map_window->show ();
}

void MainWindow::on_actionMessages_triggered()              //Display Messages
{
  m_messages_window->show();
}

void MainWindow::on_actionOpen_MAP65_data_directory_triggered()
{
  QDesktopServices::openUrl (QUrl::fromLocalFile (QDir {m_dataDir}.absolutePath()));
}

void MainWindow::on_actionOpen_triggered()                     //Open File
{
  m_monitoring=false;
  qDebug() << "m_monitoring set to" << m_monitoring << "at" << Q_FUNC_INFO;

  soundInThread.setMonitoring(m_monitoring);

  // Clear the decode labels and reset m_seenLabels
  if (m_wide_graph_window && ui->actionShow_callsigns_on_Waterfall->isChecked()) {
    m_wide_graph_window->clearDecodeLabels();
    m_seenLabels.clear();
  }

  QString fname;
  if(m_xpol) {
    fname=QFileDialog::getOpenFileName(this, "Open File", m_path,
                                       "EME65 Files (*.tf2)");
  } else {
    fname=QFileDialog::getOpenFileName(this, "Open File", m_path,
                                       "EME65 Files (*.iq)");
  }
  if(fname != "") openIQFile(fname);
}

// The body of File->Open, callable without the dialog: used by the menu
// action above and by the --open command-line flag (replay tests without GUI
// automation).
void MainWindow::openIQFile(QString fname)                     //openIQFile
{
    m_path=fname;
    int i;
    i=fname.indexOf(".iq") - 11;
    if(m_xpol) i=fname.indexOf(".tf2") - 11;
    if(i>=0) {
      lab1->setStyleSheet("QLabel{background-color: #66ff66}");
      lab1->setText(" " + fname.mid(i,15) + " ");
    }
    on_stopButton_clicked();
    // A fresh file open is a fresh test run: clear the session-global upload
    // dedupe (commons.h) so replaying the same recording uploads again -- it
    // silently swallowed every repeat replay (Uwe, 2026-08-21). Deliberately
    // NOT cleared for past-period replay, where the dedupe is exactly what
    // prevents re-uploading decodes already sent live this session.
    allDecodes.clear();
    allDecodes2.clear();
    m_diskData=true;
    m_decoderBusy=true;  //added 1.6.25 w3sz
    int dbDgrd=0;
    if(m_myCall=="K1JT" and m_idInt<0) dbDgrd=m_idInt;
    *future1 = QtConcurrent::run([this](QString fname, bool xpol, int dbDgrd) {
      this->getfile(fname, xpol, dbDgrd);
      }, fname, m_xpol, dbDgrd);      
    watcher1->setFuture(*future1);
}

void MainWindow::on_actionOpen_next_in_directory_triggered()   //Open Next
{
  if (m_decoderBusy) {
    qDebug() << "Decode is busy; not starting next file yet.";
    return;
  }
  m_decoderBusy = true;

  // Clear the decode labels and reset m_seenLabels
  if (m_wide_graph_window && ui->actionShow_callsigns_on_Waterfall->isChecked()) {
    m_wide_graph_window->clearDecodeLabels();
    m_seenLabels.clear();
  }

  int i,len;
  QFileInfo fi(m_path);
  QStringList list;
  if(m_xpol) {
      list= fi.dir().entryList().filter(".tf2");
  } else {
      list= fi.dir().entryList().filter(".iq");
  }
  for (i = 0; i < list.size()-1; ++i) {
    if(i==list.size()-2) m_loopall=false;
    const QString &entry = list.at(i);
    len = entry.length();
    if (entry == m_path.right(len)) {
      int n=m_path.length();
      QString fname = m_path;
      fname.replace(n - len, len, list.at(i+1));
      m_path=fname;
      int idx = fname.indexOf(m_xpol ? ".tf2" : ".iq") - 11;
      if (idx >= 0) {
        lab1->setStyleSheet("QLabel{background-color: #66ff66}");
          lab1->setText(" " + fname.mid(idx, len) + " ");
      }
    // A fresh file open is a fresh test run: clear the session-global upload
    // dedupe (commons.h) so replaying the same recording uploads again -- it
    // silently swallowed every repeat replay (Uwe, 2026-08-21). Deliberately
    // NOT cleared for past-period replay, where the dedupe is exactly what
    // prevents re-uploading decodes already sent live this session.
    allDecodes.clear();
    allDecodes2.clear();
      m_diskData=true;
      int dbDgrd=0;
      if(m_myCall=="K1JT" and m_idInt<0) dbDgrd=m_idInt;
      *future1 = QtConcurrent::run([this](QString fname, bool xpol, int dbDgrd) {
        this->getfile(fname, xpol, dbDgrd);
        }, fname, m_xpol, dbDgrd);
      qDebug() << "MainWindow::on_actionOpen_next_in_directory_triggered Reading wav file: " << m_path;        
      watcher1->setFuture(*future1);
      return;
    }
  }
}
                                                   //Open all remaining files
void MainWindow::on_actionDecode_remaining_files_in_directory_triggered()
{
  m_loopall=true;
  on_actionOpen_next_in_directory_triggered();
}

void MainWindow::diskDat()                                   //diskDat()
{
  double hsym;
  //These may be redundant??
  m_diskData=true;
  setNewdat(1);
  if(m_wide_graph_window->m_bForceCenterFreq) {
    setFcenter(m_wide_graph_window->m_dForceCenterFreq);
  }

  hsym=2048.0*96000.0/11025.0;   //Samples per JT65 half-symbol if SR = 96000
  if(m_fs96000 == 0) hsym=2048.0*95238.1/11025.0; // Samples per JT65 half-symbol if SR =  95238.1
  else if(m_fs96000 == 2) hsym=2048.0*192000.0/11025.0;   //Samples per JT65 half-symbol if SR = 192000
  for(int i=0; i<304; i++) {           // Do the half-symbol FFTs
    int k = i*hsym + 2048.5;
    dataSink(k);
    qApp->processEvents();             // Allow the waterfall to update
  }
}

void MainWindow::diskWriteFinished()                      //diskWriteFinished
{
//  qDebug() << "diskWriteFinished";
//  decode();
}
                                                        //Delete ../save/*.tf2
void MainWindow::on_actionDelete_all_tf2_files_in_SaveDir_triggered()
{
  int i;
  QString fname;
  int ret = QMessageBox::warning(this, "Confirm Delete",
      "Are you sure you want to delete all *.tf2 and *.iq files in\n" +
       QDir::toNativeSeparators(m_saveDir) + " ?",
       QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
  if(ret==QMessageBox::Yes) {
    QDir dir(m_saveDir);
    QStringList files=dir.entryList(QDir::Files);
    QList<QString>::iterator f;
    for(f=files.begin(); f!=files.end(); ++f) {
      fname=*f;
      i=(fname.indexOf(".tf2"));
      if(i==11) dir.remove(fname);
      i=(fname.indexOf(".iq"));
      if(i==11) dir.remove(fname);
    }
  }
}
                                          //Clear BandMap and Messages windows
void MainWindow::on_actionErase_Band_Map_and_Messages_triggered()
{
  m_band_map_window->setText("");
  m_messages_window->setText("","");
  m_map65RxLog |= 4;
}

void MainWindow::on_actionFind_Delta_Phi_triggered()              //Find dPhi
{
  m_map65RxLog |= 8;
  on_DecodeButton_clicked();
}

void MainWindow::on_actionF4_sets_Tx6_triggered()                //F4 sets Tx6
{
  m_kb8rq = !m_kb8rq;
}

void MainWindow::on_actionOnly_EME_calls_triggered()          //EME calls only
{
  m_onlyEME = ui->actionOnly_EME_calls->isChecked();
}

void MainWindow::on_actionNo_shorthands_if_Tx1_triggered()
{
  stub();
}

void MainWindow::on_actionNo_Deep_Search_triggered()          //No Deep Search
{
  m_ndepth=0;
}

void MainWindow::on_actionNormal_Deep_Search_triggered()      //Normal DS
{
  m_ndepth=1;
}

void MainWindow::on_actionAggressive_Deep_Search_triggered()  //Aggressive DS
{
  m_ndepth=2;
}

void MainWindow::on_actionQ65_Fast_triggered()                //Q65 Fast
{
  m_q65depth=1;
}

void MainWindow::on_actionQ65_Normal_triggered()              //Q65 Normal
{
  m_q65depth=2;
}

void MainWindow::on_actionQ65_Deep_triggered()                //Q65 Deep
{
  m_q65depth=3;
}

void MainWindow::on_actionNone_triggered()                    //Save None
{
  m_saveAll=false;
}

// ### Implement "Save Last" here? ###

void MainWindow::on_actionSave_all_triggered()                //Save All
{
  m_saveAll=true;
}
                                          //Display list of keyboard shortcuts
void MainWindow::on_actionKeyboard_shortcuts_triggered()
{
  stub();
}
                                              //Display list of mouse commands
void MainWindow::on_actionSpecial_mouse_commands_triggered()
{
  stub();
}
                                              //Diaplay list of Add-On pfx/sfx
void MainWindow::on_actionAvailable_suffixes_and_add_on_prefixes_triggered()
{
  stub();
}

//------------------------------------------------ Past-period decode
// View -> "Show/Decode past decodes on Wide Graph". While enabled, dataSink
// keeps a 3-deep rotating ring of raw periods in save/ring/, the waterfall
// tags each row with its period, and a click on a past row arms the Decode
// button (text shows the period time). Decode then replays the ring file
// through the standard disk path: getfile -> diskDat -> decode, which
// derives UTC and EME self-Doppler from the FILENAME, so the past period
// decodes with its own Doppler.

void MainWindow::on_actionPast_periods_on_Wide_Graph_toggled(bool b)
{
  m_pastPeriods=b;
  if(m_wide_graph_window) m_wide_graph_window->setPastEnabled(b);
  if(!b) disarmPastPeriod();
}

void MainWindow::onPastPeriodClicked(QString tag)
{
  if(!m_pastPeriods) return;
  QString cur=QDateTime::currentDateTimeUtc().toString("yyMMdd_hhmm");
  if(tag.isEmpty() or tag==cur) {      // current period or untagged row
    disarmPastPeriod();
    return;
  }
  QString ext = m_xpol ? ".tf2" : ".iq";
  QString path = m_saveDir + "/ring/" + tag + ext;
  if(!QFile::exists(path)) path = m_saveDir + "/" + tag + ext;  // Save All copy
  if(!QFile::exists(path)) {           // rotated out or never saved
    disarmPastPeriod();
    return;
  }
  m_pastArmedTag=tag;
  m_pastArmedPath=path;
  // Show the armed period's time on the Decode button: "Decode 12:03".
  // Keep the & so the Alt+D mnemonic survives arming.
  ui->DecodeButton->setText("&Decode " + tag.mid(7,2) + ":" + tag.mid(9,2));
}

void MainWindow::disarmPastPeriod()
{
  m_pastArmedTag.clear();
  m_pastArmedPath.clear();
  ui->DecodeButton->setText("&Decode");
}

void MainWindow::decodePastPeriod()
{
  if(!QFile::exists(m_pastArmedPath)) {  // ring rotated it away meanwhile
    disarmPastPeriod();
    return;
  }
  m_wasMonitoring=m_monitoring;
  // Same sequence as on_actionOpen_triggered, minus the file dialog.
  on_stopButton_clicked();
  if (m_wide_graph_window && ui->actionShow_callsigns_on_Waterfall->isChecked()) {
    m_wide_graph_window->clearDecodeLabels();
    m_seenLabels.clear();
  }
  m_path=m_pastArmedPath;
  int i=m_path.indexOf(m_xpol ? ".tf2" : ".iq") - 11;
  if(i>=0) {
    lab1->setStyleSheet("QLabel{background-color: #66ff66}");
    lab1->setText(" " + m_path.mid(i,15) + " ");
  }
  m_diskData=true;
  m_decoderBusy=true;
  m_pastReplayActive=true;
  int dbDgrd=0;
  *future1 = QtConcurrent::run([this](QString fname, bool xpol, int dbDgrd) {
    this->getfile(fname, xpol, dbDgrd);
    }, m_path, m_xpol, dbDgrd);
  watcher1->setFuture(*future1);
}

void MainWindow::on_DecodeButton_clicked()                    //Decode request
{
  int n=m_sec0%m_TRperiod;
  if(m_monitoring and n>47 and (n<52 or m_decoderBusy)) return;
  // Past-period decode: when a past period is armed (waterfall click with
  // the View toggle on), Decode replays that saved period instead of
  // re-decoding the live buffer.
  if(m_pastPeriods and !m_pastArmedPath.isEmpty()) {
    if(!m_decoderBusy) decodePastPeriod();
    return;
  }
  if(!m_decoderBusy) {
    setNewdat(0);
    setNagain(1);
    decode();
  }
}

void MainWindow::freezeDecode(int n)                          //freezeDecode()
{
  // Keep-period re-decode: on the lower (zoomed) graph only, and only while
  // the snapshot actually holds a decoded period. The operator's Tol is the
  // search window around the click, so unlike the live freeze below it is
  // left alone rather than clamped.
  const bool held = (n==1) && ui->actionKeep_last_period->isChecked()
                          && m_snapshotValid;
  if(n==2) {
    ui->tolSpinBox->setValue(5);
    setNtol(m_tol);
    setMousedf(0);
  } else if(!held) {
    ui->tolSpinBox->setValue(qMin(3,ui->tolSpinBox->value()));
    setNtol(m_tol);
  } else {
    setNtol(m_tol);
  }
  if(!m_decoderBusy) {
    setNagain(1);
    setNewdat(0);
    m_holdSnapshot = held;
    decode();
  }
}

void MainWindow::decode()                                       //decode()
{
  // No point decoding a period in which WSJT-X was keyed for more than a
  // few seconds: the buffer holds our own transmitter (blanked) and little
  // else. Same rule and threshold as QMAP. Disk replay is never affected.
  if(!m_diskData && rxMutedByWsjtx() && m_nWsjtxTxSec>5) {
    std::fprintf(stderr, "[map65] decode skipped: WSJT-X transmitted %d s of this period\n", m_nWsjtxTxSec);
    return;
  }
  // Is a PREVIOUS decode still running? m_decoderBusy is raised here and only
  // lowered on <DecodeFinished> (pass 2) -- <EarlyFinished> (pass 1) leaves it
  // set -- so this reads true when pass 1 is still in the decoder, or when a
  // new file is loaded before the last one finished. Must be sampled before
  // decodeBusy(true) below. Used to protect the snapshot copy further down.
  const bool decoderStillBusy = m_decoderRunning;
  // One-shot: armed by freezeDecode for a held-period re-decode, consumed
  // here so every other decode path refreshes the snapshot as usual.
  const bool holdSnapshot = m_holdSnapshot;
  m_holdSnapshot = false;
  decodeBusy(true);
  // Reset decode-finished count for this file
  m_decodeFinishedCount = 0;

  ui->DecodeButton->setStyleSheet(m_pbdecoding_style1);

//  QFile f("mockRTfiles.txt");
//  if(datcom_.nagain==0 && (!m_diskData) && !f.exists()) {
  if(getNagain()==0 && (!m_diskData)) {
    qint64 ms = QDateTime::currentMSecsSinceEpoch() % 86400000;
    int imin=ms/60000;
    int ihr=imin/60;
    imin=imin % 60;
    setNutc(100*ihr + imin);
  }

  setIdphi(m_dPhi);
  setMousedf(m_wide_graph_window->DF());
  setMousefqso(m_wide_graph_window->QSOfreq());
  setNdepth(m_ndepth);
  setNq65depth(m_q65depth);
  setNdiskdat(0);
  if(m_diskData) {
    if(m_myGrid.trimmed().length()>=6) {
      setNdiskdat(1);
      int i0=m_path.indexOf(".tf2");
      if(i0<0) i0=m_path.indexOf(".iq");
      if(i0>0) {
        // Compute self Doppler using the filename for Date and Time
        int nyear=m_path.mid(i0-11,2).toInt()+2000;
        int month=m_path.mid(i0-9,2).toInt();
        int nday=m_path.mid(i0-7,2).toInt();
        int nhr=m_path.mid(i0-4,2).toInt();
        int nmin=m_path.mid(i0-2,2).toInt();
        double uth=nhr + nmin/60.0;
        int nfreq = getFcenter();
        int ndop00;
        QByteArray myGridData = m_myGrid.toLatin1();
        astrosub00_(&nyear, &month, &nday, &uth, &nfreq, myGridData.constData(),&ndop00, myGridData.size());
        setNdop00(ndop00);               //Send self Doppler to decoder, via datcom
      }
    }
    else { 
      QMessageBox::information(this,"EME65","No 6-digit MyGrid recognized by decode()");
      return;
    }
  }
  setNeme(0);
  if(ui->actionOnly_EME_calls->isChecked()) setNeme(1);

  int ispan = int(m_wide_graph_window->fSpan());
  if (ispan % 2 == 1) ispan++;

  double fc = getFcenter();  //MHz
  // ifc is fractional kHz of RF center, used to align baseband window with RF dial.
  // Must stay in [0, 999]. Changing fcenter semantics will break wideband.
  int ifc = int(1000.0*(fc - int(fc)) + 0.5);  // fractional kHz of RF center

  int nfa=m_wide_graph_window->nStartFreq();
  int nfb=nfa+ispan;
  int nfshift=nfa + ispan/2 - ifc;

  setNfa(nfa);
  setNfb(nfb);
  setNfshift(nfshift);

  setNfcal(m_fCal);
  setMcall3(0);
  if(m_call3Modified) setMcall3(1);
  setNtimeout(m_timeout);
  setNtol(m_tol);
  setNxant(0);
  if(m_xpolx) setNxant(1);
  if(getNutc() < m_nutc0) m_map65RxLog |= 1;  //Date and Time to map65_rx.log
  m_nutc0=getNutc();
  setMap65RxLog(m_map65RxLog);
  if(m_fs96000 == 1) setNfsample(96000);
  if(m_fs96000 == 0) setNfsample(95238);
  else if(m_fs96000 == 2) setNfsample(192000);
 // qDebug() << "MainWindow::decode setting nfsample to " << m_fs96000;
  setNxpol(0);
  if(m_xpol) setNxpol(1);
  setNmode(10*m_modeQ65 + m_modeJT65);
//  datcom_.nfast=1;                               //No longer used
  setNsave(m_nsave);
  setMaxDrift(ui->sbMaxDrift->value());

QString mcall = (m_myCall + "            ").mid(0, 12);
QString mgrid = (m_myGrid + "            ").mid(0, 6);
QString hcall = (ui->dxCallEntry->text() + "            ").mid(0, 12);
QString hgrid = (ui->dxGridEntry->text() + "      ").mid(0, 6);

  setMyCall(mcall);
  setMyGrid(mgrid);
  setHisCall(hcall);
  setHisGrid(hgrid);
  setDatetime(m_dateTime);
  setNewdat(1);
  setJunk1(1234);
  setJunk2(5678);

  // A held-period re-decode keeps nagain=1 from freezeDecode: in the
  // decoder that means narrow pass only, dupe guards cleared, nutc
  // retained -- the held period decodes at the clicked frequency under
  // its own UTC.
  if (!holdSnapshot) setNagain(0); //added 12-30-25 to agree with legacy
  if (!m_diskData) setNdiskdat(0);  //added 12-30-25 to agree with legacy

  // Freeze the acquisition state for this decode: memcpy the live buffers
  // into the decoder-side snapshots BEFORE raising decoder_ready. This runs
  // on the GUI thread -- the same thread dataSink/symspec write from -- so
  // the copy is atomic with respect to the writer, exactly like the old
  // two-process design's shared-memory copy in 2.7.0's decode(). The
  // decoder (run_m65 loop -> m65a -> map65a) reads only the snapshots, so
  // pass-1 JT65 scans that outlive the minute wrap now see consistent data
  // instead of a half-overwritten spectra buffer.
  //
  // BUT NOT while a previous decode is still running: the decoder is reading
  // these very buffers, and refreshing them under it produces decodes made
  // from two different periods' data -- garbage callsigns carrying the wrong
  // UTC and sub-mode. That is easy to hit by loading files back to back, and
  // it is a regression this snapshot mechanism introduced (before it, a
  // second trigger touched only flags, never the data). Keeping the existing
  // snapshot means the newly-triggered pass decodes slightly older but
  // CONSISTENT data, which is always better than a torn buffer.
  // ... and not on a held-period re-decode either: keeping the snapshot IS
  // the feature there -- the decoder re-reads the last decoded period at the
  // newly clicked frequency, the RAM equivalent of 2.7.0's newdat==0 shared
  // memory copy that skipped the data blocks.
  if (!decoderStillBusy && !holdSnapshot) {
    memcpy(dd_snap,   dd,   sizeof(float) * ddSize);
    memcpy(ss_snap,   ss,   sizeof(float) * 4 * 322 * active_nfft);
    memcpy(savg_snap, savg, sizeof(float) * 4 * active_nfft);
    m_snapshotValid = true;
  } else if (decoderStillBusy) {
    fprintf(stderr, "%s", "[snapshot] decoder still busy -- keeping the current snapshot\n");
  }

  m_decoderRunning = true;
  setDecoderReady(1);
  m_map65RxLog=0;
  m_call3Modified=false;
//  qDebug() << QDateTime::currentMSecsSinceEpoch()  << "finished MainWindow::decode()";
}

bool MainWindow::subProcessFailed (QProcess * process, int exit_code, QProcess::ExitStatus status)
{
  if (exit_code || QProcess::NormalExit != status)
    {
      QStringList arguments;
      for (auto argument: process->arguments ())
        {
          if (argument.contains (' ')) argument = '"' + argument + '"';
          arguments << argument;
        }
      writeCrashData();  
      MessageBox::critical_message (this, tr ("Subprocess Error")
                                    , tr ("Subprocess failed with exit code %1")
                                    .arg (exit_code)
                                    , tr ("Running: %1\n%2")
                                    .arg (process->program () + ' ' + arguments.join (' '))
                                    .arg (QString {process->readAllStandardError()}));
      return true;
    }
  return false;
}

void MainWindow::writeCrashData() {
  QFile file("crash_data.txt");

  if (file.open(QIODevice::Append | QIODevice::Text)) {
      QTextStream out(&file);

      out << "---- Crash Data Dump ----\n";

      out << "fcenter: " << getFcenter() << "\n";

      out << "nutc: " << getNutc() << ", idphi: " << getIdphi()
          << ", mousedf: " << getMousedf() << ", mousefqso: " << getMousefqso()
          << ", nagain: " << getNagain() << ", ndepth: " << getNdepth() << "\n";

      out << "ndiskdat: " << getNdiskdat() << ", neme: " << getNeme()
          << ", newdat: " << getNewdat() << ", nfa: " << getNfa()
          << ", nfb: " << getNfb() << ", nfcal: " << getNfcal()
          << ", nfshift: " << getNfshift() << "\n";

      out << "mcall3: " << getMcall3() << ", ntimeout: " << getNtimeout()
          << ", ntol: " << getNtol() << ", nxant: " << getNxant()
          << ", map65RxLog: " << getMap65RxLog() << ", nfsample: " << getNfsample() << "\n";

      out << "nxpol: " << getNxpol() << ", nmode: " << getNmode()
          << ", nsave: " << getNsave()
          << ", max_drift: " << getMaxDrift() << ", nhsym: " << getNhsym() << "\n";

      out << "junk1: " << getJunk1() << ", junk2: " << getJunk2() << "\n";

      out << "mycall: " << getMyCall() << "\n";
      out << "hiscall: " << getHisCall() << "\n";
      out << "mygrid: " << getMyGrid() << "\n";
      out << "hisgrid: " << getHisGrid() << "\n";
      out << "datetime: " << getDatetime() << "\n";

      out << "--------------------------\n";

      file.close();
  } else {
      qWarning("Could not open crash_data.txt for writing.");
  }
}

void MainWindow::editor_error()                                 //editor_error
{
  msgBox("Error starting or running\n" + m_appDir + "/" + m_editorCommand);
}

void MainWindow::on_EraseButton_clicked()
{
  qint64 ms=QDateTime::currentMSecsSinceEpoch();
  ui->decodedTextBrowser->clear();
  if((ms-m_msErase)<500) {
    on_actionErase_Band_Map_and_Messages_triggered();
  }
  m_msErase=ms;
}

void MainWindow::mousePressEvent(QMouseEvent *event)    // mouse press events
{
  if(ui->EraseButton->hasFocus()) {                             // Erase button
    if (event->button() & Qt::RightButton) {
      ui->dxCallEntry->setText("");
      ui->dxGridEntry->setText("");
      ui->tx1->setText("");
      ui->tx2->setText("");
      ui->tx3->setText("");
      ui->tx4->setText("");
      ui->tx5->setText("");
      ui->txb6->click();
    }
    ui->EraseButton->clearFocus();
  }
}

void MainWindow::decodeBusy(bool b)                             //decodeBusy()
{
//  qDebug()  << QDateTime::currentMSecsSinceEpoch() << "decodeBusy(" << b << ")";
  m_decoderBusy=b;
  ui->DecodeButton->setEnabled(!b);
  ui->actionOpen->setEnabled(!b);
  ui->actionOpen_next_in_directory->setEnabled(!b);
  ui->actionDecode_remaining_files_in_directory->setEnabled(!b);
}

//------------------------------------------------------------- //guiUpdate()
void MainWindow::guiUpdate()
{
  // Reverse channel: mirror WSJT-X's DX Call into MAP65 when it changes
  // (only when Sync is on). Edge-detected via dxcall_seq so we act once
  // per change. Setting dxCallEntry doesn't relay back (only user clicks
  // do), so there is no feedback loop.
  if (m_syncWsjtx && m_qmapShm && m_memQmap.isAttached()) {
    QString rxCall, rxGrid;
    bool changed = false;
    m_memQmap.lock();
    if (m_qmapShm->dxcall_seq != m_lastRxDxcallSeq) {
      m_lastRxDxcallSeq = m_qmapShm->dxcall_seq;
      changed = true;
      char cbuf[16]; std::memcpy(cbuf, m_qmapShm->dxcall, sizeof(cbuf)); cbuf[sizeof(cbuf)-1]=0;
      char gbuf[8];  std::memcpy(gbuf, m_qmapShm->dxgrid, sizeof(gbuf)); gbuf[sizeof(gbuf)-1]=0;
      rxCall = QString::fromUtf8(cbuf);
      rxGrid = QString::fromUtf8(gbuf);
    }
    m_memQmap.unlock();
    if (changed) {
      if (rxCall != ui->dxCallEntry->text()) ui->dxCallEntry->setText(rxCall);
      if (rxGrid != ui->dxGridEntry->text()) ui->dxGridEntry->setText(rxGrid);
    }
  }

  static int iptt0=0;
  static int iptt=0;
  static bool btxok0=false;
  static bool bTune0=false;
  static bool bMonitoring0=false;
  static int nc0=1;
  static int nc1=1;
  static char msgsent[23];
  static int nsendingsh=0;
  int khsym=0;

  double tx1=0.0;
  double tx2=126.0*4096.0/11025.0 + 1.8;
  if(m_modeTx=="Q65") tx2=85.0*7200.0/12000.0 + 1.8;

  if(!m_txFirst) {
    tx1 += m_TRperiod;
    tx2 += m_TRperiod;
  }
  qint64 ms = QDateTime::currentMSecsSinceEpoch() % 86400000;
  int nsec=ms/1000;
  double tsec=0.001*ms;
  double t2p=fmod(tsec,120.0);
  bool bTxTime = (t2p >= tx1) and (t2p < tx2);

  if(bTune0 and !bTune) {
    btxok=false;
    m_monitoring=bMonitoring0;
  //  qDebug() << "m_monitoring set to" << m_monitoring << "at" << Q_FUNC_INFO;

    soundInThread.setMonitoring(m_monitoring);
  }
  if(bTune and !bTune0) bMonitoring0=m_monitoring;
  bTune0=bTune;

  if(m_auto or bTune) {
    if ((bTxTime or bTune) && iptt == 0 && !m_txMute) {

  if (m_pttPath != "NONE") {
      int itx=1;
      int nport = m_pttPortNumber;   // the real COM port number
      int ierr = ptt_(&nport, &itx, &iptt);

      if(ierr != 0) {
          if (!m_pttErrorShown) {
              char s[256];
              snprintf(s, sizeof(s), "Cannot open Port: %s",
                      m_pttPath.toUtf8().constData());
              msgBox(s);
              m_pttErrorShown = true;
          }
        on_stopTxButton_clicked();
      }
      }

        if (m_bIQxt)
            m_wide_graph_window->tx570();

        if (!soundOutThread.isRunning())
        soundOutThread.start(QThread::HighPriority);
      }

    if ((!bTxTime && !bTune) || m_txMute)
      btxok=false;
    }

// Calculate Tx waveform when needed
  if((iptt==1 && iptt0==0) || m_restart) {
    char message[23];
    QByteArray ba;
    if(m_ntx == 1) ba=ui->tx1->text().toLocal8Bit();
    if(m_ntx == 2) ba=ui->tx2->text().toLocal8Bit();
    if(m_ntx == 3) ba=ui->tx3->text().toLocal8Bit();
    if(m_ntx == 4) ba=ui->tx4->text().toLocal8Bit();
    if(m_ntx == 5) ba=ui->tx5->text().toLocal8Bit();
    if(m_ntx == 6) ba=ui->tx6->text().toLocal8Bit();

    ba2msg(ba,message);
    int len1=22;
    int mode65=m_mode65;
    int ntxFreq=1000;
    double samfac=1.0;
  //  qDebug() << mode65 << samfac;
    if(m_modeTx=="JT65") {
      gen65_(message,&mode65,&samfac,&nsendingsh,msgsent,iwave,
             &nwave,len1,len1);
    } else {
      if(m_modeQ65==5) ntxFreq=700;
      gen_q65_wave_(message,&ntxFreq,&m_modeQ65,msgsent,iwave,
                 &nwave,len1,len1);
    }
    msgsent[22]=0;

    if(m_restart) {
      QString t="  Tx " + m_modeTx + "   ";
      t=t.left(11);
      QFile f("map65_tx.log");
      qDebug() << "MainWindow::guiUpdate 1 File open result:" << f.open(QFileDevice::WriteOnly | QFileDevice::Text | QFileDevice::Append);
      QTextStream out(&f);
      out << QDateTime::currentDateTimeUtc().toString("yyyy-MMM-dd hh:mm")
          << t << QString::fromLatin1(msgsent)
#if QT_VERSION >= QT_VERSION_CHECK (5, 15, 0)
          << Qt::endl
#else
          << endl
#endif
        ;
      f.close();
    }

    m_restart=false;
  }

// If PTT was just raised, start a countdown for raising TxOK:
  if(iptt==1 && iptt0==0) nc1=-9;    // TxDelay = 0.8 s
  if(nc1 <= 0) nc1++;
  if(nc1 == 0) {
    xSignalMeter->setValue(0);
    ySignalMeter->setValue(0);
    m_monitoring=false;
  //  qDebug() << "m_monitoring set to" << m_monitoring << "at" << Q_FUNC_INFO;

    soundInThread.setMonitoring(false);
    btxok=true;
    m_transmitting=true;
    m_wide_graph_window->enableSetRxHardware(false);

    QString t="  Tx " + m_modeTx + "   ";
    t=t.left(11);
    QFile f("map65_tx.log");
    qDebug() << "MainWindow::guiUpdate 2 File open result:" << f.open(QFileDevice::WriteOnly | QFileDevice::Text | QFileDevice::Append);
    QTextStream out(&f);
    out << QDateTime::currentDateTimeUtc().toString("yyyy-MMM-dd hh:mm")
        << t << QString::fromLatin1(msgsent)
#if QT_VERSION >= QT_VERSION_CHECK (5, 15, 0)
        << Qt::endl
#else
        << endl
#endif
      ;
    f.close();
  }

// If btxok was just lowered, start a countdown for lowering PTT
  if(!btxok && btxok0 && iptt==1) nc0=-11;  //RxDelay = 1.0 s
  btxok0=btxok;
  if(nc0 <= 0) nc0++;
  if(nc0 == 0) {
    if(m_bIQxt) m_wide_graph_window->rx570();     // Set Si570 back to Rx Freq
    int itx=0;
  int nport = m_pttPortNumber;   // the real COM port number
  ptt_(&nport, &itx, &iptt);     // Lower PTT
  m_pttErrorShown = false;

    if(!m_txMute) {
      soundOutThread.quitExecution=true;\
    }
    m_transmitting=false;
    m_wide_graph_window->enableSetRxHardware(true);
    if(m_auto) {
      m_monitoring=true;
    //  qDebug() << "m_monitoring set to" << m_monitoring << "at" << Q_FUNC_INFO;

      soundInThread.setMonitoring(m_monitoring);
    }
  }

  if(iptt == 0 && !btxok) {
    // sending=""
    // nsendingsh=0
  }

  if(m_monitoring) {
    ui->monitorButton->setStyleSheet(m_pbmonitor_style);
  } else {
    ui->monitorButton->setStyleSheet("");
  }

  lab2->setText("QSO Freq:  " + QString::number(m_wide_graph_window->QSOfreq()));
  lab3->setText("QSO DF:  " + QString::number(m_wide_graph_window->DF()));

  m_wide_graph_window->updateFreqLabel();

  if(m_modeQ65==0 and m_modeTx=="Q65") on_pbTxMode_clicked();
  if(m_modeJT65==0  and m_modeTx=="JT65")  on_pbTxMode_clicked();

  if(nsec != m_sec0) {                                     //Once per second
//    qDebug() << "A" << nsec%60 << m_mode65 << m_modeQ65 << m_modeTx;
    // See if WSJT-X is transmitting (it writes nWTransmitting = its T/R
    // period into the shared segment every tick while keyed). Count the
    // seconds of Tx in this period; decode() skips a period that was
    // mostly Tx. Reset the count when a new period starts.
    {
      static int ntrz=99;
      const int ntr = nsec % m_TRperiod;
      bool keyed=false;
      if(m_qmapShm && m_memQmap.isAttached() && m_syncWsjtx) {
        m_memQmap.lock();
        keyed = m_qmapShm->nWTransmitting > 0;
        m_memQmap.unlock();
      }
      if(ntr < ntrz && !m_diskData) m_nWsjtxTxSec=0;
      ntrz=ntr;
      m_bWTransmitting=keyed;
      if(keyed) m_nWsjtxTxSec++;
    }
    soundInThread.setForceCenterFreqMHz(m_wide_graph_window->m_dForceCenterFreq);
    soundInThread.setForceCenterFreqBool(m_wide_graph_window->m_bForceCenterFreq);

    if(m_pctZap>30.0 and !m_transmitting) {
      lab4->setStyleSheet("QLabel{background-color: #ff0000; color: white;}");
    } else {
      lab4->setStyleSheet("");
    }

    if(m_transmitting) {
      if(nsendingsh==1) {
        lab1->setStyleSheet("QLabel{background-color: #66ffff}");
      } else if(nsendingsh==-1) {
        lab1->setStyleSheet("QLabel{background-color: #ffccff}");
      } else {
        lab1->setStyleSheet("QLabel{background-color: #ffff33}");
      }
      char s[37];
      snprintf(s, sizeof(s), "Tx: %.32s", msgsent);
      lab1->setText(s);
    } else if(m_bWTransmitting && m_monitoring) {
      lab1->setStyleSheet("QLabel{background-color: #ffff00}");  //Yellow, as in QMAP
      lab1->setText(rxMutedByWsjtx() ? "WS Transmitting" : "WS Transmitting (Rx on)");
    } else if(m_monitoring) {
      lab1->setStyleSheet("QLabel{background-color: #00ff00}");
      m_nrx=soundInThread.nrx();
      khsym=soundInThread.mhsym();
      QString t;
      if(m_network) {
        if(m_nrx==-1) t="F1";
        if(m_nrx==1) t="I1";
        if(m_nrx==-2) t="F2";
        if(m_nrx==+2) t="I2";
      } else {
        if(m_nrx==1) t="S1";
        if(m_nrx==2) t="S2";
      }
      if((abs(m_nrx)==1 and m_xpol) or (abs(m_nrx)==2 and !m_xpol))
        lab1->setStyleSheet("QLabel{background-color: #ff1493}");
      if(khsym==m_hsym0) {
        t="Nil";
        lab1->setStyleSheet("QLabel{background-color: #ffc0cb}");
      }
      lab1->setText("Receiving " + t);
    } else if (!m_diskData) {
      lab1->setStyleSheet("");
      lab1->setText("");
    }

    QDateTime t = QDateTime::currentDateTimeUtc();
    int fQSO=m_wide_graph_window->QSOfreq();
    if (m_wide_graph_window->m_bLockTxRx) m_txFreq=fQSO;
    m_astro_window->astroUpdate(t, m_myGrid, m_hisGrid, fQSO, m_setftx,
                          m_txFreq, m_azelDir, m_xavg);
    m_setftx=0;
    QString utc = t.date().toString(" yyyy MMM dd \n") + t.time().toString();
    ui->labUTC->setText(utc);
    guiDate = ui->labUTC->text().trimmed().mid(0,12); //liveCQ
    if((!m_monitoring and !m_diskData) or (khsym==m_hsym0)) {
      xSignalMeter->setValue(0);
      ySignalMeter->setValue(0);
      lab4->setText(" Rx noise:    0.0     0.0  0.0% ");
    }
    m_hsym0=khsym;
    m_sec0=nsec;
  }
  iptt0=iptt;
  bIQxt=m_bIQxt;
}

void MainWindow::ba2msg(QByteArray ba, char message[])             //ba2msg()
{
  bool eom;
  eom=false;
  for(int i=0;i<22; i++) {
    if (i >= ba.size () || !ba[i]) eom=true;
    if(eom) {
      message[i] = ' ';
    } else {
      message[i]=ba[i];
    }
  }
  message[22] = '\0';
}

void MainWindow::on_txFirstCheckBox_stateChanged(int nstate)        //TxFirst
{
  m_txFirst = (nstate==2);
}

void MainWindow::set_ntx(int n)                                   //set_ntx()
{
  m_ntx=n;
}

void MainWindow::on_txb1_clicked()                                //txb1
{
  m_ntx=1;
  ui->txrb1->setChecked(true);
  m_restart=true;
}

void MainWindow::on_txb2_clicked()                                //txb2
{
  m_ntx=2;
  ui->txrb2->setChecked(true);
  m_restart=true;
}

void MainWindow::on_txb3_clicked()                                //txb3
{
  m_ntx=3;
  ui->txrb3->setChecked(true);
  m_restart=true;
}

void MainWindow::on_txb4_clicked()                                //txb4
{
  m_ntx=4;
  ui->txrb4->setChecked(true);
  m_restart=true;
}

void MainWindow::on_txb5_clicked()                                //txb5
{
  m_ntx=5;
  ui->txrb5->setChecked(true);
  m_restart=true;
}

void MainWindow::on_txb6_clicked()                                //txb6
{
  m_ntx=6;
  ui->txrb6->setChecked(true);
  m_restart=true;
}

// Click anywhere on the line -- no more per-word picking. The whole line
namespace {

// Frequency of the decode the operator actually clicked, expressed as kHz above
// the band's whole MHz -- the unit relayClickToWsjtx() works in.
//
// The click handlers used to take this from m_bandmapFreq, a per-callsign map
// fed ONLY by the BandMap's "&" lines (processStdOut, "&" branch). A station
// missing from that map -- never listed, aged out, or below the BandMap's
// filter -- fell through to the 0.0 default, and 0.0 relays the bare band edge:
// Uwe's report of 144.000 arriving at WSJT-X in place of 144.100.
//
// Reading it from the clicked line is also more correct than any per-callsign
// lookup, because one station can be decoded on two frequencies in the same
// period -- DL3WDG appeared on both 144.124 and 144.134 during the shorthand
// work, and a by-callsign map can only remember one of them.

// Both decode windows present these two fields at the same offsets, by
// different routes:
//
//   Main window   the "!" marker is stripped (decode_line = t.mid(1, ...)),
//                 leaving map65a.f90's I3 kHz at 0-2 and I5 df in Hz at 3-7.
//                 q65b.F90 emits the same first six fields, so one reader
//                 covers JT65 and Q65 alike.
//   Messages      the "@" marker is stripped (m_messagesText += t.mid(1)) and
//                 Messages::setText then appends t1.mid(5, 67) -- shifting the
//                 line left by five -- which lands the LAST THREE characters of
//                 display.f90's f8.3 field, i.e. the kHz, at 0-2, and the i5 df
//                 at 3-7. messages.cpp reads the same three characters as
//                 "cfreq" a few lines above, and the existing UTC extraction
//                 (t2.mid(13,2) / mid(15,2)) confirms the same shift.
//
// Reading the f8.3 field as MHz was the 2026-08-19 regression: the Messages
// window never shows it. "124" was taken for 124 MHz on 144, and "080" for
// 80 MHz on 1296 -- exactly the -20 MHz and 80 MHz Uwe reported.
bool decodeLineFreqKHz(QString const& line, double* kHz)
{
  bool okk = false, okd = false;
  const int k = line.mid(0, 3).trimmed().toInt(&okk);
  const int d = line.mid(3, 5).trimmed().toInt(&okd);
  // A Fortran I3 overflows to "***" past 999 kHz. That parse fails here and the
  // caller keeps its previous fallback rather than relaying a fabricated number
  // -- a wrong frequency is worse than a truncated one.
  if (!okk || !okd || k < 0 || k > 999) return false;
  *kHz = k + d / 1000.0;
  return true;
}

}  // namespace

// under the cursor is extracted once, robustly (regardless of x-position
// or where exactly the cursor landed), and passed on as-is.
void MainWindow::selectCall2(bool ctrl, bool isDoubleClick)     //selectCall2
{
  // Single click always transfers the call. The "Enable click-to-work"
  // toggle gates only the double click (its extra Tx-enable step), so a
  // double click does nothing when the toggle is off -- the single-click
  // press that precedes it still populates DX Call.
  if (isDoubleClick && !ui->actionEnable_Click_to_Work->isChecked()) return;
  QString t = ui->decodedTextBrowser->toPlainText();
  int i = ui->decodedTextBrowser->textCursor().position();
  // Search strictly before i, not at-or-before: a double-click's default
  // word-selection can leave the cursor exactly on a line-separator
  // character (see the identical BandMap fix), which would otherwise
  // produce a start-past-end range and glob the rest of the buffer.
  int i0 = (i > 0) ? (t.lastIndexOf("\n", i - 1) + 1) : 0;
  int i1 = t.indexOf("\n", i);
  if (i1 < 0) i1 = t.length();
  QString line = t.mid(i0, i1 - i0);
  if (!line.trimmed().isEmpty()) doubleClickOnCall(line, ctrl, isDoubleClick);
}
                                                          //doubleClickOnCall
void MainWindow::doubleClickOnCall(QString rawLine, bool ctrl, bool isDoubleClick)
{
  // Calls + grid are always in columns after position 30 for MAP65's main
  // JT65 decode lines; words[1] is the sender (line "owner"), words[2] the
  // grid, matching the same layout the Universal Decode Label Parser and
  // parseMessageLine() assume for the other decode-line formats.
  auto const& words = rawLine.mid(30).split(' ', SkipEmptyParts);
  if (words.size() < 2) return;
  QString hiscall = words[1].toUpper();
  if (hiscall.length() < 3) return;

  if(m_worked[hiscall]) {
    msgBox("Possible dupe: " + hiscall + " already in log.");
  }
  ui->dxCallEntry->setText(hiscall);
  int n = 60*rawLine.mid(14,2).toInt() + rawLine.mid(16,2).toInt();
  m_txFirst = ((n%2) == 1);
  ui->txFirstCheckBox->setChecked(m_txFirst);
  if((rawLine.indexOf("#")>0) and m_modeTx!="JT65") on_pbTxMode_clicked();
  if((rawLine.indexOf(":")>0) and m_modeTx!="Q65") on_pbTxMode_clicked();

  QString grid = words.size() > 2 ? words[2] : QString();
  if(isGrid4(grid)) {
    ui->dxGridEntry->setText(grid);
  } else {
    lookup();
  }

  QString rpt="";
  if(ctrl or m_modeTx=="Q65") rpt=rawLine.mid(25,3);
  genStdMsgs(rpt);
  if(rawLine.indexOf(m_myCall)>0) {
    m_ntx=2;
    ui->txrb2->setChecked(true);
  } else {
    m_ntx=1;
    ui->txrb1->setChecked(true);
  }

  // Both local (above) AND relay to WSJT-X when Sync is on. Frequency from
  // the bandmap side-channel; decode period from m_txFirst; mode from the
  // line ("#" = JT65, ":" = Q65).
  double clickKHz = 0.0;
  if (!decodeLineFreqKHz(rawLine, &clickKHz))
    clickKHz = m_bandmapFreq.value(hiscall, 0.0);   // pre-existing fallback
  relayClickToWsjtx(hiscall, clickKHz, grid,
                    clickModeDesignator(rawLine.indexOf("#") > 0, hiscall),
                    clickEvenPeriod(hiscall), isDoubleClick);
  forwardCachedDecode(hiscall);   // push this station's decode to WSJT-X on the click
  if (isDoubleClick && !(m_syncWsjtx && m_txViaWsjtx) && !m_auto)
    on_autoButton_clicked();
}
                                                      //doubleClickOnMessages
void MainWindow::doubleClickOnMessages(QString t2, bool ctrl, bool isDoubleClick)
{
  // Single click always transfers the call. The "Enable click-to-work"
  // toggle gates only the double click (its extra Tx-enable step), so a
  // double click does nothing when the toggle is off -- the single-click
  // press that precedes it still populates DX Call.
  if (isDoubleClick && !ui->actionEnable_Click_to_Work->isChecked()) return;

  // Whole-line extraction: the "owner" of the line (sender) + grid are
  // resolved from the line's own content, not from whichever word happens
  // to be under the cursor -- a click anywhere on the line does the same
  // thing.
  QString hiscall, grid;
  if (!parseMessageLine(t2, hiscall, grid)) return;
  if (hiscall.length() < 3) return;

  if(m_worked[hiscall]) {
    msgBox("Possible dupe: " + hiscall + " already in log.");
  }
  ui->dxCallEntry->setText(hiscall);
  int n = 60*t2.mid(13,2).toInt() + t2.mid(15,2).toInt();
  m_txFirst = ((n%2) == 1);
  ui->txFirstCheckBox->setChecked(m_txFirst);

  if((t2.indexOf(":")<0) and m_modeTx!="JT65") on_pbTxMode_clicked();
  if((t2.indexOf(":")>0) and m_modeTx!="Q65") on_pbTxMode_clicked();

  if (!grid.isEmpty()) {
    ui->dxGridEntry->setText(grid);
  } else {
    lookup();
  }

  QString rpt="";
  if(ctrl or m_modeTx=="Q65") rpt=t2.mid(20,3);
  genStdMsgs(rpt);

  if(t2.indexOf(m_myCall)>0) {
    m_ntx=2;
    ui->txrb2->setChecked(true);
  } else {
    m_ntx=1;
    ui->txrb1->setChecked(true);
  }

  // Both local (above) AND relay to WSJT-X when Sync is on. Frequency from
  // the bandmap side-channel; decode period from m_txFirst; mode from the
  // line (":" present = Q65, else JT65).
  double clickKHz = 0.0;
  // Messages::setText blanks the kHz on a line whose frequency repeats the one
  // above it, so those three characters can be empty. The parse then fails and
  // the old per-callsign lookup applies, which is what happened before anyway.
  if (!decodeLineFreqKHz(t2, &clickKHz))
    clickKHz = m_bandmapFreq.value(hiscall, 0.0);   // pre-existing fallback
  relayClickToWsjtx(hiscall, clickKHz, grid,
                    clickModeDesignator(t2.indexOf(":") < 0, hiscall),
                    clickEvenPeriod(hiscall), isDoubleClick);
  forwardCachedDecode(hiscall);   // push this station's decode to WSJT-X on the click
  if (isDoubleClick && !(m_syncWsjtx && m_txViaWsjtx) && !m_auto)
    on_autoButton_clicked();
}

                                                    //handleCallsignClick
void MainWindow::handleCallsignClick(const QString& call, double freq_khz, bool is_jt65, const QString& grid, bool start_qso)
{
  // Single click (start_qso=false) always transfers the call. A double
  // click (start_qso=true) is gated by "Enable click-to-work": when the
  // toggle is off it does nothing here -- the preceding single-click
  // press already populated DX Call.
  if (start_qso && !ui->actionEnable_Click_to_Work->isChecked()) return;
  if (call.isEmpty()) return;
  if (m_worked[call]) {
    msgBox("Possible dupe: " + call + " already in log.");
  }
  ui->dxCallEntry->setText(call);
  if (is_jt65 && m_modeTx != "JT65") on_pbTxMode_clicked();
  if (!is_jt65 && m_modeTx != "Q65") on_pbTxMode_clicked();
  if (isGrid4(grid)) {
    ui->dxGridEntry->setText(grid);
  } else {
    lookup();  // populates dxGridEntry via CALL3.TXT, or clears it if unknown
  }
  genStdMsgs("");

  // Both local (above) AND relay to WSJT-X when Sync is on. Mode designator
  // (JT65 vs Q65 sub-mode) lets WSJT-X switch mode to match. The waterfall
  // click carries no decode period, so leave it default (0).
  relayClickToWsjtx(call, freq_khz, grid, clickModeDesignator(is_jt65, call),
                    clickEvenPeriod(call), start_qso);
  forwardCachedDecode(call);   // push this station's decode to WSJT-X on the click

  // Double-click Tx routing: exactly one app keys up. When Sync is on and
  // the Tx target is WSJT-X, WSJT-X keys (relay set start_qso=1 for it);
  // otherwise MAP65 keys its own Tx.
  if (start_qso && !(m_syncWsjtx && m_txViaWsjtx) && !m_auto)
    on_autoButton_clicked();
}

bool MainWindow::rxMutedByWsjtx() const
{
  return m_bWTransmitting && m_syncWsjtx && !ui->actionContinuous_waterfall->isChecked();
}

// Zero the raw I/Q frames k0..k-1 in dd[] (4 floats per frame: xi xq yi yq),
// the stretch that arrived while WSJT-X was keyed. A wrap to a new minute
// (k < k0) is left alone; the fresh minute starts clean anyway.
void MainWindow::zapWsjtxTx(int k0, int k)
{
  if(!dd || k <= k0 || k0 < 0) return;
  std::fill(dd + 4*static_cast<size_t>(k0), dd + 4*static_cast<size_t>(k), 0.0f);
}

void MainWindow::appendFwdLine(const QString& line)
{
  if (!m_fwdShm || !m_memFwd.isAttached() || line.isEmpty()) return;
  const QByteArray lb = line.toLatin1();
  m_memFwd.lock();
  const int idx = ((m_fwdShm->write_seq % QMAPFWD_CAP) + QMAPFWD_CAP) % QMAPFWD_CAP;
  std::memset(m_fwdShm->lines[idx], 0, QMAPFWD_LINE);
  std::memcpy(m_fwdShm->lines[idx], lb.constData(),
              std::min<size_t>(lb.size(), QMAPFWD_LINE - 1));
  m_fwdShm->write_seq += 1;
  m_memFwd.unlock();
  std::fprintf(stderr, "[map65] fwd -> WSJT-X #%d: %s\n", m_fwdShm->write_seq, lb.constData());
}

// True the first time a given decode (HHMM|message) is forwarded this
// session, false on every re-emission of the same line. The decoder's
// display() rewrites the complete kept list -- every decode of the last
// nkeep minutes, age stamped -- on EVERY cycle, and "@" (wideband list) and
// "!" (narrowband) both carry a fresh decode, so a line can reach this
// function many times. WSJT-X de-dupes per period on the line's OWN
// timestamp, which is exactly what an aged line defeats.
bool MainWindow::fwdFirstTime(const QString& hhmm, const QString& msg)
{
  const QString key = hhmm + "|" + msg.simplified().toUpper();
  const int nowMin = QDateTime::currentDateTimeUtc().time().hour() * 60
                   + QDateTime::currentDateTimeUtc().time().minute();
  if (m_fwdSentKeys.contains(key)) return false;
  if (m_fwdSentKeys.size() > 3000) {
    // Drop everything older than two hours; the kept list never reaches back
    // that far, so nothing the decoder can still re-emit is forgotten.
    for (auto it = m_fwdSentKeys.begin(); it != m_fwdSentKeys.end();) {
      const int age = ((nowMin - it.value()) % 1440 + 1440) % 1440;
      if (age > 120) it = m_fwdSentKeys.erase(it); else ++it;
    }
  }
  m_fwdSentKeys.insert(key, nowMin);
  return true;
}

void MainWindow::forwardDecodeToWsjtx(const QString& rawLine)
{
  if (!m_syncWsjtx) return;
  // MAP65 emits its decodes on two streams, and they are NOT copies of one
  // another:
  //   "@"  the wideband list (display.f90:190) -- what the Messages window
  //        shows. Fed only by decode pass 2 (nqd=0).
  //   "!"  the narrowband decodes at fQSO +/- ntol, written directly by
  //        map65a.f90:473 / q65b.F90:316 during pass 1 (nqd=1) -- what the
  //        main decoded-text window shows.
  // map65a.f90:219 resets km at the top of EVERY pass, so a pass-1 decode
  // never survives into display(), and the cross-pass dupe guard
  // (map65a.f90:274) then stops pass 2 from finding the same signal again.
  // Keying off "@" alone therefore dropped precisely the decodes at the QSO
  // frequency -- the ones that matter most for sequencing. Accept both.
  //
  // Duplicates need no handling here: WSJT-X de-dupes forwarded lines on
  // (HHMM, message words) before displaying or sequencing them
  // (widgets/mainwindow.cpp:9253 and :10005). So if the Fortran side is later
  // fixed to feed pass-1 decodes into display() as well, the second copy is
  // dropped there rather than reaching the sequencer twice.
  QString hhmm, msg, lastTok;
  int snr = 0;
  double kHz = 0.0;
  bool haveFreq = false;
  // The mode char must match what WSJT-X's processMessage expects for the mode
  // it is in (widgets/mainwindow.cpp: Q65 requires ':', JT65 requires '#') --
  // a mismatch is dropped at that gate and never reaches the auto-sequencer.
  char modeChar = ':';

  if (rawLine.startsWith("@")) {
    //   @FFFF.fff  NDF  flag  HHMM  SNR  MSG...  MARKER  DT  X:submode
    // MARKER is ":" for Q65, "#" for JT65 -- it separates the message from the
    // trailing DT/sub-mode and tells us the mode.
    const QStringList tk = rawLine.mid(1).split(QRegularExpression("\\s+"), SkipEmptyParts);
    if (tk.size() < 7) return;
    hhmm = tk[3];
    bool ok = false;
    snr = tk[4].toInt(&ok);
    if (!ok) return;
    int mark = -1;
    for (int i = 5; i < tk.size(); ++i) {
      if (tk[i] == ":") { mark = i; modeChar = ':'; break; }   // Q65
      if (tk[i] == "#") { mark = i; modeChar = '#'; break; }   // JT65
    }
    if (mark < 6) return;
    msg = QStringList(tk.mid(5, mark - 5)).join(" ").trimmed();
    if (msg.isEmpty()) return;
    bool okf0 = false, okdf = false;
    const double f0m = tk[0].toDouble(&okf0);
    const double dfz = tk[1].toDouble(&okdf);
    if (okf0 && okdf) {
      kHz = (f0m - std::floor(f0m)) * 1000.0 + dfz / 1000.0;
      haveFreq = true;
    }
    lastTok = tk.last();
  } else {
    BangDecode d;
    if (!parseBangLine(rawLine, d)) return;
    hhmm = d.hhmm; msg = d.msg; snr = d.snr; kHz = d.kHz; modeChar = d.modeChar;
    haveFreq = true;
    // A "!" line carries no sub-mode: map65a's cm is the constant '#'
    // (map65a.f90:94) and q65b's A3 slot holds a CQ flag, not the sub-mode.
    // lastTok stays empty, so meta.submode is left unset below and a later
    // click falls back to the current sub-mode (see clickModeDesignator).
  }
  // Build a WSJT-X DecodedText line: time(0) snr(5) dt(9) freq(14) mode(19)
  // msg(22). Audio freq is ignored by WSJT-X for forwarded lines (the click
  // sets the dial) -- use a fixed mid-passband placeholder.
  const QByteArray hb = hhmm.toLatin1();
  const QByteArray mb = msg.toLatin1();
  int RxFreq = 1500;
  if (modeChar != ':') RxFreq = 1270;
  const QString line = QString::asprintf("%-4.4s %3d %4.1f %4d %c  %s",
      hb.constData(), snr, 0.0, RxFreq, modeChar, mb.constData());
  // Cache the line by every callsign in the message, so a later click on any
  // of those calls can forward this decode immediately (click-to-forward).
  // The leading (prefix/) group is what makes ER/EA8DBM a callsign here.
  // Without it the pattern only allowed a trailing suffix, so EA8DBM/P was
  // recognised and ER/EA8DBM was not -- and a token that is not recognised
  // never reaches m_fwdLineByCall below. A later click on that station then
  // found no cached line to forward, so WSJT-X received the callsign, the
  // frequency and the mode from the click channel and generated Tx1-Tx6,
  // but the message line itself never arrived. It appeared to work
  // "sometimes" because once the DX Call is set the continuous-forward path
  // below matches on the plain word instead and does not consult this.
  //
  // This pattern is repeated six times in this file; all six were fixed
  // together, and they should really be one shared constant.
  static const QRegularExpression callre("^([A-Z0-9]{1,4}/)?[A-Z0-9]{1,3}[0-9][A-Z0-9]{0,3}[A-Z](/[A-Z0-9]+)?$");
  const QStringList words = msg.toUpper().split(QRegularExpression("\\s+"), SkipEmptyParts);
  for (const QString& w : words) if (callre.match(w).hasMatch()) m_fwdLineByCall[w] = line;
  // Record this decode's own sub-mode + period so a later click on any surface
  // can relay them (see clickModeDesignator / clickEvenPeriod). The line's last
  // token is display.f90's iage+cmode, e.g. "0#A" (JT65 sub-mode A) or "0:C"
  // (Q65 sub-mode C); HHMM gives the minute the station transmitted in.
  DecodeMeta meta;
  if (!lastTok.isEmpty()) {
    static const QRegularExpression cmodere("^\\d*[#:]([A-E])$");
    const auto cm = cmodere.match(lastTok);
    if (cm.hasMatch()) meta.submode = cm.captured(1).at(0).toLatin1();
  }
  if (hhmm.size() >= 3) {
    bool okh=false, okm=false;
    const int hh = hhmm.left(hhmm.size()-2).toInt(&okh);
    const int mm = hhmm.right(2).toInt(&okm);
    if (okh && okm) meta.minute = 60*hh + mm;      // minutes since 00:00, as display.f90's utc
  }
  if (meta.submode || meta.minute >= 0)
    for (const QString& w : words) if (callre.match(w).hasMatch()) m_decodeMeta[w] = meta;
  // Remember every frequency each station has been decoded on this session, so
  // a shorthand can be matched against the NEAREST of them. A single
  // last-write-wins value is not enough: the same station shows up on more than
  // one frequency (images/aliases -- e.g. DL3WDG at both 144.124 and 144.134),
  // and the stale one would defeat the Tol test.
  if (haveFreq) recordCallFreqs(words, kHz);
  // Continuous forward: only decodes involving the currently-selected DX Call,
  // and each decode once -- the aged re-listings must not cross again.
  const QString dx = ui->dxCallEntry->text().trimmed().toUpper();
  if (!dx.isEmpty() && words.contains(dx)) {
    if (fwdFirstTime(hhmm, msg)) appendFwdLine(line);
    return;
  }

  // --- JT65 shorthand (RO/RRR/73) ------------------------------------------
  // Shorthand is a bare tone pair: map65a.f90:303 sets the message to the token
  // ALONE, with no callsign, so the DX-Call test above can never match it --
  // yet RO/RRR/73 are exactly the QSO-completing exchanges. Shorthand is
  // therefore ALWAYS forwarded (the m_syncWsjtx entry gate is the only gate):
  // a bare token shows up in WSJT-X's Band Activity as receive diversity and
  // is ignored by the QSO logic (processMessage returns on
  // message_words.size()<3). With "Add Call to SH" (View menu) enabled and
  // the sender inferable by frequency proximity to the DX Call, the line is
  // instead synthesised in WSJT-X's "<to> <from>" shape -- "<my call>
  // <dx call> RO" reads as "addressed to me, from the DX", which the
  // auto-sequencer acts on.
  const QString sh = msg.trimmed().toUpper();
  if (modeChar != '#') return;
  if (sh != "RO" && sh != "RRR" && sh != "73") return;
  if (!fwdFirstTime(hhmm, msg + " " + QString::number(kHz, 'f', 1))) return;   // once per period and frequency
  if (m_addCallToSh && !dx.isEmpty() && !m_myCall.trimmed().isEmpty()) {
    if (haveFreq && inferShorthandCall(kHz) == dx) {
      const QByteArray sb = QString("%1 %2 %3")
          .arg(m_myCall.trimmed().toUpper(), dx, sh).toLatin1();
      // Provenance marker: a leading '*' tells WSJT-X this line's callsigns
      // were INFERRED, not received, so it can render them inverted. Carried
      // in-band rather than as a new QmapFwd field -- the struct is
      // hand-mirrored in widgets/mainwindow.cpp and a layout change would
      // need both sides in step. WSJT-X strips the marker before parsing.
      appendFwdLine(QString::asprintf("*%-4.4s %3d %4.1f %4d %c  %s",
          hb.constData(), snr, 0.0, 1270, modeChar, sb.constData()));
      return;
    }
  }
  appendFwdLine(line);   // bare shorthand: display-only in WSJT-X
}

void MainWindow::recordCallFreqs(const QStringList& words, double kHz)
{
  // JT65 sync-tone audio offset correction (same used for shorthands)
  static constexpr double kJT65AudioOffsetKHz = 1.2705;
  kHz -= kJT65AudioOffsetKHz;
  // Remember every frequency each station has been decoded on, so a shorthand
  // can be matched against the NEAREST of them.
  static const QRegularExpression cre("^([A-Z0-9]{1,4}/)?[A-Z0-9]{1,3}[0-9][A-Z0-9]{0,3}[A-Z](/[A-Z0-9]+)?$");
  for (const QString& w : words) {
    if (!cre.match(w).hasMatch()) continue;
    QVector<double>& v = m_callFreqKHz[w];
    bool seen = false;
    for (double f : v) if (std::fabs(f - kHz) < 0.05) { seen = true; break; }   // <50 Hz = same spot
    if (!seen) { v.append(kHz); if (v.size() > 8) v.removeFirst(); }
  }
}

QString MainWindow::inferShorthandCall(double shKHzRaw) const
{
  // Shorthand carries no callsign, so the sender is inferred from frequency.
  // First remove the JT65 nominal sync-tone audio offset: map65a.f90:273
  // derives fshort straight from an FFT bin (0.001*(i0-16385)*df), i.e. raw
  // baseband INCLUDING the audio offset, whereas decode1a reports normal
  // decodes already referenced to the signal's nominal position (:364). Without
  // this every shorthand sits ~1270 Hz high and can never match its own station.
  static constexpr double kJT65AudioOffsetKHz = 1.2705;   // 1270.5 Hz (Q65 would be 1.5)
  const double shKHz = shKHzRaw - kJT65AudioOffsetKHz;
  const QString dx = ui->dxCallEntry->text().trimmed().toUpper();
  if (dx.isEmpty()) return QString();
  // Nearest frequency this station has been heard on, not the most recent.
  double bestHz = -1.0;
  for (double f : m_callFreqKHz.value(dx)) {
    const double dHz = std::fabs(shKHz - f) * 1000.0;
    if (bestHz < 0.0 || dHz < bestHz) bestHz = dHz;
  }
  if (bestHz < 0.0 && m_bandmapFreq.contains(dx))         // fall back to the bandmap value
    bestHz = std::fabs(shKHz - m_bandmapFreq.value(dx)) * 1000.0;
  if (bestHz < 0.0) return QString();                     // no known frequency for the DX yet
  return (bestHz <= double(m_tol)) ? dx : QString();
}

void MainWindow::forwardCachedDecode(const QString& call)
{
  if (!m_syncWsjtx) return;
  const QString c = call.trimmed().toUpper();
  if (m_fwdLineByCall.contains(c)) appendFwdLine(m_fwdLineByCall.value(c));
}

QString MainWindow::clickModeDesignator(bool is_jt65, const QString& call) const
{
  // Prefer the clicked decode's OWN sub-mode (captured from its "@" line) over
  // whatever the menus currently say -- the wideband list can hold stations
  // heard at a different sub-mode than the one now selected. Fall back to the
  // menu setting when the decode predates the cmode marker.
  char sub = 0;
  const auto it = m_decodeMeta.constFind(call.trimmed().toUpper());
  if (it != m_decodeMeta.constEnd()) sub = it->submode;
  if (is_jt65) {
    // JT65 sub-mode A/B/C -> "JT65A".."JT65C"; WSJT-X's applyQmapClickMode
    // sets its sbSubmode from the letter. m_modeJT65 is 1..3 = A..C.
    if (sub < 'A' || sub > 'C')
      sub = (m_modeJT65 >= 1 && m_modeJT65 <= 3) ? char('A' + m_modeJT65 - 1) : 0;
    return sub ? QStringLiteral("JT65") + QChar(sub) : QStringLiteral("JT65");
  }
  // Q65: "<period><letter>" from the TR period + sub-mode (A..E = m_modeQ65 1..5).
  if (sub < 'A' || sub > 'E')
    sub = (m_modeQ65 >= 1 && m_modeQ65 <= 5) ? char('A' + (m_modeQ65 - 1)) : 0;
  if (!sub) return QString();
  return QString::number(m_TRperiod) + QChar(sub);
}

int MainWindow::clickEvenPeriod(const QString& call) const
{
  // Which period the DECODED station transmitted in (1 = even/1st). WSJT-X
  // flips its own Tx Even/1st to the opposite so the reply lands in the right
  // slot -- on EME both sequences are in use at once, so this must come from
  // the decode, not from our own Tx setting. Derived from the decode's UTC
  // minute; HHMM can't resolve 15 s / 30 s periods, so those keep the old
  // behaviour of mirroring MAP65's own Tx sequence.
  const auto it = m_decodeMeta.constFind(call.trimmed().toUpper());
  if (it != m_decodeMeta.constEnd() && it->minute >= 0 &&
      m_TRperiod >= 60 && m_TRperiod % 60 == 0) {
    const int mins = m_TRperiod / 60;               // 60->1, 120->2, 300->5
    return ((it->minute / mins) % 2 == 0) ? 1 : 0;
  }
  return m_txFirst ? 0 : 1;
}

void MainWindow::relayClickToWsjtx(const QString& call, double freq_khz,
                                   const QString& grid, const QString& mode,
                                   int even_period, bool start_qso)
{
  if (!m_syncWsjtx || !m_qmapShm || !m_memQmap.isAttached()) return;
  qint64 ms=QDateTime::currentMSecsSinceEpoch();
  // Adaptive polarization reports 0..1000 kHz but uses a wrapped -500..+500
  // scale; unwrap before handing the frequency to WSJT-X (mirrors the
  // waterfall-label wrap in the Universal Decode Label Parser).
  if (m_xpol && freq_khz > 500.0) freq_khz -= 1000.0;
  // MAP65's frequency scale is JT65-heritage for BOTH modes: a displayed
  // freq F means the station's tone sits at F + 1.27046 kHz (map65a.f90
  // foffset and q65b.F90 nq65df both reference 1270.46 Hz). WSJT-X expects
  // dial = tone - nominal audio, so a JT65 click relays F unchanged (nominal
  // 1270.46) but a Q65 click must be re-referenced to the 1500 Hz nominal or
  // the reply lands 229.5 Hz high. QMAP needs none of this: its fsked is
  // already 1500-referenced (q65b.F90: fsked = frx - 1.5).
  const bool click_is_jt65 = mode.trimmed().toUpper().startsWith("JT65");
  if (!click_is_jt65) freq_khz -= (1.5 - 1.27046);
  // Absolute RF Hz = MAP65 centre freq (MHz, floored to the band) + the
  // label's audio offset (kHz). WSJT-X derives its own audio offset from
  // this regardless of which slice it's on. Mirrors QMAP's rf_hz math.
  const double base_hz = std::floor(getFcenter()) * 1000000.0;
  const qint64 rf_hz = static_cast<qint64>(base_hz + freq_khz * 1000.0 + (freq_khz >= 0 ? 0.5 : -0.5));
  const int wsjtxStart = (m_txViaWsjtx && start_qso) ? 1 : 0;
  auto putStr = [](char* dst, int cap, const QString& s) {
    std::memset(dst, 0, cap);
    const QByteArray b = s.toUtf8();
    std::memcpy(dst, b.constData(), std::min<size_t>(b.size(), static_cast<size_t>(cap) - 1));
  };
  if ((ms-m_msErase)<500 or m_txViaWsjtx) {
    m_memQmap.lock();
    putStr(m_qmapShm->click_callsign, sizeof(m_qmapShm->click_callsign), call);
    // Only relay something that IS a grid. Every caller takes the grid from
    // a fixed column of the decode line -- words[2] of rawLine.mid(30) -- and
    // each already guards its OWN field with isGrid4 before using it, but
    // passed the raw token here. On a message that carries no grid that
    // column is the next field along: clicking "CQ ER/EA8DBM" relayed the
    // RC value and WSJT-X showed a DX Grid of "1".
    //
    // Guarding here rather than at the four call sites, so a fifth cannot
    // reintroduce it. An empty grid is correct and harmless: WSJT-X keeps
    // whatever it had, exactly as when MAP65 falls through to lookup().
    putStr(m_qmapShm->click_grid,     sizeof(m_qmapShm->click_grid),
           isGrid4(grid) ? grid : QString());
    putStr(m_qmapShm->click_mode,     sizeof(m_qmapShm->click_mode),     mode);
    m_qmapShm->click_rf_hz        = rf_hz;
    m_qmapShm->click_even_period  = even_period;   // WSJT-X sets txFirst = !even_period
    m_qmapShm->click_start_qso    = wsjtxStart;
    m_qmapShm->click_seq         += 1;   // WSJT-X edge-detects this
    const int seq = m_qmapShm->click_seq;
    m_memQmap.unlock();
    std::fprintf(stderr, "[map65] relayed click call=%s rf=%lld startQso=%d\n",
                 call.toUtf8().constData(), static_cast<long long>(rf_hz), wsjtxStart);
    // The start-QSO intent must not outlive the click. WSJT-X consumes a
    // click within 100 ms; two seconds later the flag has either done its
    // job or the gate was closed and it never will. Clear it -- WITHOUT
    // touching click_seq, so nothing re-fires -- unless a newer click has
    // replaced this record meanwhile. Otherwise a double-click made while
    // WSJT-X sat in FT8 stays in the segment as "start QSO" indefinitely.
    if (wsjtxStart) {
      QTimer::singleShot(2000, this, [this, seq] {
        if (!m_qmapShm || !m_memQmap.isAttached()) return;
        m_memQmap.lock();
        if (m_qmapShm->click_seq == seq) m_qmapShm->click_start_qso = 0;
        m_memQmap.unlock();
      });
    }
  }
  m_msErase=ms;
}

                                              //handleBandMapCallsignClick
void MainWindow::handleBandMapCallsignClick(const QString& call, bool isDoubleClick)
{
  // Single click always transfers the call. The "Enable click-to-work"
  // toggle gates only the double click (its extra Tx-enable step), so a
  // double click does nothing when the toggle is off -- the single-click
  // press that precedes it still populates DX Call.
  if (isDoubleClick && !ui->actionEnable_Click_to_Work->isChecked()) return;
  if (call.isEmpty()) return;
  if (m_worked[call]) {
    msgBox("Possible dupe: " + call + " already in log.");
  }
  ui->dxCallEntry->setText(call);
  if (m_bandmapMode.contains(call)) {
    bool is_jt65 = m_bandmapMode.value(call);
    if (is_jt65 && m_modeTx != "JT65") on_pbTxMode_clicked();
    if (!is_jt65 && m_modeTx != "Q65") on_pbTxMode_clicked();
  }
  QString grid = m_bandmapGrid.value(call);
  if (isGrid4(grid)) {
    ui->dxGridEntry->setText(grid);
  } else {
    lookup();  // populates dxGridEntry via CALL3.TXT, or clears it if unknown
  }
  genStdMsgs("");

  // Both local (above) AND relay to WSJT-X when Sync is on. Frequency comes
  // from the side-channel map; mode from the bandmap's is_jt65 side-channel.
  relayClickToWsjtx(call, m_bandmapFreq.value(call, 0.0), grid,
                    clickModeDesignator(m_bandmapMode.value(call, false), call),
                    clickEvenPeriod(call), isDoubleClick);
  forwardCachedDecode(call);   // push this station's decode to WSJT-X on the click

  // Double-click Tx routing: WSJT-X keys when Sync+TxViaWSJTX, else MAP65.
  if (isDoubleClick && !(m_syncWsjtx && m_txViaWsjtx) && !m_auto)
    on_autoButton_clicked();
}

void MainWindow::genStdMsgs(QString rpt)                       //genStdMsgs()
{
  if(rpt.left(2)==" -") rpt="-0"+rpt.mid(2,1);
  if(rpt.left(2)==" +") rpt="+0"+rpt.mid(2,1);
  QString hiscall=ui->dxCallEntry->text().toUpper().trimmed();
  ui->dxCallEntry->setText(hiscall);
  QString t0=hiscall + " " + m_myCall + " ";
  QString t=t0;
  if(t0.indexOf("/")<0) t=t0 + m_myGrid.mid(0,4);
  msgtype(t, ui->tx1);
  if(rpt == "" and m_modeTx=="Q65") rpt="-24";
  if(rpt == "" and m_modeTx=="JT65") {
    t=t+" OOO";
    msgtype(t, ui->tx2);
    msgtype("RO", ui->tx3);
    msgtype("RRR", ui->tx4);
    msgtype("73", ui->tx5);
  } else {
    t=t0 + rpt;
    msgtype(t, ui->tx2);
    t=t0 + "R" + rpt;
    msgtype(t, ui->tx3);
    t=t0 + "RRR";
    msgtype(t, ui->tx4);
    t=t0 + "73";
    msgtype(t, ui->tx5);
  }
  t="CQ " + m_myCall + " " + m_myGrid.mid(0,4);
  msgtype(t, ui->tx6);
  m_ntx=1;
  ui->txrb1->setChecked(true);
}

// CALL3.TXT lives in the writable data dir. If it is not there yet, seed it
// once by copying a legacy one: first the bundled copy beside the exe, then
// the old JTSDK install dir. Every CALL3 consumer goes through this accessor.
// Zero-byte files count as absent on BOTH sides: the decoder's
// open(status='unknown') leaves empty strays behind, which must neither be
// seeded from nor allowed to block seeding.
QString MainWindow::call3Path() const
{
  // User-picked location wins outright: no seeding, no zero-byte rescue --
  // if the operator points at a file, that file is the callbook, and an
  // empty one simply means "no entries yet". Only the default (data-dir)
  // location gets the legacy seed-from-appDir/JTSDK treatment below.
  if (!m_call3PathUser.isEmpty()) return m_call3PathUser;

  QString writablePath = QDir {m_dataDir}.absoluteFilePath("CALL3.TXT");
  if (QFileInfo {writablePath}.size() <= 0) {
    for (auto const& legacyDir : {m_appDir, legacyJtsdkDir}) {
      QString legacyPath = QDir {legacyDir}.absoluteFilePath("CALL3.TXT");
      if (QFileInfo {legacyPath}.size() > 0) {
        QFile::remove(writablePath);   // clear an empty stray; copy won't overwrite
        QFile::copy(legacyPath, writablePath);
        break;
      }
    }
  }
  return writablePath;
}

// Push the resolved CALL3.TXT path down to the decoder (npar_ptrs_mod
// call3_path -> map65a's open of unit 23). Called at startup and whenever
// Settings changes the path, so deep search and the GUI never diverge.
void MainWindow::pushCall3PathToDecoder() const
{
  QByteArray p = QDir::toNativeSeparators(call3Path()).toLocal8Bit();
  set_call3_path_(p.constData(), p.size());
}

void MainWindow::lookup()                                       //lookup()
{
  QString hiscall=ui->dxCallEntry->text().toUpper().trimmed();
  ui->dxCallEntry->setText(hiscall);
  QString call3File = call3Path();
  QFile f(call3File);
  if(!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
    msgBox("Cannot open " + call3File);
    return;
  }
  char c[132];
  qint64 n=0;
  for(int i=0; i<999999; i++) {
    n=f.readLine(c,sizeof(c));
    if(n <= 0) {
      ui->dxGridEntry->setText("");
      break;
     }
    QString t=QString(c);
    if(t.indexOf(hiscall)==0) {
      int i1=t.indexOf(",");
      QString hisgrid=t.mid(i1+1,6);
      i1=hisgrid.indexOf(",");
      if(i1>0) {
        hisgrid=hisgrid.mid(0,4);
      } else {
        hisgrid=hisgrid.mid(0,4) + hisgrid.mid(4,2).toLower();
      }
      ui->dxGridEntry->setText(hisgrid);
      break;
    }
  }
  f.close();
}

void MainWindow::on_lookupButton_clicked()                    //Lookup button
{
  lookup();
}

void MainWindow::on_addButton_clicked()                       //Add button
{
  if(ui->dxGridEntry->text()=="") {
    msgBox("Please enter a valid grid locator.");
    return;
  }
  m_call3Modified=false;
  QString hiscall=ui->dxCallEntry->text().toUpper().trimmed();
  QString hisgrid=ui->dxGridEntry->text().trimmed();
  QString newEntry=hiscall + "," + hisgrid;

  int ret = QMessageBox::warning(this, "Add",
       newEntry + "\n" + "Is this station known to be active on EME?",
       QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
  if(ret==QMessageBox::Yes) {
    newEntry += ",EME,,";
  } else {
    newEntry += ",,,";
  }
  QString call3File = call3Path();
  QFile f1(call3File);
  if(!f1.open(QIODevice::ReadWrite | QIODevice::Text)) {
    msgBox("Cannot open " + call3File);
    return;
  }

  if(f1.size()==0) {
    QTextStream out(&f1);
    out << "ZZZZZZ"
#if QT_VERSION >= QT_VERSION_CHECK (5, 15, 0)
        << Qt::endl
#else
        << endl
#endif
      ;
    f1.seek (0);
  }

  // Temp file goes beside the target CALL3.TXT: the rename/replace below
  // must stay on the same volume, which a hardcoded data-dir path breaks
  // as soon as the operator points CALL3.TXT at another drive.
  QString tmpFile = QFileInfo {call3File}.absolutePath() + "/CALL3.TMP";
  QFile f2(tmpFile);
  if(!f2.open(QIODevice::ReadWrite | QIODevice::Truncate | QIODevice::Text)) {
    msgBox("Cannot open " + tmpFile);
    return;
  }
  {
    QTextStream in(&f1);
    QTextStream out(&f2);
    QString hc=hiscall;
    QString hc1="";
    QString hc2="000000";
    QString s;
    do {
      s=in.readLine();
      hc1=hc2;
      if(s.mid(0,2)=="//") {
        out << s + "\n";
      } else {
        int i1=s.indexOf(",");
        hc2=s.mid(0,i1);
        if(hc>hc1 && hc<hc2) {
          out << newEntry + "\n";
          out << s + "\n";
          m_call3Modified=true;
        } else if(hc==hc2) {
          QString t=s + "\n\n is already in CALL3.TXT\n" +
            "Do you wish to replace it?";
          int ret = QMessageBox::warning(this, "Add",t,
                                         QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
          if(ret==QMessageBox::Yes) {
            out << newEntry + "\n";
            m_call3Modified=true;
          }
        } else {
          if(s!="") out << s + "\n";
        }
      }
    } while(!s.isNull());
    if(hc>hc1 && !m_call3Modified) out << newEntry + "\n";
  }
  
  if(m_call3Modified) {
    auto const& old_path = m_dataDir + "/CALL3.OLD";
    QFile f0 {old_path};
    if (f0.exists ()) f0.remove ();
    f1.copy (old_path);         // copying as we want to preserve
                                // symlinks
    qDebug() << "MainWindow::on_addButton_clicked File open result:" << f1.open (QFileDevice::WriteOnly | QFileDevice::Text); // truncates
    f2.seek (0);
    f1.write (f2.readAll ());   // copy contents
    f2.remove ();
  }
}

void MainWindow::msgtype(QString t, QLineEdit* tx)                //msgtype()
{
//  if(t.length()<1) return 0;
  char message[23];
  char msgsent[23];
  int len1=22;
  int mode65=0;            //mode65 ==> check message but don't make wave()
  double samfac=1.0;
  int nsendingsh=0;
  int mwave;
  t=t.toUpper();
  int i1=t.indexOf(" OOO");
  QByteArray s=t.toUpper().toLocal8Bit();
  ba2msg(s,message);
  gen65_(message,&mode65,&samfac,&nsendingsh,msgsent,iwave,
         &mwave,len1,len1);

  QPalette p(tx->palette());
  if(nsendingsh==1) {
    p.setColor(QPalette::Base,"#66ffff");
  } else if(nsendingsh==-1) {
    p.setColor(QPalette::Base,"#ffccff");
  } else {
    p.setColor(QPalette::Base,Qt::white);
  }
  tx->setPalette(p);
  int len=t.length();
  if(nsendingsh==-1) {
    len=qMin(len,13);
    if(i1>10) {
      tx->setText(t.mid(0,len).toUpper() + " OOO");
    } else {
      tx->setText(t.mid(0,len).toUpper());
    }
  } else {
    tx->setText(t);
  }
}

void MainWindow::on_tx1_editingFinished()                       //tx1 edited
{
  QString t=ui->tx1->text();
  msgtype(t, ui->tx1);
}

void MainWindow::on_tx2_editingFinished()                       //tx2 edited
{
  QString t=ui->tx2->text();
  msgtype(t, ui->tx2);
}

void MainWindow::on_tx3_editingFinished()                       //tx3 edited
{
  QString t=ui->tx3->text();
  msgtype(t, ui->tx3);
}

void MainWindow::on_tx4_editingFinished()                       //tx4 edited
{
  QString t=ui->tx4->text();
  msgtype(t, ui->tx4);
}

void MainWindow::on_tx5_editingFinished()                       //tx5 edited
{
  QString t=ui->tx5->text();
  msgtype(t, ui->tx5);
}

void MainWindow::on_tx6_editingFinished()                       //tx6 edited
{
  QString t=ui->tx6->text();
  msgtype(t, ui->tx6);
}

void MainWindow::on_setTxFreqButton_clicked()                  //Set Tx Freq
{
  m_setftx=1;
  m_txFreq=m_wide_graph_window->QSOfreq();
}

void MainWindow::on_dxCallEntry_textChanged(const QString &t) //dxCall changed
{
  m_hisCall=t.toUpper().trimmed();
  ui->dxCallEntry->setText(m_hisCall);
  // Move the waterfall's red DX-Call highlight immediately -- covers
  // typing and every click-to-work path (waterfall, bandmap, decoded
  // text, messages), since they all set dxCallEntry's text.
  if (m_wide_graph_window) m_wide_graph_window->updateActiveCallsign(m_hisCall);
}

void MainWindow::on_dxGridEntry_textChanged(const QString &t) //dxGrid changed
{
  int n=t.length();
  if(n!=4 and n!=6) return;
  if(!t[0].isLetter() or !t[1].isLetter()) return;
  if(!t[2].isDigit() or !t[3].isDigit()) return;
  if(n==4) m_hisGrid=t.mid(0,2).toUpper() + t.mid(2,2);
  if(n==6) m_hisGrid=t.mid(0,2).toUpper() + t.mid(2,2) +
      t.mid(4,2).toLower();
  ui->dxGridEntry->setText(m_hisGrid);
}

void MainWindow::on_genStdMsgsPushButton_clicked()         //genStdMsgs button
{
  genStdMsgs("");
}

void MainWindow::on_logQSOButton_clicked()                 //Log QSO button
{
  int nMHz=getFcenter();
  QDateTime t = QDateTime::currentDateTimeUtc();
  QString qsoMode=lab5->text();
  if(m_modeTx.startsWith("Q65")) qsoMode=lab6->text();
  QString logEntry=t.date().toString("yyyy-MMM-dd,") +
      t.time().toString("hh:mm,") + m_hisCall + "," + m_hisGrid + "," +
          QString::number(nMHz) + "," + qsoMode + "\r\n";

  int ret = QMessageBox::warning(this, "Log Entry",
       "Please confirm log entry:\n\n" + logEntry + "\n",
       QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
  if(ret==QMessageBox::No) return;
  QFile f("wsjt.log");
  if(!f.open(QFile::Append)) {
    msgBox("Cannot open file \"wsjt.log\".");
    return;
  }
  QTextStream out(&f);
  out << logEntry;
  f.close();
  m_worked[m_hisCall]=true;
}

void MainWindow::on_actionErase_map65_rx_log_triggered()     //Erase Rx log
{
  int ret = QMessageBox::warning(this, "Confirm Erase",
      "Are you sure you want to erase file map65_rx.log ?",
       QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
  if(ret==QMessageBox::Yes) {
    m_map65RxLog |= 2;                      // Rewind map65_rx.log
  }
}

void MainWindow::on_actionErase_map65_tx_log_triggered()     //Erase Tx log
{
  int ret = QMessageBox::warning(this, "Confirm Erase",
      "Are you sure you want to erase file map65_tx.log ?",
       QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
  if(ret==QMessageBox::Yes) {
    QFile f("map65_tx.log");
    f.remove();
  }
}

void MainWindow::on_actionNoJT65_triggered()
{
  m_mode65=0;
  m_modeJT65=0;
  m_TRperiod=60;
  soundInThread.setPeriod(m_TRperiod);
  soundOutThread.setPeriod(m_TRperiod);
  m_wide_graph_window->setMode65(m_mode65);
  m_wide_graph_window->setPeriod(m_TRperiod);
  lab5->setStyleSheet("");
  lab5->setText("");
}
void MainWindow::on_actionJT65A_triggered()
{
  m_mode="JT65A";
  m_modeJT65=1;
  m_mode65=1;
  m_TRperiod=60;
  soundInThread.setPeriod(m_TRperiod);
  soundOutThread.setPeriod(m_TRperiod);
  m_wide_graph_window->setMode65(m_mode65);
  m_wide_graph_window->setPeriod(m_TRperiod);
  lab5->setStyleSheet("QLabel{background-color: #ff6666}");
  lab5->setText("JT65A");
  ui->actionJT65A->setChecked(true);
}

void MainWindow::on_actionJT65B_triggered()
{
  m_mode="JT65B";
  m_modeJT65=2;
  m_mode65=2;
  m_TRperiod=60;
  soundInThread.setPeriod(m_TRperiod);
  soundOutThread.setPeriod(m_TRperiod);
  m_wide_graph_window->setMode65(m_mode65);
  m_wide_graph_window->setPeriod(m_TRperiod);
  lab5->setStyleSheet("QLabel{background-color: #ffff66}");
  lab5->setText("JT65B");
  ui->actionJT65B->setChecked(true);
}

void MainWindow::on_actionJT65C_triggered()
{
  m_mode="JT65C";
  m_modeJT65=3;
  m_mode65=4;
  m_TRperiod=60;
  soundInThread.setPeriod(m_TRperiod);
  soundOutThread.setPeriod(m_TRperiod);
  m_wide_graph_window->setMode65(m_mode65);
  m_wide_graph_window->setPeriod(m_TRperiod);
  lab5->setStyleSheet("QLabel{background-color: #66ffb2}");
  lab5->setText("JT65C");
  ui->actionJT65C->setChecked(true);
}

void MainWindow::on_actionNoQ65_triggered()
{
  m_modeQ65=0;
  lab6->setStyleSheet("");
  lab6->setText("");
}

void MainWindow::on_actionQ65A_triggered()
{
  m_modeQ65=1;
  lab6->setStyleSheet("QLabel{background-color: #ffb266}");
  lab6->setText("Q65A");
}

void MainWindow::on_actionQ65B_triggered()
{
  m_modeQ65=2;
  lab6->setStyleSheet("QLabel{background-color: #b2ff66}");
  lab6->setText("Q65B");
}


void MainWindow::on_actionQ65C_triggered()
{
  m_modeQ65=3;
  lab6->setStyleSheet("QLabel{background-color: #66ffff}");
  lab6->setText("Q65C");
}

void MainWindow::on_actionQ65D_triggered()
{
  m_modeQ65=4;
  lab6->setStyleSheet("QLabel{background-color: #b266ff}");
  lab6->setText("Q65D");
}

void MainWindow::on_actionQ65E_triggered()
{
  m_modeQ65=5;
  lab6->setStyleSheet("QLabel{background-color: #ff66ff}");
  lab6->setText("Q65E");
}


void MainWindow::on_NBcheckBox_toggled(bool checked)
{
  m_NB=checked;
  ui->NBslider->setEnabled(m_NB);
}

void MainWindow::on_NBslider_valueChanged(int n)
{
  m_NBslider=n;
}

void MainWindow::on_actionAdjust_IQ_Calibration_triggered()
{
  m_adjustIQ=1;
}

void MainWindow::on_actionApply_IQ_Calibration_triggered()
{
  m_applyIQcal= 1-m_applyIQcal;
}

void MainWindow::on_actionFUNcube_Dongle_triggered()
{
  proc_qthid.start (QDir::toNativeSeparators(m_appDir + "/qthid"), QStringList {});
}

void MainWindow::on_actionEdit_wsjt_log_triggered()
{
  proc_editor.start (QDir::toNativeSeparators (m_editorCommand), {QDir::toNativeSeparators (m_dataDir + "/wsjt.log"), });
}

void MainWindow::on_actionTx_Tune_triggered()
{
  if (!g_pTxTune) {
    g_pTxTune = new TxTune(0);

    if (!g_TxTuneGeometry.isEmpty())
      g_pTxTune->restoreGeometry(g_TxTuneGeometry);
  }
  g_pTxTune->set_iqAmp(iqAmp);
  g_pTxTune->set_iqPhase(iqPhase);
  g_pTxTune->set_txPower(txPower);
  g_pTxTune->show();
}

void MainWindow::on_pbTxMode_clicked()
{
  if(m_modeTx=="Q65") {
    m_modeTx="JT65";
    ui->pbTxMode->setText("Tx JT65   #");
  } else {
    m_modeTx="Q65";
    ui->pbTxMode->setText("Tx Q65  :");
  }
//  m_wideGraph->setModeTx(m_modeTx);
//  statusChanged();
}

bool MainWindow::isGrid4(QString g)
{
  if(g.length()!=4) return false;
  if(g.mid(0,1)<'A' or g.mid(0,1)>'R') return false;
  if(g.mid(1,1)<'A' or g.mid(1,1)>'R') return false;
  if(g.mid(2,1)<'0' or g.mid(2,1)>'9') return false;
  if(g.mid(3,1)<'0' or g.mid(3,1)>'9') return false;
  return true;
}

// Given already-tokenized message-body words (e.g. ["CQ","W1ABC","FN20"] or
// ["W1ABC","K2XYZ","FN20"]), extract the sender callsign (the "owner" of
// the line -- whoever's grid/report follows) and grid square if present/
// valid. Shared by the Messages-window/BandMap click-to-work extraction
// and the live Universal Decode Label Parser, so all three agree on the
// same CQ-vs-QSO layout rules. Returns false if no sender was found.
bool MainWindow::senderAndGridFromMsgCols(const QStringList& msg_cols, QString& sender, QString& grid)
{
  sender.clear();
  grid.clear();
  if (msg_cols.size() < 2) return false;
  static const QRegularExpression call_re("^([A-Z0-9]{1,4}/)?[A-Z0-9]{1,3}[0-9][A-Z0-9]{0,3}[A-Z](/[A-Z0-9]+)?$");
  if (msg_cols[0] == "CQ") {
    int senderIdx = 1;
    if (msg_cols.size() >= 3 && msg_cols[1] == "DX") senderIdx = 2;
    if (senderIdx < msg_cols.size()) {
      sender = msg_cols[senderIdx].toUpper();
      if (msg_cols.size() > senderIdx + 1) {
        QString g = msg_cols[senderIdx + 1].toUpper();
        if (isGrid4(g)) grid = g;
      }
    }
  } else if (call_re.match(msg_cols[1].toUpper()).hasMatch()) {
    sender = msg_cols[1].toUpper();
    if (msg_cols.size() > 2) {
      QString g = msg_cols[2].toUpper();
      if (isGrid4(g)) grid = g;
    }
  }
  return !sender.isEmpty();
}

// Parses a Messages-window decode line into its sender + grid, mirroring
// the same JT65 hash-column / Q65 colon-split body extraction the live
// Universal Decode Label Parser uses -- so a click anywhere on the line
// resolves to the same "owner" regardless of exactly where it lands,
// instead of picking whichever word happens to be under the cursor.
bool MainWindow::parseMessageLine(const QString& t2, QString& sender, QString& grid)
{
  sender.clear();
  grid.clear();
  QString s = t2.trimmed();
  const QStringList cols = s.split(QRegularExpression("\\s+"), SkipEmptyParts);
  if (cols.size() < 6) return false;

  QString body;
  int sep_q65 = s.indexOf(':');
  int sep_msg_jt65 = s.indexOf("#");

  if (sep_q65 >= 0) {
    // Q65 message line: everything left of ':' is the interesting part.
    QString left = s.left(sep_q65).trimmed();
    QStringList leftCols = left.split(QRegularExpression("\\s+"), SkipEmptyParts);
    if (leftCols.size() > 5) {
      QStringList msg;
      for (int i = 5; i < leftCols.size(); ++i) msg << leftCols[i];
      body = msg.join(" ");
    } else {
      body = left;
    }
  } else if (sep_msg_jt65 >= 0) {
    // JT65 message line (#, #H, #V): find the "#" column, calls+grid follow.
    int hashCol = -1;
    for (int i = 0; i < cols.size(); ++i) {
      if (cols[i].startsWith("#")) { hashCol = i; break; }
    }
    if (hashCol >= 0 && hashCol + 1 < cols.size()) {
      QString afterHash = cols[hashCol + 1];
      static const QRegularExpression call_re("^([A-Z0-9]{1,4}/)?[A-Z0-9]{1,3}[0-9][A-Z0-9]{0,3}[A-Z](/[A-Z0-9]+)?$");
      if (call_re.match(afterHash).hasMatch()) {
        QStringList msg;
        for (int i = hashCol + 1; i < cols.size(); ++i) msg << cols[i];
        body = msg.join(" ");
      } else {
        // Calls + grid are always in columns 5,6,7 for MAP65 JT65 messages.
        QStringList msg;
        for (int i = 5; i <= 7 && i < cols.size(); ++i) msg << cols[i];
        body = msg.join(" ");
      }
    } else {
      body = s.mid(sep_msg_jt65 + 3).trimmed();
    }
  } else {
    return false;
  }

  QStringList msg_cols = body.split(QRegularExpression("\\s+"), SkipEmptyParts);
  return senderAndGridFromMsgCols(msg_cols, sender, grid);
}


void MainWindow::read_log()
{
  // Rebuild "m_worked" from WSJT-X's wsjtx.log -- a legacy-layout feature
  // that only functions when a colocated WSJT-X writes its log into our cwd
  // (the JTSDK-era shared install dir). Since the cwd moved to the MAP65
  // data dir, the file is normally absent: in that case KEEP the dictionary
  // (seeded from MAP65's own wsjt.log at startup, plus QSOs logged this
  // session) instead of wiping it on every decode batch -- clearing before
  // the open() check is what killed worked-station highlighting.
  QFile f("wsjtx.log");
  if(!f.open(QFileDevice::ReadOnly)) return;
  m_worked.clear();
  QTextStream in(&f);
  while(!in.atEnd()) {
    QString line = in.readLine();
    if(line.length() < 46) continue;
    QString callsign = line.mid(40,6).trimmed();
    int n = callsign.indexOf(",");
    if(n > 0) callsign = callsign.left(n);
    if(callsign.length() > 2)
      m_worked[callsign] = true;
  }
  f.close();
}

void pa_deinit()
{
    Pa_Terminate();
}


