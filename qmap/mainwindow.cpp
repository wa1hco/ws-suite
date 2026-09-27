//------------------------------------------------------------------ MainWindow
#include "mainwindow.h"
#include <fftw3.h>
#include <cmath>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSettings>
#include <QTextBlock>
#include <QTimer>
#include <QToolTip>
#include "revision_utils.hpp"
#include "qt_helpers.hpp"
#include "SettingsGroup.hpp"
#include "widgets/MessageBox.hpp"
#include "ui_mainwindow.h"
#include "devsetup.h"
#include "plotter.h"
#include "about.h"
#include "astro.h"
#include "widegraph.h"
#include "bandmap.h"
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QLineEdit>
#include "sleep.h"

#include <QCoreApplication>  //liveCQ
#include <QNetworkAccessManager>  //liveCQ
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QUrl>
#include <QUrlQuery>
#include <QEventLoop>

#include "qmap_runtime_config.h"
#include "runtime_paths.h"
#include "Network/PSKReporter.hpp"

#define NFFT 32768   // 96 kHz baseline; runtime active size = qmap_runtime::activeNfft()

qint16 id[2*60*qmap_runtime::MAX_IQ_RATE_HZ];

QSharedMemory mem_qmap("mem_qmap");            //Memory segment to be shared (optionally) with WSJT-X
int* ipc_wsjtx;

struct QmapMetaLayout {
  char config_name[32];
  char claimed_by[32];
  qint64 claim_heartbeat;
};
QSharedMemory mem_qmap_meta("mem_qmap_meta");

extern const int RxDataFrequency = 96000;

// Parameters for highlighting the click-to-work callsign
bool is_active = false;
QString DXcall = "";

// Reverse channel appended past decodes_ in the mem_qmap segment (layout also
// mirrored in map65/qmap_shared.h): WSJT-X publishes its DX Call + Grid there
// and bumps dxcall_seq on every change. The fields sit beyond sizeof(decodes_),
// so QMAP's own full-struct pushes never overwrite them.
struct QmapReverse {
  int  dxcall_seq;
  char dxcall[16];
  char dxgrid[8];
};
static_assert(sizeof(decodes_) + sizeof(QmapReverse) <= 4096,
              "decodes_ + reverse tail must fit the 4096-byte mem_qmap segment");

//-------------------------------------------------- MainWindow constructor
MainWindow::MainWindow(QString const& settings_filename, QWidget *parent) :
  QMainWindow(parent),
  ui(new Ui::MainWindow),
  m_appDir {QApplication::applicationDirPath ()},
  m_dataDir {qmapDataDir ()},
  m_settings_filename {!settings_filename.isEmpty()
                       ? settings_filename
                       // Same (idempotent) resolution+migration main() already
                       // did for its early sample-rate peek.
                       : qmapSettingsFile(m_appDir, m_dataDir)},
  m_astro_window {new Astro {m_settings_filename}},
  m_wide_graph_window {new WideGraph {m_settings_filename}},
  m_bandmap_window {new BandMap {m_settings_filename}},
  m_gui_timer {new QTimer {this}}
{
  ui->setupUi(this);
//  ui->decodedTextBrowser->clear();
  ui->labUTC->setStyleSheet( \
        "QLabel { background-color : black; color : yellow; }");
  ui->labFreq->setStyleSheet( \
        "QLabel { background-color : black; color : yellow; }");
  ui->labTol1->setStyleSheet( \
        "QLabel { background-color : white; color : black; }");
  ui->labTol1->setFrameStyle(QFrame::Panel | QFrame::Sunken);

  QActionGroup* paletteGroup = new QActionGroup(this);
  ui->actionCuteSDR->setActionGroup(paletteGroup);
  ui->actionLinrad->setActionGroup(paletteGroup);
  ui->actionAFMHot->setActionGroup(paletteGroup);
  ui->actionBlue->setActionGroup(paletteGroup);

  QActionGroup* modeGroup2 = new QActionGroup(this);
  ui->actionQ65A->setActionGroup(modeGroup2);
  ui->actionQ65B->setActionGroup(modeGroup2);
  ui->actionQ65C->setActionGroup(modeGroup2);
  ui->actionQ65D->setActionGroup(modeGroup2);
  ui->actionQ65E->setActionGroup(modeGroup2);

  QActionGroup* saveGroup = new QActionGroup(this);
  ui->actionNone->setActionGroup(saveGroup);
  ui->actionSave_all->setActionGroup(saveGroup);
  ui->actionSave_decoded->setActionGroup(saveGroup);

  QActionGroup* tickGroup = new QActionGroup(this);
  ui->actionTick_5kHz->setActionGroup(tickGroup);
  ui->actionTick_10kHz->setActionGroup(tickGroup);
  ui->actionTick_20kHz->setActionGroup(tickGroup);
  ui->actionTick_50kHz->setActionGroup(tickGroup);

  QActionGroup* transparencyGroup = new QActionGroup(this);
  ui->actionTransparency_None  ->setActionGroup(transparencyGroup);
  ui->actionTransparency_Low   ->setActionGroup(transparencyGroup);
  ui->actionTransparency_Medium->setActionGroup(transparencyGroup);
  ui->actionTransparency_High  ->setActionGroup(transparencyGroup);

  QActionGroup* fontGroup = new QActionGroup(this);
  ui->actionCallsign_font_small ->setActionGroup(fontGroup);
  ui->actionCallsign_font_normal->setActionGroup(fontGroup);
  ui->actionCallsign_font_medium->setActionGroup(fontGroup);
  ui->actionCallsign_font_large ->setActionGroup(fontGroup);

  QActionGroup* positionGroup = new QActionGroup(this);
  ui->actionCallsign_position_top   ->setActionGroup(positionGroup);
  ui->actionCallsign_position_bottom->setActionGroup(positionGroup);

  updateWindowTitle();

  connect(&soundInThread, SIGNAL(readyForFFT(int)), this, SLOT(dataSink(int)));
  connect(&soundInThread, SIGNAL(error(QString)), this, SLOT(showSoundInError(QString)));
  connect(&soundInThread, SIGNAL(status(QString)), this, SLOT(showStatusMessage(QString)));
  createStatusBar();
  connect(m_gui_timer, &QTimer::timeout, this, &MainWindow::guiUpdate);

  m_waterfallAvg=1;
  m_network=true;
  m_restart=false;
  m_myCall="K1JT";
  m_myGrid="FN20qi";
  m_myCallColor=0;
  m_saveDir="";
  m_azelDir="";
  m_loopall=false;
  m_startAnother=false;
  m_saveAll=false;
  m_saveDecoded=false;
  m_sec0=-1;
  m_hsym0=-1;
  m_palette="CuteSDR";
  m_nutc0=9999;
  m_NB=false;
  m_mode="Q65";
  m_udpPort=50004;
  m_modeQ65=0;
  m_TRperiod=60;

  xSignalMeter = new SignalMeter(ui->xMeterFrame);
  xSignalMeter->resize(50, 160);

//Attach or create a memory segment to be shared with WSJT-X.
  int memSize=4096;
  {
    QSettings peek {m_settings_filename, QSettings::IniFormat};
    SettingsGroup g {&peek, "Common"};
    int id = peek.value("InstanceId", 1).toInt();
    if (id < 1 || id > 4) id = 1;
    const QString key = (id == 1) ? QStringLiteral("mem_qmap")
                                  : QString("mem_qmap_%1").arg(id);
    mem_qmap.setKey(key);
    std::fprintf(stderr, "[qmap] mem_qmap key: %s (instance ID %d)\n",
                 key.toUtf8().constData(), id);
  }
  if(!mem_qmap.attach()) {
    if(!mem_qmap.create(memSize)) {
      msgBox("Unable to create shared memory segment mem_qmap.");
    }
  }
  ipc_wsjtx = (int*)mem_qmap.data();
  mem_qmap.lock();
  memset(ipc_wsjtx,0,memSize);         //Zero all of shared memory
  mem_qmap.unlock();

  {
    QSettings peek {m_settings_filename, QSettings::IniFormat};
    SettingsGroup g {&peek, "Common"};
    int id = peek.value("InstanceId", 1).toInt();
    if (id < 1 || id > 4) id = 1;
    mem_qmap_meta.setKey(QString("mem_qmap_meta_%1").arg(id));
    if (!mem_qmap_meta.attach()) {
      if (mem_qmap_meta.create(sizeof(QmapMetaLayout))) {
        mem_qmap_meta.lock();
        memset(mem_qmap_meta.data(), 0, sizeof(QmapMetaLayout));
        mem_qmap_meta.unlock();
      }
    }
  }

//  fftwf_import_wisdom_from_filename (QDir {m_appDir}.absoluteFilePath ("qmap_wisdom.dat").toLocal8Bit ());
  readSettings();		             //Restore user's setup params
  updateWindowTitle();                        // now m_instanceId/m_configName are loaded
  if (m_wide_graph_window) m_wide_graph_window->setInstanceLabel(m_instanceId, m_configName);
  publishConfigNameToMeta();
  updatePSKReporter();

  m_pbdecoding_style1="QPushButton{background-color: cyan; \
      border-style: outset; border-width: 1px; border-radius: 5px; \
      border-color: black; min-width: 5em; padding: 3px;}";
  m_pbmonitor_style="QPushButton{background-color: #00ff00; \
      border-style: outset; border-width: 1px; border-radius: 5px; \
      border-color: black; min-width: 5em; padding: 3px;}";
  m_pbmonitor_style2="QPushButton{background-color: #ffff00; \
      border-style: outset; border-width: 1px; border-radius: 5px; \
      border-color: black; min-width: 5em; padding: 3px;}";
  m_pbAutoOn_style="QPushButton{background-color: red; \
      border-style: outset; border-width: 1px; border-radius: 5px; \
      border-color: black; min-width: 5em; padding: 3px;}";

  // Reopen the Astro window only if it was open when QMAP last exited
  // (visibility captured in closeEvent, persisted as "AstroOpen").
  if(m_astroOpen) on_actionAstro_Data_triggered();
  on_actionWide_Waterfall_triggered();
  if (m_astro_window) m_astro_window->setFontSize (m_astroFont);

  if(m_modeQ65==1) on_actionQ65A_triggered();
  if(m_modeQ65==2) on_actionQ65B_triggered();
  if(m_modeQ65==3) on_actionQ65C_triggered();
  if(m_modeQ65==4) on_actionQ65D_triggered();
  if(m_modeQ65==5) on_actionQ65E_triggered();

  connect(&watcher3, SIGNAL(finished()),this,SLOT(decoderFinished()));

// Assign input device and start input thread
  soundInThread.setRate(96000.0);
  soundInThread.setBufSize(10*7056);
  soundInThread.setNetwork(m_network);
  soundInThread.setPort(m_udpPort);
  soundInThread.setPeriod(m_TRperiod);
  soundInThread.start(QThread::HighestPriority);

  m_monitoring=true;                           // Start with Monitoring ON
  soundInThread.setMonitoring(m_monitoring);
  m_diskData=false;
  m_wide_graph_window->setFcal(m_fCal);
  m_wide_graph_window->setFsample(qmap_runtime::activeRateHz());
  m_wide_graph_window->setIqRateLabel(qmap_runtime::activeRateHz(),
                                      QString::fromLatin1(qmap_runtime::rateSourceLabel()));
  QString rev{"WS-MAP v" + QCoreApplication::applicationVersion() + " " + revision()};
  m_revision=rev;

// Create "m_worked", a dictionary of all calls in wsjt.log
  QFile f("wsjt.log");
  f.open(QIODevice::ReadOnly);
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

// Read items for fAddComboBox
  ui->fAddComboBox->addItem (0);
  ui->fAddComboBox->setItemText(0, QString::number(m_fAdd));
  QFile g("fadd.txt");
  QTextStream stream(&g);
  if(g.open (QIODevice::ReadOnly | QIODevice::Text)) {
    while (!stream.atEnd()) {
      QString fAddline = stream.readLine();
      if (fAddline != "") ui->fAddComboBox->addItem (fAddline);
    }
    stream.flush();
    g.close();
  }

  if(ui->actionLinrad->isChecked()) on_actionLinrad_triggered();
  if(ui->actionCuteSDR->isChecked()) on_actionCuteSDR_triggered();
  if(ui->actionAFMHot->isChecked()) on_actionAFMHot_triggered();
  if(ui->actionBlue->isChecked()) on_actionBlue_triggered();

  connect (m_wide_graph_window.get (), &WideGraph::freezeDecode2, this, &MainWindow::freezeDecode);
  connect (m_wide_graph_window.get (), &WideGraph::f11f12, this, &MainWindow::bumpDF);
  connect (m_wide_graph_window.get (), &WideGraph::callsignClicked,
           this, &MainWindow::handleCallsignClick);
  connect (m_bandmap_window.get (), &BandMap::callsignClicked,
           this, &MainWindow::handleCallsignClick);

  ui->decodedTextBrowser->viewport()->installEventFilter(this);

  ui->actionShow_callsigns_on_Waterfall->setChecked(m_wide_graph_window->decodeLabelsEnabled());
  connect (ui->actionShow_callsigns_on_Waterfall, &QAction::toggled,
           m_wide_graph_window.get (), &WideGraph::setDecodeLabelsEnabled);
  connect (m_wide_graph_window.get (), &WideGraph::decodeLabelsEnabledChanged,
           ui->actionShow_callsigns_on_Waterfall, &QAction::setChecked);

  {
    const int tk = m_wide_graph_window->tickSpacingKhz();
    if      (tk == 10) ui->actionTick_10kHz->setChecked(true);
    else if (tk == 20) ui->actionTick_20kHz->setChecked(true);
    else if (tk == 50) ui->actionTick_50kHz->setChecked(true);
    else               ui->actionTick_5kHz ->setChecked(true);  // 5 = default
    auto* wg = m_wide_graph_window.get();
    connect(ui->actionTick_5kHz,  &QAction::triggered,
            wg, [wg]{ wg->setTickSpacingKhz(5);  });
    connect(ui->actionTick_10kHz, &QAction::triggered,
            wg, [wg]{ wg->setTickSpacingKhz(10); });
    connect(ui->actionTick_20kHz, &QAction::triggered,
            wg, [wg]{ wg->setTickSpacingKhz(20); });
    connect(ui->actionTick_50kHz, &QAction::triggered,
            wg, [wg]{ wg->setTickSpacingKhz(50); });
  }

  // Callsign-overlay transparency:
  {
    const int a = m_wide_graph_window->decodeLabelAlpha();
    if      (a == 175) ui->actionTransparency_High  ->setChecked(true);
    else if (a == 200) ui->actionTransparency_Medium->setChecked(true);
    else if (a == 220) ui->actionTransparency_Low   ->setChecked(true);
    else               ui->actionTransparency_None  ->setChecked(true);
    auto* wg = m_wide_graph_window.get();
    connect(ui->actionTransparency_None,   &QAction::triggered,
            wg, [wg]{ wg->setDecodeLabelAlpha(255); });
    connect(ui->actionTransparency_Low,    &QAction::triggered,
            wg, [wg]{ wg->setDecodeLabelAlpha(220); });
    connect(ui->actionTransparency_Medium, &QAction::triggered,
            wg, [wg]{ wg->setDecodeLabelAlpha(200); });
    connect(ui->actionTransparency_High,   &QAction::triggered,
            wg, [wg]{ wg->setDecodeLabelAlpha(175); });
  }

  // Callsign-overlay font size: Small=7, Normal=8 (default), Medium=10, Large=12.
  {
    auto* wg = m_wide_graph_window.get();
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
  }

  // Callsign-overlay anchor position
  {
    auto* wg = m_wide_graph_window.get();
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

  //default freq at startup for Doppler and Tsky
  datcom_.fcenter = 1296.150;
  
  // only start the guiUpdate timer after this constructor has finished
  QTimer::singleShot (0, [=] {
                           m_gui_timer->start(100); //Don't change the 100 ms!
                         });

  if (!qmap_runtime::g_open_path.isEmpty()) {
    const QString open_path = qmap_runtime::g_open_path;
    QTimer::singleShot(1000, this, [this, open_path] {
      m_path = open_path;
      m_monitoring = false;
      soundInThread.setMonitoring(m_monitoring);
      m_diskData = true;
      int dbDgrd = 0;
      int iret = 4;
      if (open_path.indexOf(".iq") > 0) {
        getfile(open_path, dbDgrd);
      } else {
        read_qm_(open_path.toLatin1(), &iret, open_path.length());
      }
      if (iret > 0) diskDat(iret);
    });
  }
  if (qmap_runtime::activeRateHz() == 96000) {
    lab8->setStyleSheet("QLabel{background-color: #b4ffb4}");
    lab8->setText("96 kHz");
  }
  if (qmap_runtime::activeRateHz() == 128000) {
    lab8->setStyleSheet("QLabel{background-color: #ffff9b}");
    lab8->setText("128 kHz");
  }
  if (qmap_runtime::activeRateHz() == 192000) {
    lab8->setStyleSheet("QLabel{background-color: #ffccff}");
    lab8->setText("192 kHz");
  }
  if (qmap_runtime::activeRateHz() == 256000) {
    lab8->setStyleSheet("QLabel{background-color: #aaffff}");
    lab8->setText("256 kHz");
  }
}

  //--------------------------------------------------- MainWindow destructor
MainWindow::~MainWindow()
{
  writeSettings();
  all_done_();

  if (soundInThread.isRunning()) {
    soundInThread.quit();
    soundInThread.wait(3000);
  }
  delete ui;
}

//----------------------------------------------------- updateWindowTitle()
void MainWindow::updateWindowTitle()
{
  QString title = program_title ();
  if (!m_configName.isEmpty()) {
    // Multi-instance identity is set — show "ID N  name", e.g.
    // "QMAP — 1  1296 CQ". More informative than the raw INI basename.
    title += QString(" \xe2\x80\x94 %1  %2").arg(m_instanceId).arg(m_configName);
  } else {
    const QString default_path = QDir {m_dataDir}.absoluteFilePath("wsmap.ini");
    if (m_settings_filename != default_path) {
      const QString base = QFileInfo {m_settings_filename}.completeBaseName();
      if (!base.isEmpty()) title += " \xe2\x80\x94 " + base;   // em-dash
    }
  }
  setWindowTitle (title);
}

//---------------------------------------------- publishConfigNameToMeta()
void MainWindow::publishConfigNameToMeta()
{
  if (!mem_qmap_meta.data()) return;   // not attached (shouldn't happen, but be safe)
  mem_qmap_meta.lock();
  char* base = static_cast<char*>(mem_qmap_meta.data());
  memset(base, 0, 32);
  const QByteArray nameBytes = m_configName.toUtf8().left(31);
  memcpy(base, nameBytes.constData(), nameBytes.size());
  mem_qmap_meta.unlock();
}

//-------------------------------------------------------- writeSettings()
void MainWindow::writeSettings()
{
  QSettings settings(m_settings_filename, QSettings::IniFormat);
  {
    SettingsGroup g {&settings, "MainWindow"};
    settings.setValue("geometry", saveGeometry());
    settings.setValue("MRUdir", m_path);
  }

  {
  SettingsGroup g {&settings, "Common"};
  settings.setValue("MyCall",m_myCall);
  settings.setValue("MyGrid",m_myGrid);
  settings.setValue("AstroFont",m_astroFont);
  settings.setValue("MyCallColor",m_myCallColor);
  settings.setValue("SaveDir",m_saveDir);
  settings.setValue("AzElDir",m_azelDir);
  settings.setValue("Fcal",m_fCal);
  settings.setValue("Fadd",m_fAdd);
  settings.setValue("NetworkInput", m_network);
  settings.setValue("paInDevice",m_paInDevice);
  settings.setValue("Scale_dB",m_dB);
  settings.setValue("UDPport",m_udpPort);
  settings.setValue("PaletteCuteSDR",ui->actionCuteSDR->isChecked());
  settings.setValue("PaletteLinrad",ui->actionLinrad->isChecked());
  settings.setValue("PaletteAFMHot",ui->actionAFMHot->isChecked());
  settings.setValue("PaletteBlue",ui->actionBlue->isChecked());
  settings.setValue("Mode",m_mode);
  settings.setValue("nModeQ65",m_modeQ65);
  settings.setValue("SaveNone",ui->actionNone->isChecked());
  settings.setValue("SaveAll",ui->actionSave_all->isChecked());
  settings.setValue("SaveDecoded",ui->actionSave_decoded->isChecked());
  settings.setValue("ContinuousWaterfall",ui->continuous_waterfall->isChecked());
  settings.setValue("FaddControls",ui->actionFadd_controls->isChecked());
  settings.setValue("SyncWSJTX",ui->actionSync_WSJTX->isChecked());
  settings.setValue("AstroOpen",m_astroOpen);
  settings.setValue("NB",m_NB);
  settings.setValue("NBslider",m_NBslider);
  settings.setValue("MaxDrift",ui->sbMaxDrift->value());
  settings.setValue("Offset",ui->sbOffset->value());
  settings.setValue("Also30",m_bAlso30);
  settings.setValue("LiveCQEnabled",m_livecqEnabled);        //liveCQ
  settings.setValue("LiveCQDestOfficial",m_livecqOfficial);  //liveCQ
  settings.setValue("LiveCQDestN6NU",m_livecqN6NU);          //liveCQ
  settings.setValue("LiveCQDestCustom",m_livecqCustom);      //liveCQ
  settings.setValue("LiveCQW3SZAppId",m_livecqW3szAppId);    //liveCQ
  settings.setValue("otherUrl",m_otherUrl);                  //liveCQ
  settings.setValue("spotPSK",m_spotPSK);
  settings.setValue("PSKReporterTCPIP",m_spotPSKTcpIp);
  settings.setValue("FTol",m_tol);
  settings.setValue("InstanceId",m_instanceId);
  settings.setValue("ConfigName",m_configName);
  }   // end Common SettingsGroup

  {
    SettingsGroup linrad_g {&settings, "Linrad"};
    settings.setValue("sample_rate_mode",
                      m_sampleRateModeOrZero == 0 ? QString("auto")
                                                  : QString::number(m_sampleRateModeOrZero));
    settings.setValue("host", m_linradHost);
    settings.setValue("tcp_port", m_linradTcpPort);
  }
}

//---------------------------------------------------------- readSettings()
void MainWindow::readSettings()
{
  QSettings settings(m_settings_filename, QSettings::IniFormat);
  // Per-instance work dir (also the process cwd, set by main()): default home
  // for recordings and azel.dat. Not shared across instances -- .qm names are
  // bare UTC timestamps and azel.dat's Doppler line is band-dependent.
  const QString instanceDir =
      qmapInstanceDir(m_dataDir, qmapInstanceId(m_settings_filename));
  {
    SettingsGroup g {&settings, "MainWindow"};
    restoreGeometry(settings.value("geometry").toByteArray());
    m_path = settings.value("MRUdir", instanceDir + "/save").toString();
  }

  {
  SettingsGroup g {&settings, "Common"};
  m_myCall=settings.value("MyCall","").toString();
  m_myGrid=settings.value("MyGrid","").toString();
  m_astroFont=settings.value("AstroFont",18).toInt();
  m_myCallColor=settings.value("MyCallColor",1).toInt();
  m_saveDir=settings.value("SaveDir",instanceDir + "/save").toString();
  m_azelDir=settings.value("AzElDir",instanceDir).toString();
  m_fCal=settings.value("Fcal",0).toInt();
  m_fAdd=settings.value("FAdd",0).toDouble();
  soundInThread.setFadd(m_fAdd);
  m_network = settings.value("NetworkInput",true).toBool();
  m_dB = settings.value("Scale_dB",0).toInt();
  m_udpPort = settings.value("UDPport",50004).toInt();
  soundInThread.setScale(m_dB);
  soundInThread.setPort(m_udpPort);
  ui->actionCuteSDR->setChecked(settings.value(
                                  "PaletteCuteSDR",true).toBool());
  ui->actionLinrad->setChecked(settings.value(
                                 "PaletteLinrad",false).toBool());

  m_astroOpen=settings.value("AstroOpen",true).toBool();
  m_modeQ65=settings.value("nModeQ65",3).toInt();
  if(m_modeQ65==1) ui->actionQ65A->setChecked(true);
  if(m_modeQ65==2) ui->actionQ65B->setChecked(true);
  if(m_modeQ65==3) ui->actionQ65C->setChecked(true);
  if(m_modeQ65==4) ui->actionQ65D->setChecked(true);
  if(m_modeQ65==5) ui->actionQ65E->setChecked(true);

  ui->actionNone->setChecked(settings.value("SaveNone",true).toBool());
  ui->actionSave_all->setChecked(settings.value("SaveAll",false).toBool());
  ui->actionSave_decoded->setChecked(settings.value("SaveDecoded",false).toBool());
  ui->continuous_waterfall->setChecked(settings.value("ContinuousWaterfall",false).toBool());
  ui->actionFadd_controls->setChecked(settings.value("FaddControls",false).toBool());
  ui->actionSync_WSJTX->setChecked(settings.value("SyncWSJTX",true).toBool());
  m_saveAll=ui->actionSave_all->isChecked();
  m_saveDecoded=ui->actionSave_decoded->isChecked();
  if(m_saveAll) {
    lab5->setStyleSheet("QLabel{background-color: #ffff00}");
    lab5->setText("Save all");
  } else if(m_saveDecoded) {
    lab5->setStyleSheet("QLabel{background-color: #ffff00}");
    lab5->setText("Save decoded");
  } else {
    lab5->setStyleSheet("");
    lab5->setText("");
  }
  m_NB=settings.value("NB",false).toBool();
  ui->NBcheckBox->setChecked(m_NB);
  ui->sbMaxDrift->setValue(settings.value("MaxDrift",0).toInt());
  ui->sbOffset->setValue(settings.value("Offset",1500).toInt());
  m_NBslider=settings.value("NBslider",40).toInt();
  ui->NBslider->setValue(m_NBslider);
  m_bAlso30=settings.value("Also30",true).toBool();
  ui->actionAlso_Q65_30x->setChecked(m_bAlso30);
  on_actionAlso_Q65_30x_toggled(m_bAlso30);
  if(!ui->actionLinrad->isChecked() && !ui->actionCuteSDR->isChecked() &&
    !ui->actionAFMHot->isChecked() && !ui->actionBlue->isChecked()) {
    on_actionLinrad_triggered();
    ui->actionLinrad->setChecked(true);
  }
  // LiveCQ destinations are independent checkboxes (matches WSJT-X
  // 2026-08-12). A new install starts with W3SZ (w3sz.com) off and N6NU
  // on (Andreas, 2026-09-12); an existing ini keeps the operator's choice.
  m_livecqEnabled=settings.value("LiveCQEnabled",true).toBool();          //liveCQ
  m_spotPSK=settings.value("spotPSK",false).toBool();
  m_spotPSKTcpIp=settings.value("PSKReporterTCPIP",false).toBool();
  m_otherUrl=settings.value("otherUrl","").toString();  //liveCQ
  // Program name reported to w3sz.com: QMAP, its tag there before the
  // re-branding, or WSMAP. N6NU and Custom always receive WSMAP.
  m_livecqW3szAppId=settings.value("LiveCQW3SZAppId","QMAP").toString();  //liveCQ
  if(m_livecqW3szAppId!="WSMAP") m_livecqW3szAppId="QMAP";
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
    m_livecqOfficial=settings.value("LiveCQDestOfficial",false).toBool(); //liveCQ
    m_livecqN6NU=settings.value("LiveCQDestN6NU",true).toBool();          //liveCQ
    m_livecqCustom=settings.value("LiveCQDestCustom",false).toBool();     //liveCQ
  }

  m_instanceId=settings.value("InstanceId",1).toInt();
  if (m_instanceId < 1 || m_instanceId > 4) m_instanceId = 1;
  m_configName=settings.value("ConfigName","").toString();
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

  ui->fAddComboBox->setVisible(ui->actionFadd_controls->isChecked());
  ui->fAdd_label->setVisible(ui->actionFadd_controls->isChecked());
  ui->pbSet->setVisible(ui->actionFadd_controls->isChecked());
  ui->pbAdd->setVisible(ui->actionFadd_controls->isChecked());
  }   // end Common SettingsGroup

  {
    SettingsGroup linrad_g {&settings, "Linrad"};
    QString mode_str = settings.value("sample_rate_mode", "auto").toString();
    bool ok = false;
    int hz = mode_str.toInt(&ok);
    m_sampleRateModeOrZero = (ok && hz > 0) ? hz : 0;
    m_linradHost    = settings.value("host", "127.0.0.1").toString();
    m_linradTcpPort = settings.value("tcp_port", 49812).toInt();
  }
}

//-------------------------------------------------------------- dataSink()
void MainWindow::dataSink(int k)
{
  // Sized at MAX_NFFT (256 kHz wide-mode upper bound) so any runtime
  // active NFFT fits; loops below bound by qmap_runtime::activeNfft().
  static float s[qmap_runtime::MAX_NFFT];
  static float splot[qmap_runtime::MAX_NFFT];
  const int nfft_a = qmap_runtime::activeNfft();
  static int n=0;
  static int ihsym=0;
  static int ihsym0=0;
  static int nzap=0;
  static int ntrz=0;
  static int nkhz;
  static int nfsample=96000;
  static int nsec0=0;
  static int nsum=0;
  static int ndiskdat;
  static int nb;
  static int k0=0;
  static float px=0.0;
  static uchar lstrong[1024];
  static float slimit;
  static double xsum=0.0;

  if(m_diskData) {
    ndiskdat=1;
    datcom_.ndiskdat=1;
  } else {
    ndiskdat=0;
    datcom_.ndiskdat=0;
  }
// Get power, spectrum, nkhz, and ihsym
  nb=0;
  if(m_NB) nb=1;
  nfsample=qmap_runtime::activeRateHz();    //was: 96000 hardcoded

  if(m_bWTransmitting) zaptx_(datcom_.d4, &k0, &k);
  k0=k;

  symspec_(&k, &ndiskdat, &nb, &m_NBslider, &nfsample,
           &px, s, &nkhz, &ihsym, &nzap, &slimit, lstrong);

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

  if(m_bWTransmitting) px=0.0;
  QString t;
  m_pctZap=nzap/178.3;

  lab2->setText (
        QString {" Rx: %1  %2 % "}
        .arg (px, 5, 'f', 1)
        .arg (m_pctZap, 5, 'f', 1)
        );

  xSignalMeter->setValue(px);                   // Update the signal meter
  //Suppress scrolling if WSJT-X is transmitting
  if((m_monitoring and (!m_bWTransmitting or ui->continuous_waterfall->isChecked())) or m_diskData) {
      m_wide_graph_window->dataSink2(s,nkhz,ihsym,m_diskData,lstrong);
  }

  //Average over specified number of spectra
  if (n==0) {
    for (int i=0; i<nfft_a; i++)
      splot[i]=s[i];
  } else {
    for (int i=0; i<nfft_a; i++)
      splot[i] += s[i];
  }
  n++;

  if (n>=m_waterfallAvg) {
    for (int i=0; i<nfft_a; i++) {
        splot[i] /= n;                           //Normalize the average
    }

// Time according to this computer
    qint64 ms = QDateTime::currentMSecsSinceEpoch() % 86400000;
    int ntr = (ms/1000) % m_TRperiod;
    if((m_diskData && ihsym <= m_waterfallAvg) || (!m_diskData && ntr<ntrz)) {
      for (int i=0; i<nfft_a; i++) {
        splot[i] = 1.e30;
      }
    }
    ntrz=ntr;
    n=0;
  }

  bool bCallDecoder=false;
  if(ihsym < m_hsymStop) m_decode_called=false;
  if(ihsym==m_hsymStop and !m_decode_called) bCallDecoder=true; //Decode at t=58.5 s
  if(ihsym==130) bCallDecoder=true;
  if(m_bAlso30 and (ihsym==200)) bCallDecoder=true;
  if(ihsym==330) bCallDecoder=true;
  if(ihsym==ihsym0) bCallDecoder=false;

  ihsym0=ihsym;
  if(bCallDecoder) {
    if(ihsym==m_hsymStop) m_decode_called=true;
    datcom_.nagain=0;
    datcom_.nhsym=ihsym;
    decode();                                           //Prepare to start the decoder
    if(ihsym==m_hsymStop) {
      m_nTx30a=0;
      m_nTx30b=0;
      m_nTx60=0;
    }
  }
  soundInThread.m_dataSinkBusy=false;
}

void MainWindow::showSoundInError(const QString& errorMsg)
 {QMessageBox::critical(this, tr("Error in SoundIn"), errorMsg);}

void MainWindow::showStatusMessage(const QString& statusMsg)
 {statusBar()->showMessage(statusMsg);}

void MainWindow::on_actionSettings_triggered()
{
  DevSetup dlg(this);
  dlg.m_myCall=m_myCall;
  dlg.m_myGrid=m_myGrid;
  dlg.m_astroFont=m_astroFont;
  dlg.m_myCallColor=m_myCallColor;
  dlg.m_saveDir=m_saveDir;
  dlg.m_azelDir=m_azelDir;
  dlg.m_fCal=m_fCal;
  dlg.m_fAdd=m_fAdd;
  dlg.m_network=m_network;
  dlg.m_udpPort=m_udpPort;
  dlg.m_dB=m_dB;
  dlg.m_sampleRateModeOrZero=m_sampleRateModeOrZero;
  dlg.m_linradHost=m_linradHost;
  dlg.m_linradTcpPort=m_linradTcpPort;
  dlg.m_livecqEnabled = m_livecqEnabled;    //liveCQ
  dlg.m_livecqOfficial = m_livecqOfficial;  //liveCQ
  dlg.m_livecqN6NU = m_livecqN6NU;          //liveCQ
  dlg.m_livecqCustom = m_livecqCustom;      //liveCQ
  dlg.m_livecqW3szAppId = m_livecqW3szAppId;  //liveCQ
  dlg.m_otherUrl=m_otherUrl;  //liveCQ
  dlg.m_spotPSK=m_spotPSK;
  dlg.m_spotPSKTcpIp=m_spotPSKTcpIp;
  dlg.m_instanceId=m_instanceId;
  dlg.m_configName=m_configName;
  dlg.initDlg();
  if(dlg.exec() == QDialog::Accepted) {
    m_myCall=dlg.m_myCall;
    m_myGrid=dlg.m_myGrid;
    m_astroFont=dlg.m_astroFont;
    m_myCallColor=dlg.m_myCallColor;
    if(m_astro_window && m_astro_window->isVisible()) m_astro_window->setFontSize(m_astroFont);
    ui->actionFind_Delta_Phi->setEnabled(false);
    m_saveDir=dlg.m_saveDir;
    m_azelDir=dlg.m_azelDir;
    m_fCal=dlg.m_fCal;
    m_fAdd=dlg.m_fAdd;
    soundInThread.setFadd(m_fAdd);
    m_wide_graph_window->setFcal(m_fCal);
    m_network=dlg.m_network;
    m_udpPort=dlg.m_udpPort;
    m_dB=dlg.m_dB;
    m_sampleRateModeOrZero=dlg.m_sampleRateModeOrZero;
    m_linradHost=dlg.m_linradHost;
    m_linradTcpPort=dlg.m_linradTcpPort;
    m_livecqEnabled=dlg.m_livecqEnabled;
    m_livecqOfficial=dlg.m_livecqOfficial;
    m_livecqN6NU=dlg.m_livecqN6NU;
    m_livecqCustom=dlg.m_livecqCustom;
    m_livecqW3szAppId=dlg.m_livecqW3szAppId;
    m_otherUrl=dlg.m_otherUrl;
    m_spotPSK=dlg.m_spotPSK;
    // the client keeps its UDP or TCP/IP connection, so a change starts a new one
    if (dlg.m_spotPSKTcpIp != m_spotPSKTcpIp) m_pskReporter.reset();
    m_spotPSKTcpIp=dlg.m_spotPSKTcpIp;
    m_instanceId=dlg.m_instanceId;
    m_configName=dlg.m_configName;
    updateWindowTitle();
    if (m_wide_graph_window) m_wide_graph_window->setInstanceLabel(m_instanceId, m_configName);
    publishConfigNameToMeta();
    updatePSKReporter();
    soundInThread.setScale(m_dB);

    if(dlg.m_restartSoundIn) {
      soundInThread.quit();
      soundInThread.wait(1000);
      soundInThread.setNetwork(m_network);
      soundInThread.setRate(96000.0);
      soundInThread.setNrx(1);
      soundInThread.start(QThread::HighestPriority);
    }

    if (ui->fAddComboBox->isVisible()) {
      ui->fAddComboBox->setItemText(0, QString::number(m_fAdd));
      ui->fAddComboBox->setCurrentIndex(0);
    }
  }
}

void MainWindow::on_monitorButton_clicked()                  //Monitor
{
  if(m_monitoring or m_loopall) {
    m_monitoring=false;
    soundInThread.setMonitoring(false);
    m_loopall=false;
  } else {
    m_monitoring=true;
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

void MainWindow::keyPressEvent( QKeyEvent *e )                //keyPressEvent
{
  switch(e->key())
  {
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
  }
}

// Click-to-work: relays a callsign-overlay click into the mem_qmap
// shared segment so WSJT-X can populate DX Call and Fsked.
void MainWindow::handleCallsignClick(const QString& call, double freq_khz,
                                     const QString& grid, const QString& mode,
                                     int even_period, double fsked_khz,
                                     bool start_qso)
{
  if (!ui->actionSync_WSJTX->isChecked()) return;
  if (call.isEmpty()) {
    DXcall = "";  // Reset click-to-work callsign highlighting
    if (m_wide_graph_window) m_wide_graph_window->updateActiveCallsign("");
    refreshDxHighlight();
    return;
  }
  // Absolute RF in Hz. datcom_.fcenter is the QMAP wide-mode centre.
  const double click_khz = (fsked_khz >= 0) ? fsked_khz : freq_khz;
  const qint64 rf_hz =
      static_cast<qint64>(datcom_.fcenter) * 1000000LL +
      static_cast<qint64>(click_khz * 1000.0 + 0.5);
  mem_qmap.lock();
  // Bump the sequence so WSJT-X's edge detector trips on the next tick.
  decodes_.click_seq += 1;
  std::memset(decodes_.click_callsign, 0, sizeof(decodes_.click_callsign));
  std::memset(decodes_.click_grid,     0, sizeof(decodes_.click_grid));
  std::memset(decodes_.click_mode,     0, sizeof(decodes_.click_mode));
  const QByteArray cb = call.toUtf8();
  std::memcpy(decodes_.click_callsign, cb.constData(),
              std::min<size_t>(cb.size(), sizeof(decodes_.click_callsign) - 1));
  decodes_.click_rf_hz = rf_hz;
  decodes_.click_even_period = (even_period >= 0) ? even_period : 0;
  const QByteArray gb = grid.toUtf8();
  std::memcpy(decodes_.click_grid, gb.constData(),
              std::min<size_t>(gb.size(), sizeof(decodes_.click_grid) - 1));
  const QByteArray mb = mode.toUtf8();
  std::memcpy(decodes_.click_mode, mb.constData(),
              std::min<size_t>(mb.size(), sizeof(decodes_.click_mode) - 1));
  // Start-QSO intent: 1 on a double click so WSJT-X also enables Tx.
  decodes_.click_start_qso = start_qso ? 1 : 0;
  // Push the full struct so WSJT-X sees the click fields on its next
  // memcpy in. Other fields are unchanged (managed by the decode flow).
  std::memcpy(static_cast<char*>(mem_qmap.data()), &decodes_, sizeof(decodes_));
  mem_qmap.unlock();
  // Highlight the clicked call on the waterfall. updateActiveCallsign
  // sweeps is_active ONLY, so every label repaints by its TRUE status:
  // the new call turns red and any previously-selected call reverts to
  // its real colour (green CQ / default). The old approach re-added the
  // previous call via addDecodeLabel with is_cq=false, which stomped its
  // CQ flag and left it unable to paint green again. The band map paints
  // by is_cq alone (no selection colour), so it needs no update here --
  // only that its is_cq is no longer clobbered.
  DXcall = call;
  m_wide_graph_window->updateActiveCallsign(DXcall);
  refreshDxHighlight();
}

bool MainWindow::parseDecodeLine(const QString& t, QString& sender, double& freq_khz,
                                 double& fsked_khz, QString& grid, QString& mode,
                                 int& even_period, bool& is_cq)
{
  // Decode line format (whitespace-separated, after trim()):
  //   "<HHMMSS> <freq_kHz> <fQSO_kHz> <DT> <SNR> <mode> <message>"
  //   e.g. "000100  177.771  176.3   2.48  -15  60A  CQ K1JT FN20"
  sender.clear(); grid.clear(); mode.clear();
  freq_khz = -1.0; fsked_khz = -1.0; even_period = -1; is_cq = false;
  const QStringList cols = QString(t).split(QRegularExpression("\\s+"), SkipEmptyParts);
  if (cols.size() < 7) return false;
  bool ok = false;
  freq_khz = cols[1].toDouble(&ok);        // column 2 (frx): where the signal landed
  if (!ok) return false;
  bool fok = false;
  fsked_khz = cols[2].toDouble(&fok);      // column 3 (fsked): sked QSO channel
  if (!fok) fsked_khz = -1.0;              // unknown -> click falls back to frx
  // Message starts at column 6 (0-indexed). For "CQ X Y..." sender is X
  // (one past CQ, or one past "CQ DX"); else sender is the second token
  // of the directed message (one past recipient).
  int sender_idx = -1;
  if (cols[6] == "CQ" && cols.size() >= 8) {
    is_cq = true;
    if (cols[7] == "DX" && cols.size() >= 9) { sender = cols[8]; sender_idx = 8; }
    else                                     { sender = cols[7]; sender_idx = 7; }
  } else if (cols.size() >= 8) {
    sender = cols[7]; sender_idx = 7;
  }
  // Grid is the token right after the sender when it looks like a 4- or
  // 6-char Maidenhead locator (CQ messages usually carry one; replies omit).
  if (sender_idx >= 0 && cols.size() > sender_idx + 1) {
    static const QRegularExpression grid_re("^[A-R][A-R][0-9][0-9]([A-X][A-X])?$");
    if (grid_re.match(cols[sender_idx + 1]).hasMatch() && cols[sender_idx + 1] != "RR73") grid = cols[sender_idx + 1];
  }
  // Mode is column 5 (0-indexed): the Q65 sub-mode designator ("60A" etc).
  mode = cols[5].trimmed();
  // Even/odd Tx-sequence parity. Period = leading digits of the mode token;
  // cols[0] is HHMMSS (q65b.f90 stamps the 2nd Q65-30 half-minute :30, so
  // the two 30s periods inside one minute resolve to OPPOSITE parity).
  {
    int nd = 0;
    while (nd < mode.size() && mode[nd].isDigit()) ++nd;
    const int period = mode.left(nd).toInt();
    bool tok_ok = false;
    const int hhmmss = cols[0].toInt(&tok_ok);
    if (tok_ok && period > 0) {
      const int sod = (hhmmss / 10000) * 3600
                    + ((hhmmss / 100) % 100) * 60
                    + (hhmmss % 100);
      even_period = (((sod / period) % 2) == 0) ? 1 : 0;
    }
  }
  return !sender.isEmpty();
}

void MainWindow::handleDecodeLineClick(const QString& line, bool start_qso)
{
  QString sender, grid, mode;
  double freq_khz = -1.0, fsked_khz = -1.0;
  int even_period = -1;
  bool is_cq = false;
  if (!parseDecodeLine(line.trimmed(), sender, freq_khz, fsked_khz, grid, mode,
                       even_period, is_cq)) return;
  handleCallsignClick(sender, freq_khz, grid, mode, even_period, fsked_khz, start_qso);
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
  // Click on a decoded-text line == clicking that callsign on the waterfall.
  // Single click (MouseButtonPress) transfers only; double click also
  // enables Tx to start the QSO.
  if (object == ui->decodedTextBrowser->viewport()
      && (event->type() == QEvent::MouseButtonPress
          || event->type() == QEvent::MouseButtonDblClick)) {
    QMouseEvent *me = static_cast<QMouseEvent *>(event);
    if (me->button() == Qt::LeftButton) {
      QTextCursor cur = ui->decodedTextBrowser->cursorForPosition(me->pos());
      handleDecodeLineClick(cur.block().text(),
                            event->type() == QEvent::MouseButtonDblClick);
    }
    return false;
  }
  // Only the WideGraph installs this filter for key routing; don't route
  // the decoded-text viewport's keys (scroll/copy) into keyPressEvent.
  if (event->type() == QEvent::KeyPress
      && object != ui->decodedTextBrowser->viewport()) {
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
  lab1->setMinimumSize(QSize(110,10));
  lab1->setStyleSheet("QLabel{background-color: #00ff00}");
  lab1->setFrameStyle(QFrame::Panel | QFrame::Sunken);
  statusBar()->addWidget(lab1);

  lab2 = new QLabel("");
  lab2->setAlignment(Qt::AlignHCenter);
  lab2->setMinimumSize(QSize(80,10));
  lab2->setFrameStyle(QFrame::Panel | QFrame::Sunken);
  statusBar()->addWidget(lab2);

  lab3 = new QLabel("");
  lab3->setAlignment(Qt::AlignHCenter);
  lab3->setMinimumSize(QSize(60,10));
  lab3->setFrameStyle(QFrame::Panel | QFrame::Sunken);
  statusBar()->addWidget(lab3);

  lab4 = new QLabel("");
  lab4->setAlignment(Qt::AlignHCenter);
  lab4->setMinimumSize(QSize(80,10));
  lab4->setFrameStyle(QFrame::Panel | QFrame::Sunken);
  statusBar()->addWidget(lab4);

  lab5 = new QLabel("");
  lab5->setAlignment(Qt::AlignHCenter);
  lab5->setMinimumSize(QSize(100,10));
  lab5->setFrameStyle(QFrame::Panel | QFrame::Sunken);
  lab5->setStyleSheet("");
  statusBar()->addWidget(lab5);

  lab8 = new QLabel("");
  lab8->setAlignment(Qt::AlignHCenter);
  lab8->setMinimumSize(QSize(70,10));
  lab8->setFrameStyle(QFrame::Panel | QFrame::Sunken);
  lab8->setStyleSheet("");
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
  if (m_gui_timer) m_gui_timer->stop ();
  m_wide_graph_window->saveSettings();
  if (m_bandmap_window) { m_bandmap_window->markShutdown(); m_bandmap_window->saveSettings(); }
  // Capture Astro visibility BEFORE closing it -- writeSettings runs
  // later (destructor), when isVisible() is already false.
  m_astroOpen = m_astro_window && m_astro_window->isVisible();
  if (m_astro_window) m_astro_window->close ();
  if (m_wide_graph_window) m_wide_graph_window->close ();
  QMainWindow::closeEvent (e);
}

void MainWindow::msgBox(QString t)                             //msgBox
{
  msgBox0.setText(t);
  msgBox0.exec();
}

void MainWindow::on_actionAstro_Data_triggered()             //Display Astro
{
  if (m_astro_window ) m_astro_window->show();
}

void MainWindow::on_actionWide_Waterfall_triggered()      //Display Waterfalls
{
  m_wide_graph_window->show();
}

void MainWindow::on_actionBand_Map_triggered()            //Display Band Map
{
  m_bandmap_window->show();
  m_bandmap_window->raise();
  m_bandmap_window->activateWindow();
}

void MainWindow::on_actionOpen_QMAP_data_directory_triggered()
{
  QDesktopServices::openUrl (QUrl::fromLocalFile (QDir {m_dataDir}.absolutePath()));
}

void MainWindow::on_actionOpen_triggered()                     //Open File
{
  m_monitoring=false;
  soundInThread.setMonitoring(m_monitoring);
  if (m_wide_graph_window) m_wide_graph_window->clearDecodeLabels();
  QString fname;
  fname=QFileDialog::getOpenFileName(this, "Open File", m_path,
                                     "EME65/WS-MAP Files (*.iq *.qm)");
  if(fname != "") {
    m_path=fname;
    int i;
    i=qMax(fname.indexOf(".iq") - 11, fname.indexOf(".qm") - 11);
    if(i>=0) {
      lab1->setStyleSheet("QLabel{background-color: #66ff66}");
      lab1->setText(" " + fname.mid(i,15) + " ");
    }
    if(m_monitoring) on_monitorButton_clicked();
    m_diskData=true;
    int dbDgrd=0;
    int iret=4;
    if(m_path.indexOf(".iq")>0) {
      getfile(fname, dbDgrd);
    } else {
      read_qm_(fname.toLatin1(), &iret, fname.length());
    }
    if(iret > 0) diskDat(iret);
  }
}

void MainWindow::on_actionOpen_next_in_directory_triggered()   //Open Next
{
  if (m_wide_graph_window) m_wide_graph_window->clearDecodeLabels();
  int i,len;
  QFileInfo fi(m_path);
  QStringList list;
  if(m_path.indexOf(".iq")>0) {
    list= fi.dir().entryList().filter(".iq");
  } else {
    list= fi.dir().entryList().filter(".qm");
  }
  for (i = 0; i < list.size()-1; ++i) {
    if(i==list.size()-2) m_loopall=false;
    len=list.at(i).length();
    if(list.at(i)==m_path.right(len)) {
      int n=m_path.length();
      QString fname=m_path.replace(n-len,len,list.at(i+1));
      m_path=fname;
      int i;
      i=qMax(fname.indexOf(".iq") - 11, fname.indexOf(".qm") - 11);
      if(i>=0) {
        lab1->setStyleSheet("QLabel{background-color: #66ff66}");
        lab1->setText(" " + fname.mid(i,len) + " ");
      }
      m_diskData=true;
      int dbDgrd=0;
      int iret=4;
      if(m_path.indexOf(".iq")>0) {
        getfile(fname, dbDgrd);
      } else {
        read_qm_(fname.toLatin1(), &iret, fname.length());
      }
      if(iret > 0) diskDat(iret);
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

void MainWindow::diskDat(int iret)                                   //diskDat()
{
  int ia=0;
  int ib=400;
  if(iret==1) ib=202;
  m_bDiskDatBusy=true;
  double hsym;
  //These may be redundant??
  m_diskData=true;
  datcom_.newdat=1;
  m_nTx30a=datcom_.ntx30a;
  m_nTx30b=datcom_.ntx30b;
  hsym=0.15*96000.0;                   //Samples per Q65-30x half-symbol or Q65-60x quarter-symbol
  for(int i=ia; i<ib; i++) {           // Do the half-symbol FFTs
    int k = i*hsym + 0.5;
    if(k > 60*96000) break;
    dataSink(k);
    qApp->processEvents();             // Allow the waterfall to update
    while(m_decoderBusy) {
      qApp->processEvents();           // Wait for an early decode to finish
    }
  }
  m_bDiskDatBusy=false;
  // --exit-after-decode: the post-diskDat fallback is no longer
  // needed — the disk-load path always runs decode() after the
  // half-symbol loop, and decoderFinished() is the single canonical
  // quit trigger for both disk-load and wire-mode.
}

void MainWindow::decoderFinished()
{
  // Decoder kernel just returned. Capture elapsed first so the log
  // tap below reflects the actual q65c_ wall-time, not whatever
  // mainwindow housekeeping runs after.
  const qint64 decoder_ms =
      m_decoderTimer.isValid() ? m_decoderTimer.elapsed() : -1;

  m_startAnother=m_loopall;
  decodes_.nQDecoderDone=1;
  decodes_.kHzRequested=0;
  if(m_diskData) decodes_.nQDecoderDone=2;
  mem_qmap.lock();
  decodes_.nWDecoderBusy=ipc_wsjtx[3];                   //Prevent overwriting values
  decodes_.nWTransmitting=ipc_wsjtx[4];                  //written here by WSJT-X
  m_bWTransmitting=decodes_.nWTransmitting>0;
  // A double-click's start-QSO intent must not outlive the click. WSJT-X
  // consumes a click within 100 ms of it being written; if click_seq has
  // not moved since the PREVIOUS push here, the click is a full period old
  // and long acted on (or discarded), so the flag has done its job. Clear
  // it before re-pushing the struct, or a stale double-click sits in the
  // segment as "start QSO" for as long as QMAP runs.
  if (decodes_.click_seq==m_lastPushedClickSeq) decodes_.click_start_qso=0;
  m_lastPushedClickSeq=decodes_.click_seq;
  memcpy((char*)ipc_wsjtx, &decodes_, sizeof(decodes_)); //Send decodes and flags to WSJT-X
  mem_qmap.unlock();
  // guiUpdate()'s own once-per-second block is the usual place lab1
  // gets repainted from m_bWTransmitting, but that tick is gated on
  // QMAP's half-symbol clock (nsec) and can stall independently of
  // whether WSJT-X has actually stopped transmitting -- leaving the
  // yellow "WSJT-X Transmitting" indicator stuck even after the shared
  // nWTransmitting flag has already cleared. Clear it here immediately
  // instead of waiting on that tick; the fuller "Receiving ..." status
  // text (nrx/khsym-dependent) still refreshes normally within the
  // next second via guiUpdate() once monitoring resumes.
  if (!m_bWTransmitting) {
    if (m_monitoring) {
      lab1->setStyleSheet("QLabel{background-color: #00ff00}");
      lab1->setText("Receiving");
    } else if (!m_diskData) {
      lab1->setStyleSheet("");
      lab1->setText("");
    }
  }
  QString t1;
  t1=t1.asprintf(" %.1f s  %d/%d ", 0.15*datcom2_.nhsym, decodes_.ndecodes, decodes_.ncand);
  lab4->setText(t1);
  decodeBusy(false);

  // Regression-harness taps. Both run only when --decode-log /
  // --exit-after-decode were passed on argv; in normal interactive
  // use this whole block is no-ops.
  //
  // 1) Append "# decoder_ms=<n> ndecodes=<n> rate=<hz>" to the decode
  //    log. Lets the harness display real kernel time (separate from
  //    QProcess wall-clock, which in wire mode is dominated by the
  //    TR-boundary wait). decoder_ms > TR period = bad — overlaps the
  //    next decode cycle.
  // 2) Schedule QApplication::quit() 50 ms after the first decoder
  //    cycle completes (whether or not it produced a decode). Quitting
  //    here unifies the disk-load and wire-mode paths — disk would
  //    have decoded and exited via the per-line tap before, wire
  //    needed this fallback. m_harnessQuitScheduled prevents double-
  //    firing on a TR-30 file that runs a second cycle before quit
  //    actually lands.
  if (!qmap_runtime::g_decode_log_path.isEmpty()) {
    QFile log(qmap_runtime::g_decode_log_path);
    if (log.open(QIODevice::Append | QIODevice::Text)) {
      QTextStream s(&log);
      s << QString("# decoder_ms=%1 ndecodes=%2 rate=%3\n")
             .arg(decoder_ms)
             .arg(decodes_.ndecodes)
             .arg(qmap_runtime::activeRateHz());
    }
  }
  // Wire mode has three decoder triggers per TR-60 cycle (ihsym=130
  // / 330 / 390). The Q65-30 burst typically lands in the 330 cycle
  // (full 30 s window with WAV[0..49.5] buffered); the 130 cycle
  // sees only WAV[0..19.5] and may miss the burst. Quit ASAP on a
  // successful decode, otherwise wait until the last cycle of the
  // TR period (m_hsymStop, ~58.5 s past minute) before giving up.
  // Disk-load runs the decoder once per file, so quit on its first
  // decoderFinished regardless.
  const bool wire_last_chance =
      m_lastDecodeTriggerNhsym >= m_hsymStop;
  const bool should_quit =
      qmap_runtime::g_exit_after_decode && !m_harnessQuitScheduled &&
      (m_diskData || decodes_.ndecodes > 0 || wire_last_chance);
  if (should_quit) {
    m_harnessQuitScheduled = true;
    // When the kernel produced decodes, the GUI's decoded-text-browser
    // append (which is where the --decode-log tap lives) runs on a
    // later main-thread tick than this slot. At 96 kHz that's ~50 ms.
    // At 256 kHz the wider FFT cycle pushes everything out and the
    // append can lag 200+ ms. Wait longer so the decode line lands in
    // the log file before we exit. No-decode path stays fast.
    const int quit_delay_ms = (decodes_.ndecodes > 0) ? 750 : 50;
    QTimer::singleShot(quit_delay_ms, qApp, &QCoreApplication::quit);
  }

  if(m_bDecodeAgain) {
    datcom_.nhsym=390;
    datcom_.nagain=1;
    m_bDecodeAgain=false;
    decode();
  }
}

void MainWindow::on_actionDelete_all_iq_files_in_SaveDir_triggered()
{
  int i;
  QString fname;
  int ret = QMessageBox::warning(this, "Confirm Delete",
      "Are you sure you want to delete all *.iq and *.qm files in\n" +
       QDir::toNativeSeparators(m_saveDir) + " ?",
       QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
  if(ret==QMessageBox::Yes) {
    QDir dir(m_saveDir);
    QStringList files=dir.entryList(QDir::Files);
    QList<QString>::iterator f;
    for(f=files.begin(); f!=files.end(); ++f) {
      fname=*f;
      i=(fname.indexOf(".iq"));
      if(i==11) dir.remove(fname);
      i=(fname.indexOf(".qm"));
      if(i==11) dir.remove(fname);
    }
  }
}

void MainWindow::on_actionNone_triggered()                    //Save None
{
  m_saveAll=false;
  m_saveDecoded=false;
  lab5->setStyleSheet("");
  lab5->setText("");
}

void MainWindow::on_actionSave_decoded_triggered()
{
  m_saveDecoded=true;
  m_saveAll=false;
  lab5->setStyleSheet("QLabel{background-color: #ffff00}");
  lab5->setText("Save decoded");
}

void MainWindow::on_actionSave_all_triggered()
{
  m_saveAll=true;
  m_saveDecoded=false;
  lab5->setStyleSheet("QLabel{background-color: #ffff00}");
  lab5->setText("Save all");
}

void MainWindow::on_DecodeButton_clicked()                    //Decode request
{
  if(!m_decoderBusy) {
    datcom_.newdat=0;
    datcom_.nagain=1;
    if(m_bAlso30 and m_nTx30a<5) {
      datcom_.nhsym=200;                   //Decode the first half-minute
      if(m_nTx30b<5) m_bDecodeAgain=true;  //Queue up decoding of the seciond half minute
    }
    decode();
  }
}

void MainWindow::freezeDecode(int n)                          //freezeDecode()
{
  if(n==3) {
    decodes_.kHzRequested=m_wide_graph_window->QSOfreq();
    mem_qmap.lock();
    ipc_wsjtx[5]=decodes_.kHzRequested;
    mem_qmap.unlock();
    return;
  }
  if(n==2) {
    ui->tolSpinBox->setValue(5);
    datcom_.ntol=m_tol;
    datcom_.mousedf=0;
  } else {
    ui->tolSpinBox->setValue(qMin(3,ui->tolSpinBox->value()));
    datcom_.ntol=m_tol;
  }
  m_nDoubleClicked++;
  if(!m_decoderBusy) {
    datcom_.nagain=1;
    datcom_.newdat=0;
    on_DecodeButton_clicked();
  }
}

void MainWindow::decode()                                       //decode()
{
  if(m_decoderBusy) {
    return;  //Don't attempt decode if decoder already busy
  }
  decodeBusy(true);
  QString fname="           ";
  if(datcom_.nagain==0 && (!m_diskData)) {
    qint64 ms = QDateTime::currentMSecsSinceEpoch() % 86400000;
    int imin=ms/60000;
    int ihr=imin/60;
    imin=imin % 60;
    datcom_.nutc=100*ihr + imin;
  }

  datcom_.mousedf=m_wide_graph_window->DF() + m_fCal;
  datcom_.mousefqso=m_wide_graph_window->QSOfreq();
  datcom_.fselected=datcom_.mousefqso + 0.001*datcom_.mousedf;
  datcom_.ndiskdat=0;
  if(m_diskData) {
    datcom_.ndiskdat=1;
    int i0=qMax(m_path.indexOf(".iq"),m_path.indexOf(".qm"));
    if(i0>0) {
      fname=m_path.mid(i0-11,11);
    }
  }

  int ispan=int(m_wide_graph_window->fSpan());
  if(ispan%2 == 1) ispan++;
  int ifc=int(1000.0*(datcom_.fcenter - int(datcom_.fcenter))+0.5);
  int nfa=m_wide_graph_window->nStartFreq();
  int nfb=nfa+ispan;
  int nfshift=nfa + ispan/2 - ifc;

  datcom_.nfa=nfa;
  datcom_.nfb=nfb;
  datcom_.nfcal=m_fCal;
  datcom_.nfshift=nfshift;
  datcom_.ntol=m_tol;
  m_nutc0=datcom_.nutc;
  datcom_.nfsample=qmap_runtime::activeRateHz();    //was: 96000 hardcoded
  datcom_.nBaseSubmode=m_modeQ65;
  datcom_.max_drift=ui->sbMaxDrift->value();
  datcom_.offset=ui->sbOffset->value();
  // CFOM (self-Doppler IQ pre-shift) gate. Polled per-decode-cycle from
  // the Astro window's checkbox; the Fortran side (qmapa.f90) skips the
  // call to cfom() when nCFOM == 0 so the 96 kHz baseline path stays
  // byte-identical to upstream.
  datcom_.nCFOM = (m_astro_window && m_astro_window->isCFOMEnabled()) ? 1 : 0;
  datcom_.ndepth=1;
  if(datcom_.nagain==1)   datcom_.ndepth=3;

  QString mcall=(m_myCall+"            ").mid(0,12);
  QString mgrid=(m_myGrid+"            ").mid(0,6);

  memcpy(datcom_.mycall, mcall.toLatin1(), 12);
  memcpy(datcom_.mygrid, mgrid.toLatin1(), 6);
  if(m_diskData) {
    memcpy(datcom_.datetime, fname.toLatin1(), 11);
  } else {
    memcpy(datcom_.datetime, m_dateTime.toLatin1(), 11);
  }
  datcom_.ntx30a=m_nTx30a;
  datcom_.ntx30b=m_nTx30b;
  datcom_.ntx60=m_nTx60;

  datcom_.nsave=0;
  if(m_saveDecoded) datcom_.nsave=1;
  if(m_saveAll) datcom_.nsave=2;

  datcom_.n60=m_n60;
  datcom_.junk1=1234;                                     //Check for these values in m65
  datcom_.junk2=5678;
  datcom_.bAlso30=m_bAlso30;
  datcom_.ndop00=m_dop00;
  datcom_.ndop58=m_dop58;

  char *to = (char*) datcom2_.d4;
  char *from = (char*) datcom_.d4;
  memcpy(to, from, sizeof(datcom_));    //Copy the full datcom_ common block into datcom2_

  datcom_.ndiskdat=0;

  if((!m_bAlso30 and (datcom2_.nhsym==330)) or (m_bAlso30 and (datcom2_.nhsym==130))) {
    decodes_.ndecodes=0;    //Start the decode cycle with a clean slate
    m_fetched=0;
  }
  decodes_.ncand=0;
  decodes_.nQDecoderDone=0;

  m_saveFileName="NoSave";
  if(!m_diskData) {
    QDateTime t = QDateTime::currentDateTimeUtc();
    m_dateTime=t.toString("yyMMdd_hhmm");
    QDir dir(m_saveDir);
    if (!dir.exists()) dir.mkpath(".");
    m_saveFileName=m_saveDir + "/" + m_dateTime + ".qm";
  }

  bool bSkipDecode=false;
  //No need to call decoder for first half, if we transmitted in the first half:
  if((datcom2_.nhsym<=200) and (m_nTx30a>5)) bSkipDecode=true;
  //No need to call decoder at 330, if we transmitted in 2nd half:
  if((datcom2_.nhsym==330) and (m_nTx30b>5)) bSkipDecode=true;
  //No need to call decoder at all, if we transmitted in a 60 s submode.
  if(m_nTx60>5) bSkipDecode=true;

  if(bSkipDecode) {
    decodeBusy(false);
    return;
  }

  int len1=m_saveFileName.length();
  int len2=m_revision.length();

  memcpy(savecom_.revision, m_revision.toLatin1(), len2);
  memcpy(savecom_.saveFileName, m_saveFileName.toLatin1(),len1);

  ui->actionExport_wav_file_at_fQSO->setEnabled(m_diskData);
  m_lastDecodeTriggerNhsym = datcom_.nhsym;
  m_decoderTimer.start();
  watcher3.setFuture(QtConcurrent::run (q65c_));
  decodeBusy(true);
}

void MainWindow::on_EraseButton_clicked()
{
  ui->decodedTextBrowser->clear();
  lab4->clear();
  m_nline=0;
}

void MainWindow::mousePressEvent(QMouseEvent *event)    // mouse press events
{
  if(ui->EraseButton->hasFocus()) {                             // Erase button
    if (event->button() & Qt::RightButton) {
      DXcall = "";
      if (m_wide_graph_window) m_wide_graph_window->updateActiveCallsign("");
      refreshDxHighlight();
    }
    ui->EraseButton->clearFocus();
  }
}

// Background colour rules for one decoded-text row. Applied when the line is
// appended, and re-applied by refreshDxHighlight() when the DX Call selection
// changes -- keep this the single home of the rules so the two never drift.
QBrush MainWindow::decodeRowBackground(const QString& t) const
{
  QBrush bg {Qt::white};
  if(t.mid(36,2)=="30") bg=QBrush(Qt::yellow);
  if(t.indexOf(m_myCall)>10 and m_myCallColor==1) bg=QBrush(QColor(255,102,102));
  if(t.indexOf(m_myCall)>10 and m_myCallColor==2) bg=QBrush(Qt::green);
  if(t.indexOf(m_myCall)>10 and m_myCallColor==3) bg=QBrush(Qt::cyan);
  if (!t.contains(" " + m_myCall + " ") && DXcall != "" && t.mid(42).contains(" " + DXcall + " ")) bg=QBrush(QColor(255,215,225));
  return bg;
}

// Recolour every row already in the decoded-text window against the current
// DX Call, so a selection change (a local click or the WSJT-X mirror) moves
// the light-red highlight on history instead of only on future decodes.
void MainWindow::refreshDxHighlight()
{
  QTextDocument* doc = ui->decodedTextBrowser->document();
  for (QTextBlock block = doc->firstBlock(); block.isValid(); block = block.next()) {
    QTextCursor cursor(block);
    QTextBlockFormat f = cursor.blockFormat();
    f.setBackground(decodeRowBackground(block.text()));
    cursor.setBlockFormat(f);
  }
}

void MainWindow::decodeBusy(bool b)                             //decodeBusy()
{
  m_decoderBusy=b;
  ui->DecodeButton->setEnabled(!b);
  if(!b) ui->DecodeButton->setStyleSheet("");
  if(b) ui->DecodeButton->setStyleSheet(m_pbdecoding_style1);
  ui->actionOpen->setEnabled(!b);
  ui->actionOpen_next_in_directory->setEnabled(!b);
  ui->actionDecode_remaining_files_in_directory->setEnabled(!b);
}

// QSO-sequence position of a directed message, from the payload word the RX
// decoder produced. Everything here is inferred from decoded RF -- LiveCQ
// reports what came off the air, nothing else.
static void classifyPayload(const QString& payloadIn, int* slot, bool* is73, bool* isRR73)
{
  const QString payload = payloadIn.trimmed().toUpper();
  *slot = 0; *is73 = false; *isRR73 = false;
  if (payload == "RR73")      { *slot = 4; *isRR73 = true; }
  else if (payload == "RRR")    *slot = 4;
  else if (payload == "73")   { *slot = 5; *is73 = true; }
  else if (payload.size() >= 3 && payload[0] == 'R'
           && (payload[1] == '-' || payload[1] == '+')) *slot = 3;
  else if (payload.startsWith('-') || payload.startsWith('+')) *slot = 2;
  else if (QRegularExpression("^[A-R]{2}[0-9]{2}$").match(payload).hasMatch()) *slot = 1;
}

// PSK Reporter: WS-MAP spots its own decodes with the shared client from
// Network/ (as WS and EME65 do); WS no longer spots them on its behalf.
// Off until enabled in Setup, and only with a valid My Call and My Grid.
void MainWindow::updatePSKReporter()
{
  if (!m_spotPSK || m_myCall.size() < 3 || m_myGrid.size() < 4) {
    m_pskReporter.reset();
    return;
  }
  if (!m_pskReporter) {
    m_pskReporter.reset(new PSKReporter {
      {m_spotPSKTcpIp,
       QCoreApplication::applicationDirPath() + "/eclipse.txt",
       QString {"WS-MAP v" + QCoreApplication::applicationVersion()}.simplified()}
    });
  }
  m_pskReporter->setLocalStation(m_myCall.toUpper(), m_myGrid.toUpper(), "N/A", "N/A (WS-MAP)");
}

void MainWindow::spotToPSKReporter(QString const& sender, QString const& grid, qint64 rf_hz,
                                   QString const& mode, int snr, QString const& hhmmss, bool is_cq)
{
  if (!m_pskReporter || m_diskData) return;                     // live decodes only
  if (grid.isEmpty() && !is_cq) return;                         // a locator or a CQ, as WS spotted them
  if (sender.compare(m_myCall, Qt::CaseInsensitive) == 0) return;  // never spot ourselves
  const QTime t {hhmmss.mid(0,2).toInt(), hhmmss.mid(2,2).toInt(), hhmmss.mid(4,2).toInt()};
  const QDateTime now = QDateTime::currentDateTimeUtc();
  QDateTime when {now.date(), t, Qt::UTC};
  if (when > now.addSecs(60)) when = when.addDays(-1);          // decoded just before midnight
  m_pskReporter->addRemoteStation(sender, grid, static_cast<Radio::Frequency>(rf_hz), mode, snr, when);
}

void MainWindow::CreateLiveCQ(QStringList cqliveText)
{
  // Disk decodes upload for everyone now, carrying the flags disk bit; the
  // server hides them by default and sendLiveCQData routes them to N6NU only.
  // This replaces the old W3SZ/DL3WDG callsign whitelist, which silently
  // dropped every other operator's file replays -- and, conversely, let those
  // two upload disk decodes UNFLAGGED to all destinations.
  if (cqliveText.size() == 0) return;

  QStringList cqliveFinalText;
  QStringList oldFile;
  bool ok;
  int freqOffset = ui->sbOffset->value();
  QStringList bandInfo;
  bandInfo = ui->labFreq->text().split(".",SkipEmptyParts);
  QString bandFreq = bandInfo.at(0);
  QString theDate = ui->labUTC->text().trimmed().mid(0,12);
  QList<QStringList> decodeList;
  bool strOK = false;

  for (const QString &item : cqliveText) {
    QString line = " ";
    QStringList thePostLine;
    line = line.repeated(100);  //.replace("<","").replace(">","");
    QStringList thePieces;
    thePieces = item.split(" ",SkipEmptyParts);
    int rxFreq = 0.0;
    if (thePieces.size() < 9) continue;
    const QString w6 = thePieces.at(6).trimmed().toUpper();
    const bool isCQtype = (w6 == "CQ" || w6 == "QRZ" || w6 == "CQV" || w6 == "CQH" || w6 == "QRT");
    // A valid callsign as the first message word is a DIRECTED message with
    // that word as the addressee: "K1JT W7GJ RR73".
    const bool isDir = !isCQtype && testCall(w6);
    if((isCQtype || isDir) && m_myCall.length() >=3 && m_myGrid.length()>=4  ) {
      try {
        //extract Fsked freq and format to 3 digits no decimals
        QString theMsg;
        QString theCall;
        QString theGrid;
        QStringList thekHz;
        int nWords=thePieces.length();
        QString thePayload;         // last message word of a DIR, if any
        if(nWords==9) {
          // Handle CQ CALL (or TOCALL CALL) messages without a third word
          if(thePieces.at(6).isEmpty() or thePieces.at(7).isEmpty() or thePieces.at(8).isEmpty()) continue;
          theCall = thePieces.at(7);
          bool isCall = testCall(theCall);
          if(!isCall) continue;
          theGrid = "--";
          theMsg = thePieces.at(6) + " " + theCall;
          thekHz = thePieces.at(8).split(".");
          rxFreq = freqOffset + thekHz.at(1).toInt(&ok);
        // int rxFreq = freqOffset + 100 * thekHz.at(1).toInt(&ok);
          if (!ok) continue;
        } else if(nWords==10) {
          // Handle CQ CALL GRID --or-- CQ XXX CALL
          if(thePieces.at(6).isEmpty() or thePieces.at(7).isEmpty() or thePieces.at(8).isEmpty() or thePieces.at(9).isEmpty()) continue;
          // Test for callsign at thePieces.at(7)
          theCall = thePieces.at(7);
          bool isCall = testCall(theCall);
          // Handle CQ CALL GRID (or TOCALL CALL X, X = grid or exchange)
          if(isCall) {
            if (isDir) {
              thePayload = thePieces.at(8);
              theGrid = "--";            // set below only if X is a real grid
              theMsg = thePieces.at(6) + " " + theCall + " " + thePayload;
            } else {
              theGrid = thePieces.at(8);
              theMsg = thePieces.at(6) + " " + theCall + " " + theGrid;
            }
          }
          // Handle CQ XXX CALL
          else {
            if (isDir) continue;         // DIR sender must be a callsign
            theCall = thePieces.at(8);
            isCall = testCall(theCall);
            if(!isCall) continue;
            theGrid = "--";
            theMsg = thePieces.at(6) + " " + theCall;
          }
          thekHz = thePieces.at(9).split(".");
          rxFreq = freqOffset + thekHz.at(1).toInt(&ok);
        // int rxFreq = freqOffset + 100 * thekHz.at(1).toInt(&ok);
          if (!ok) continue;
          // Handle CQ XXX CALL GRID
        } else if (nWords==11) {
           if (isDir) continue;          // no four-word directed shape
           if(thePieces.at(6).isEmpty() or thePieces.at(7).isEmpty() or thePieces.at(8).isEmpty() or thePieces.at(9).isEmpty() or thePieces.at(10).isEmpty()) continue;
          theCall = thePieces.at(8);
          bool isCall = testCall(theCall);
          if(!isCall) continue;
          theGrid = thePieces.at(9);
          theMsg = thePieces.at(6) + " " + theCall + " " + theGrid;
          thekHz = thePieces.at(10).split(".");
          rxFreq = freqOffset + thekHz.at(1).toInt(&ok);
        // int rxFreq = freqOffset + 100 * thekHz.at(1).toInt(&ok);
          if (!ok) continue;
        }
        else continue;
        strOK = true;
        int skedFreq;
        QString skedFreqString;
        if (rxFreq <= freqOffset + 500) {
          skedFreq = thekHz.at(0).toInt(&ok);
        } else {
          skedFreq = thekHz.at(0).toInt(&ok) + 1;
          rxFreq=rxFreq - 1000;
        }
        skedFreqString = QString::number(skedFreq).rightJustified(3,'0');
        QString mode = "0 Q65-" + thePieces.at(5);
        line.insert(0,bandFreq + "." + skedFreqString);
        line.insert(10,QString::number(rxFreq));
        line.insert(15,"0");
        line.insert(18,thePieces.at(0));
        line.insert(26,thePieces.at(3));
        line.insert(32,thePieces.at(4));
        line.insert(36,theMsg);
        line.insert(55,mode);
        line.insert(67,m_myGrid.toUpper());
        line.insert(74,"Q");
        line.insert(76,theDate);
        line.insert(88,m_myCall.toUpper());
        cqliveFinalText << line.trimmed();
        //qDebug () << "cqliveFinalText is: " << cqliveFinalText;

        thePostLine.insert(0, bandFreq + "." + skedFreqString);  //skedfreq
        thePostLine.insert(1, QString::number(rxFreq)); //rxfreq
        thePostLine.insert(2, "--"); //rpol
        thePostLine.insert(3,thePieces.at(0)); //utc HHmmSS
        thePostLine.insert(4,thePieces.at(3)); //dt
        thePostLine.insert(5, thePieces.at(4)); //dB
        thePostLine.insert(6, "Q65-" + thePieces.at(5)); //Q65 submode
        // Flags: one char, ASCII 48 + 6 bits -- QSO-sequence position
        // inferred by the RX decoder from the received message shape, plus
        // the disk-input bit.
        int slot = 0; bool is73 = false, isRR73 = false;
        if (isDir) {
          classifyPayload(thePayload, &slot, &is73, &isRR73);
          if (slot == 1) theGrid = thePayload.toUpper();
        } else {
          slot = 6;                      // CQ-family = Tx6 shape
        }
        const int flagsVal = slot | (m_diskData ? 8 : 0) | (is73 ? 16 : 0) | (isRR73 ? 32 : 0);
        thePostLine.insert(7, isDir ? QString("DIR") : w6); //msg type
        thePostLine.insert(8, theCall); //dx call
        thePostLine.insert(9, theGrid); //dx grid
        thePostLine.insert(10, m_myGrid.toUpper()); //myGrid
        thePostLine.insert(11, theDate);  //the date
        thePostLine.insert(12, m_myCall.toUpper()); //myCall
        thePostLine.insert(13, "--"); //txpol
        thePostLine.insert(14, isDir ? w6 : QString()); //tocall (addressee)
        thePostLine.insert(15, QString(QChar(48 + flagsVal))); //flags char
        decodeList.append(thePostLine);
        //qDebug () << "thePostLine is: " << thePostLine;
      }
      catch (const std::exception& e) {
          // Handle standard C++ exceptions
          QMessageBox::critical(this, "Exception", "Exception at line 1116 MainWindow::CreateLiveCQ " + QString::fromStdString(e.what())); 
      }
      catch (...) {
          // Handle any other type of exception
          QMessageBox::critical(this, "Exception", "Unknown Exception at line 1121 MainWindow::CreateLiveCQ"); 
      }
  }
}
  if(strOK) {
  sendLiveCQData(decodeList);
  }
}

bool MainWindow::testCall(QString w)
{
// Check "callsign" to see if it could be a valid standard callsign or a valid
// compound callsign.
// Return a logical "call ok" indicator.
  if(w.indexOf('.') >= 0) return false;
  if(w.indexOf('+') >= 0) return false;
  if(w.indexOf('-') >= 0) return false;
  if(w.indexOf('?') >= 0) return false;
  w = w.replace('<',"");
  w = w.replace('>',""); 
  int n1=w.length();
  if(n1 > 11) return false;
  //qDebug() << "Line 1186 w is: " << w << " and i0 is: " << i0 << " and n1 is: " << n1 << " and call is: " << w;
  QString bc = QString();
  QStringList wSplit = w.split("/");
  if(wSplit.length() > 1) {
    if(wSplit.at(0).length() > wSplit.at(1).length()) {
      bc = wSplit.at(0);
    }
    else {
      bc = wSplit.at(1);
    }
  }
  else {
    bc = w;
  }
  int nbc=bc.trimmed().length();
  if(nbc > 8) return false;  //Base call should have no more than 8 characters  e.g. YW18FIFA

// One of first two characters (c1 or c2) must be a letter
  if((!bc[0].isLetter()) && (!bc[1].isLetter())) return false;
// Real calls don't start with Q, but we'll allow the placeholder
// callsign QU1RK to be considered a standard call:
  if(bc[0]=='Q' && bc.mid(0,5) != "QU1RK") return false;

// Must have a digit in 2nd or 3rd or 4th position
  int i1=0;
  if(bc[1].isDigit()) i1=1;
  if(bc[2].isDigit()) i1=2;
  if(bc[3].isDigit()) i1=3;
  if(i1==0) return false;

// Callsign must have a suffix of 1-4 letters e.g. YW18FIFA
  if(i1==nbc) return false;
  int n=0;
  QChar j=QChar();
  for (int i=i1+1; i<=nbc-1; ++i) {
     j=bc[i];
     if(j<QChar('A') || j > QChar('Z')) return false;
     n=n+1;
  }
  if(n >= 1 && n <= 4) return true;
  
  return false;  
}

void MainWindow::sendLiveCQData(QList<QStringList>decodeList)
{
  if (decodeList.size() == 0) return;
  if (!m_livecqEnabled) return;
  // Independent destinations: one decode fans out to every enabled URL.
  // Custom is inert unless its URL parses as http(s).
  QStringList urls;
  if (m_livecqOfficial) urls << w3szUrlAddr;
  if (m_livecqN6NU) urls << n6nuUrlAddr;
  if (m_livecqCustom && m_otherUrl.trimmed().startsWith("http")) urls << m_otherUrl.trimmed();
  if (urls.isEmpty()) return;

  if (!m_livecqNAM) m_livecqNAM = new QNetworkAccessManager(this);

  for (const QStringList &thePostLine : decodeList) {

    QString utcdatetimestringOriginal = thePostLine.at(11) + " " + thePostLine.at(3);
    QDateTime utcdatetimeUTC = QDateTime::fromString(utcdatetimestringOriginal, "yyyy MMM dd  HHmmss");
    utcdatetimeUTC.setTimeSpec((Qt::UTC));
    QString utcdatetimeUTCString = utcdatetimeUTC.toString("yyyy-MM-ddTHH:mm:ss");
    utcdatetimeUTCString = utcdatetimeUTCString + "Z";

    const QString tocall = thePostLine.size() > 14 ? thePostLine.at(14) : QString();
    const QString flagsCh = thePostLine.size() > 15 ? thePostLine.at(15) : QString();
    const bool lineIsDir = !tocall.isEmpty();
    const bool lineIsDisk = flagsCh.size() == 1 && ((flagsCh.at(0).unicode() - 48) & 8) != 0;
    QString postString =  "skedfreq=" + thePostLine.at(0) + "&rxfreq=" + thePostLine.at(1) + "&rpol=" + thePostLine.at(2) + "&dt="  +  thePostLine.at(4) + "&dB="  + thePostLine.at(5) + "&msgtype="  +  thePostLine.at(7) + (lineIsDir ? "&tocall=" + tocall : QString()) + "&callsign="  +  thePostLine.at(8) + "&grid="  +  thePostLine.at(9) + "&mode="  +  thePostLine.at(6) + "&utcdatetime="  +  utcdatetimeUTCString + "&spotter="  +  thePostLine.at(12) + "&spottergrid=" +  thePostLine.at(10)  + "&band=" + QString::number(int(datcom_.fcenter)) + "&txpol=" + thePostLine.at(13);
    const QString flagsParam = flagsCh.isEmpty()
        ? QString() : "&flags=" + QString::fromLatin1(QUrl::toPercentEncoding(flagsCh));

    // W3SZ and DL3WDG develop the w3sz site together and feed it disk
    // replays -- the standing exception the old whitelist encoded. It
    // survives the uniform policy for DISK CQ-type rows only: directed
    // messages stay N6NU-only for everyone (w3sz is live-CQ-only by the
    // 2026-08-12 ownership model), and both operators' disk uploads to N6NU
    // are flagged and hidden like anyone else's.
    const QString myUp = m_myCall.trimmed().toUpper();
    const bool w3szDiskOp = (myUp == "W3SZ" || myUp == "DL3WDG");
    for (const QString &u : urls) {
      if ((lineIsDir || lineIsDisk) && !u.contains("livecq.n6nu.org")) {
        if (!(lineIsDisk && !lineIsDir && w3szDiskOp)) continue;
      }
      // w3sz.com gets the program name chosen in Settings: QMAP with the QMAP
      // User-Agent, exactly as before the re-branding, or WSMAP.
      const bool legacyW3sz = u.contains("w3sz.com") && m_livecqW3szAppId == "QMAP";
      // flags is N6NU-protocol only; other destinations get requests
      // byte-identical to the pre-flag client.
      QByteArray postByteArray = (postString
          + (u.contains("livecq.n6nu.org") ? flagsParam : QString())
          + "&apptype=" + (legacyW3sz ? "QMAP" : "WSMAP")).toUtf8();
      QNetworkRequest request{QUrl{u}};
      request.setRawHeader("User-Agent", legacyW3sz ? "QMAP v0.5" : "WSMAP v0.5");
      request.setRawHeader("X-Custom-User-Agent", legacyW3sz ? "QMAP v0.5" : "WSMAP v0.5");
      request.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
      request.setTransferTimeout(15000);   // don't let a hung server queue requests
#endif
      QNetworkReply *reply = m_livecqNAM->post(request,postByteArray);
      QObject::connect(reply, &QNetworkReply::finished, this, &MainWindow::handleReply);
    }
  }
}

void MainWindow::handleReply()
{
  try {
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    if (reply->error() == QNetworkReply::NoError) {
		qDebug() << reply->readAll();
    } else {
		qDebug() << reply->errorString();
    }
    reply->deleteLater();
  }
    catch (const std::exception& e) {
        // Handle standard C++ exceptions
        QMessageBox::critical(this, "Exception", "Exception at line 1188 MainWindow::handleReply " + QString::fromStdString(e.what()));   
    }
    catch (...) {
        // Handle any other type of exception
        QMessageBox::critical(this, "Exception", "Unknown Exception at line 1193 MainWindow::handleReply");   
    }
}


//------------------------------------------------------------- //guiUpdate()
void MainWindow::guiUpdate()
{
  int khsym=0;

  QStringList cqliveText;  //liveCQ

  qint64 ms = QDateTime::currentMSecsSinceEpoch() % 86400000;
  int nsec=ms/1000;

  if(m_monitoring) {
    if(m_saveAll or m_saveDecoded) {
      ui->monitorButton->setStyleSheet(m_pbmonitor_style2);
    } else {
      ui->monitorButton->setStyleSheet(m_pbmonitor_style);
    }
  } else {
    ui->monitorButton->setStyleSheet("");
  }

  m_wide_graph_window->updateFreqLabel();

  // Reverse channel: mirror WSJT-X's DX Call selection. When the operator
  // changes or clears DX Call over there, follow it here -- the red decode
  // label on the waterfall and the light-red rows in the decoded text both
  // track the new selection. Edge-detected via dxcall_seq; mirroring never
  // relays a click back, so there is no feedback loop.
  if (ui->actionSync_WSJTX->isChecked() && mem_qmap.isAttached()) {
    QmapReverse rev;
    mem_qmap.lock();
    std::memcpy(&rev, static_cast<char*>(mem_qmap.data()) + sizeof(decodes_),
                sizeof(rev));
    mem_qmap.unlock();
    if (rev.dxcall_seq != m_lastRxDxcallSeq) {
      m_lastRxDxcallSeq = rev.dxcall_seq;
      rev.dxcall[sizeof(rev.dxcall)-1] = 0;
      const QString rxCall = QString::fromUtf8(rev.dxcall).trimmed();
      if (rxCall != DXcall) {
        DXcall = rxCall;
        if (m_wide_graph_window) m_wide_graph_window->updateActiveCallsign(DXcall);
        refreshDxHighlight();
      }
    }
  }

  if(m_startAnother and !m_bDiskDatBusy) {
    m_startAnother=false;
    on_actionOpen_next_in_directory_triggered();
  }

  QString t1;
  if(decodes_.ndecodes > m_fetched) {
    doLiveCQ = true;
    while(m_fetched<decodes_.ndecodes) {
      QString t=QString::fromLatin1(decodes_.result[m_fetched]);
      QString t2=QString::fromLatin1(decodes2_.result2[m_fetched]);
      if(m_UTC0!="" and m_UTC0!=t.left(4)) {
        t1="-";
        ui->decodedTextBrowser->append(t1.repeated(60));
        m_nline++;
        QTextCursor cursor(ui->decodedTextBrowser->document()->findBlockByLineNumber(m_nline-1));
        QTextBlockFormat f = cursor.blockFormat();
        f.setBackground(QBrush(Qt::white));
        cursor.setBlockFormat(f);
      }
      m_UTC0=t.left(4);
      t=t.trimmed();
      t2=t2.trimmed();            //liveCQ
      QString t3 = t + " " + t2;  //liveCQ
      cqliveText.append(t3);      //liveCQ
      ui->decodedTextBrowser->append(t);
      {
        QString sender, grid, mode;
        double freq_khz = -1.0, fsked_khz = -1.0;
        int even_period = -1;
        bool is_cq = false;
        if (parseDecodeLine(t, sender, freq_khz, fsked_khz, grid, mode,
                            even_period, is_cq)) {
          is_active = (DXcall == sender);
          m_wide_graph_window->addDecodeLabel(freq_khz, sender, is_cq, grid, is_active, mode, even_period, fsked_khz);
          m_bandmap_window->addEntry(freq_khz, sender, is_cq, grid, is_active, mode, even_period, fsked_khz);
          // PSK Reporter gets the sked dial (the station at 1500 Hz), as WS sent it before
          const QStringList cols = t.split(QRegularExpression {"\\s+"}, SkipEmptyParts);
          if (fsked_khz >= 0.0 && cols.size() > 4)
            spotToPSKReporter(sender, grid, qint64(std::floor(datcom_.fcenter)) * 1000000 + qRound64(fsked_khz * 1000.0),
                              "Q65", cols[4].toInt(), cols[0], is_cq);
        }
      }
      if (!qmap_runtime::g_decode_log_path.isEmpty()) {
        QFile log(qmap_runtime::g_decode_log_path);
        if (log.open(QIODevice::Append | QIODevice::Text)) {
          QTextStream s(&log);
          s << t << "\n";
        }
      }
      m_fetched++;
      m_nline++;
      QTextCursor cursor(ui->decodedTextBrowser->document()->findBlockByLineNumber(m_nline-1));
      QTextBlockFormat f = cursor.blockFormat();
      f.setBackground(decodeRowBackground(t));
      cursor.setBlockFormat(f);
    }
  }
  if(doLiveCQ) {
    if(cqliveText.size() != 0) {
      CreateLiveCQ(cqliveText);  //liveCQ
      doLiveCQ = false;
    }
  }

  t1="";
  t1=t1.asprintf("%.3f",datcom_.fcenter);
  ui->labFreq->setText(t1);

  if(nsec != m_sec0) {                                     //Once per second

    static int n60z=99;
    m_n60=nsec%60;

// See if WSJT-X is transmitting
    int itest[5];
    mem_qmap.lock();
    memcpy(&itest, (char*)ipc_wsjtx, 20);
    mem_qmap.unlock();
    if(itest[4]>0) {
      m_WSJTX_TRperiod=itest[4];
      m_bWTransmitting=true;
      if(m_WSJTX_TRperiod==30 and m_n60<30) m_nTx30a++;
      if(m_WSJTX_TRperiod==30 and m_n60>=30) m_nTx30b++;
      if(m_WSJTX_TRperiod==60) m_nTx60++;
    } else {
      m_bWTransmitting=false;
    }

    if((m_n60<n60z) and !m_diskData) {
      m_nTx30a=0;
      m_nTx30b=0;
      m_nTx60=0;
    }
    n60z=m_n60;

    if(m_pctZap>30.0) {
      lab2->setStyleSheet("QLabel{background-color: #ff0000}");
    } else {
      lab2->setStyleSheet("");
    }

    if(m_monitoring and !m_bWTransmitting) {
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
      if(khsym==m_hsym0) {
        t="Nil";
        lab1->setStyleSheet("QLabel{background-color: #ffc0cb}");
      }
      lab1->setText("Receiving " + t);
    } else if(m_bWTransmitting) {
      lab1->setStyleSheet("QLabel{background-color: #ffff00}");  //Yellow
      lab1->setText("WS Transmitting");
    } else if(!m_diskData) {
      lab1->setStyleSheet("");
      lab1->setText("");
    }

    datcom_.mousefqso=m_wide_graph_window->QSOfreq();
    QDateTime t = QDateTime::currentDateTimeUtc();
    m_astro_window->astroUpdate(t, m_myGrid, m_azelDir, m_xavg);
    QString utc = t.date().toString(" yyyy MMM dd \n") + t.time().toString();
    ui->labUTC->setText(utc);
    m_hsym0=khsym;
    m_sec0=nsec;
    if(m_n60==0) m_dop00=datcom_.ndop00;
    if(m_n60==58) m_dop58=datcom_.ndop00;
  }
}

void MainWindow::on_actionQ65A_triggered()
{
  m_modeQ65=1;
   ui->actionAlso_Q65_30x->setText("Also Q65-30A");
  lab3->setStyleSheet("QLabel{background-color: #ffb266}");
  lab3->setText("Q65-60A");
}

void MainWindow::on_actionQ65B_triggered()
{
  m_modeQ65=2;
  ui->actionAlso_Q65_30x->setText("Also Q65-30A");
  lab3->setStyleSheet("QLabel{background-color: #b2ff66}");
  lab3->setText("Q65-60B");
}

void MainWindow::on_actionQ65C_triggered()
{
  m_modeQ65=3;
  ui->actionAlso_Q65_30x->setText("Also Q65-30B");
  lab3->setStyleSheet("QLabel{background-color: #66ffff}");
  lab3->setText("Q65-60C");
}

void MainWindow::on_actionQ65D_triggered()
{
  m_modeQ65=4;
  ui->actionAlso_Q65_30x->setText("Also Q65-30C");
  lab3->setStyleSheet("QLabel{background-color: #d9b3ff}");
  lab3->setText("Q65-60D");
}

void MainWindow::on_actionQ65E_triggered()
{
  m_modeQ65=5;
  ui->actionAlso_Q65_30x->setText("Also Q65-30D");
  lab3->setStyleSheet("QLabel{background-color: #ff66ff}");
  lab3->setText("Q65-60E");
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

bool MainWindow::isGrid4(QString g)
{
  if(g.length()!=4) return false;
  if(g.mid(0,1)<'A' or g.mid(0,1)>'R') return false;
  if(g.mid(1,1)<'A' or g.mid(1,1)>'R') return false;
  if(g.mid(2,1)<'0' or g.mid(2,1)>'9') return false;
  if(g.mid(3,1)<'0' or g.mid(3,1)>'9') return false;
  return true;
}

void MainWindow::on_actionQuick_Start_Guide_to_Q65_triggered()
{
  QDesktopServices::openUrl (QUrl {"https://wsjt.sourceforge.io/Q65_Quick_Start.pdf"});
}

void MainWindow::on_actionQuick_Start_Guide_to_WSJT_X_2_7_and_QMAP_triggered()
{
  QDesktopServices::openUrl (QUrl {"https://wsjt.sourceforge.io/Quick_Start_WSJT-X_2.7_QMAP.pdf"});
}

void MainWindow::on_actionAlso_Q65_30x_toggled(bool b)
{
  m_bAlso30=b;
}


void MainWindow::on_sbMaxDrift_valueChanged(int n)
{
  if(n==0) ui->sbMaxDrift->setStyleSheet("");
  if(n==5) ui->sbMaxDrift->setStyleSheet("QSpinBox { background-color: #ffff82; }");
  if(n>=10) ui->sbMaxDrift->setStyleSheet("QSpinBox { background-color: #ffff00; }");
}

void MainWindow::on_actionExport_wav_file_at_fQSO_triggered()
{
  datcom_.newdat=0;
  datcom_.nagain=2;
  decode();
}

void MainWindow::on_actionExport_wav_file_at_fQSO_30a_triggered()
{
  datcom_.newdat=0;
  datcom_.nagain=3;
  decode();
}

void MainWindow::on_actionExport_wav_file_at_fQSO_30b_triggered()
{
  datcom_.newdat=0;
  datcom_.nagain=4;
  decode();
}

void MainWindow::on_actionFadd_controls_triggered()
{
  if (ui->actionFadd_controls->isChecked()) {
    ui->fAddComboBox->setVisible(true);
    ui->fAdd_label->setVisible(true);
    ui->pbSet->setVisible(true);
    ui->pbAdd->setVisible(true);
  } else {
    ui->fAddComboBox->setVisible(false);
    ui->fAdd_label->setVisible(false);
    ui->pbSet->setVisible(false);
    ui->pbAdd->setVisible(false);
  }
}

void MainWindow::on_fAddComboBox_activated()
{
  if (ui->fAddComboBox->isVisible() && ui->fAddComboBox->currentText() != "") {
    m_fAdd=ui->fAddComboBox->currentText().toDouble();
    soundInThread.setFadd(m_fAdd);
    ui->decodedTextBrowser->append("Setting Fadd to " + QString::number(m_fAdd) + " MHz");
  }
}

void MainWindow::on_pbSet_clicked()
{
  m_fAdd=ui->fAddComboBox->currentText().toDouble();
  soundInThread.setFadd(m_fAdd);
  ui->decodedTextBrowser->append("Setting Fadd to " + QString::number(m_fAdd) + " MHz");
}

void MainWindow::on_pbAdd_clicked()
{
  m_fAdd=ui->fAddComboBox->currentText().toDouble();
  if (ui->fAddComboBox->currentText() != "") {
    QFile g("fadd.txt");
    if(g.open(QIODevice::Text | QIODevice::Append)) {
      QString addedEntry = (ui->fAddComboBox->currentText());
      QTextStream out(&g);
      out << addedEntry <<
#if QT_VERSION < QT_VERSION_CHECK(5, 15, 0)
          endl
#else
          Qt::endl
#endif
          ;
      g.close();
      if (ui->fAddComboBox->findText(addedEntry) < 0) ui->fAddComboBox->addItem (QString::number(m_fAdd));
      ui->decodedTextBrowser->append("Adding " + QString::number(m_fAdd) + " to file fadd.txt");
    }
  }
}
