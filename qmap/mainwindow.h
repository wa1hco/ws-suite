#ifndef MAINWINDOW_H
#define MAINWINDOW_H
#include <QtGui>
#include <QtWidgets>
#include <QPointer>
#include <QScopedPointer>
#include <QLabel>
#include <QDateTime>
#include <QElapsedTimer>
#include <QHash>
#include <memory>
#include "getfile.h"
#include "soundin.h"
#include "signalmeter.h"
#include "commons.h"
#include "sleep.h"
#include <QtConcurrent/QtConcurrent>

#define NFFT 32768
#define NSMAX 5760000

//--------------------------------------------------------------- MainWindow
namespace Ui {
  class MainWindow;
}

class QTimer;
class Astro;
class WideGraph;
class BandMap;
class QNetworkAccessManager;
class PSKReporter;

class MainWindow : public QMainWindow
{
  Q_OBJECT

public:
  // settings_filename empty = default applicationDirPath/qmap.ini.
  // Non-empty = explicit INI path (multi-instance via --config <path>).
  // Same path is shared with Astro and WideGraph so all settings live
  // in the one INI.
  explicit MainWindow(QString const& settings_filename = {},
                      QWidget *parent = 0);
  ~MainWindow();
  bool m_network;

public slots:
  void showSoundInError(const QString& errorMsg);
  void showStatusMessage(const QString& statusMsg);
  void dataSink(int k);
  void diskDat(int iret);
  void decoderFinished();
  void freezeDecode(int n);
  void guiUpdate();

private:
  virtual void keyPressEvent (QKeyEvent *) override;
  virtual bool eventFilter (QObject *, QEvent *) override;
  virtual void closeEvent (QCloseEvent *) override;

private slots:
  void on_monitorButton_clicked();
  void on_actionExit_triggered();
  void on_actionAbout_triggered();
  void on_actionLinrad_triggered();
  void on_actionCuteSDR_triggered();
  void on_tolSpinBox_valueChanged(int arg1);
  void on_actionAstro_Data_triggered();
  void on_actionWide_Waterfall_triggered();
  void on_actionBand_Map_triggered();
  void on_actionOpen_QMAP_data_directory_triggered();
  void on_actionOpen_triggered();
  void on_actionOpen_next_in_directory_triggered();
  void on_actionDecode_remaining_files_in_directory_triggered();
  void on_actionDelete_all_iq_files_in_SaveDir_triggered();
  void on_actionNone_triggered();
  void on_actionSave_all_triggered();
  void on_DecodeButton_clicked();
  void decode();
  void decodeBusy(bool b);
  void on_EraseButton_clicked();
  void bumpDF(int n);
  void handleCallsignClick(const QString& call, double freq_khz, const QString& grid,
                           const QString& mode, int even_period, double fsked_khz,
                           bool start_qso);
  void handleDecodeLineClick(const QString& line, bool start_qso);
  void on_actionSettings_triggered();
  void on_NBcheckBox_toggled(bool checked);
  void on_NBslider_valueChanged(int value);
  void on_actionAFMHot_triggered();
  void on_actionBlue_triggered();
  void on_actionQ65A_triggered();
  void on_actionQ65B_triggered();
  void on_actionQ65C_triggered();
  void on_actionQ65D_triggered();
  void on_actionQ65E_triggered();
  void on_actionQuick_Start_Guide_to_Q65_triggered();
  void on_actionQuick_Start_Guide_to_WSJT_X_2_7_and_QMAP_triggered();
  void on_actionAlso_Q65_30x_toggled(bool b);
  void on_sbMaxDrift_valueChanged(int arg1);
  void on_actionSave_decoded_triggered();
  void on_actionExport_wav_file_at_fQSO_triggered();
  void on_actionExport_wav_file_at_fQSO_30a_triggered();
  void on_actionExport_wav_file_at_fQSO_30b_triggered();
  void on_actionFadd_controls_triggered();
  void on_fAddComboBox_activated();
  void on_pbSet_clicked();
  void on_pbAdd_clicked();
  void handleReply(); //liveCQ
  void mousePressEvent(QMouseEvent *event) override;

private:
  bool parseDecodeLine(const QString& t, QString& sender, double& freq_khz,
                       double& fsked_khz, QString& grid, QString& mode,
                       int& even_period, bool& is_cq);
  void updateWindowTitle();
  void publishConfigNameToMeta();
  QBrush decodeRowBackground(const QString& t) const;
  void refreshDxHighlight();

  Ui::MainWindow *ui;
  QString m_appDir;
  QString m_dataDir;    // writable per-user dir (AppData/Local/QMAP): inis at root, instance-<ID>/ work files
  QString m_settings_filename;
  QScopedPointer<Astro> m_astro_window;
  QScopedPointer<WideGraph> m_wide_graph_window;
  QScopedPointer<BandMap> m_bandmap_window;
  QPointer<QTimer> m_gui_timer;
  qint32  m_waterfallAvg;
  qint32  m_DF;
  qint32  m_tol;
  qint32  m_astroFont;
  bool    m_astroOpen=true;      //Astro window open/closed state, persisted
  qint32  m_fCal;
  qint32  m_sec0;
  qint32  m_nutc0;
  qint32  m_nrx;
  qint32  m_hsym0;
  qint32  m_paInDevice;
  qint32  m_udpPort;
  qint32  m_NBslider;
  qint32  m_TRperiod;
  qint32  m_modeQ65;
  qint32  m_dB;
  qint32  m_sampleRateModeOrZero=0;    // 0 = Auto (Linrad TCP detect); else forced rate Hz
  qint32  m_linradTcpPort=49812;       // Linrad parameter-server TCP port (auto-detect handshake)
  qint32  m_instanceId=1;              // 1-4, clamped on read; default 1
  QString m_configName="";             // free-text label, e.g. "1296 CQ"
  qint32  m_fetched=0;
  qint32  m_lastRxDxcallSeq=0;
  qint32  m_hsymStop=390;              // 390*0.15 = 58.5 s
  qint32  m_nTx30a=0;
  qint32  m_nTx30b=0;
  qint32  m_nTx60=0;
  qint32  m_nDoubleClicked=0;
  qint32  m_nline=0;
  qint32  m_WSJTX_TRperiod=0;
  qint32  m_dop00=0;
  qint32  m_dop58=0;
  qint32  m_n60;

  double  m_fAdd;
  double  m_xavg;

  bool    m_monitoring;
  bool    m_diskData;
  bool    m_loopall;
  bool    m_decoderBusy=false;
  bool    m_restart;
  bool    m_startAnother;
  bool    m_saveAll;
  bool    m_saveDecoded;
  bool    m_NB;
  bool    m_decode_called=false;
  bool    m_bAlso30=true;
  bool    m_bDiskDatBusy=false;
  bool    m_bWTransmitting=false;
  int     m_lastPushedClickSeq=-1;   // click_seq as of the previous periodic push to WSJT-X
  bool    m_bDecodeAgain=false;
  
  // LiveCQ destinations are independent checkboxes; each CQ decode fans
  // out to every enabled URL. Custom is inert while m_otherUrl is empty.
  bool    m_livecqEnabled = true;   //liveCQ master switch
  bool    m_livecqOfficial = false; //liveCQ -> w3sz.com
  bool    m_livecqN6NU = true;      //liveCQ -> livecq.n6nu.org
  QString m_livecqW3szAppId {"QMAP"};  //liveCQ program name reported to w3sz.com
  bool    m_livecqCustom = false;   //liveCQ -> m_otherUrl
  QString m_otherUrl="";    //liveCQ
  QString w3szUrlAddr="https://w3sz.com/livecq_update.php"; //liveCQ
  QString n6nuUrlAddr="https://livecq.n6nu.org/api/livecq_update"; //liveCQ
  QNetworkAccessManager* m_livecqNAM = nullptr; //liveCQ (created on first upload)
  bool    m_spotPSK = false;        // spot own decodes to PSK Reporter (off until enabled)
  bool    m_spotPSKTcpIp = false;   // ... over TCP/IP instead of UDP
  std::unique_ptr<PSKReporter> m_pskReporter;   // only while spotting is enabled

  float   m_pctZap;

  int     m_myCallColor;

  QRect   m_wideGraphGeom;

  QLabel* lab1;                            // labels in status bar
  QLabel* lab2;                            // labels in status bar
  QLabel* lab3;                            // labels in status bar
  QLabel* lab4;
  QLabel* lab5;
  QLabel* lab6;
  QLabel* lab7;                   //Why still needed?
  QLabel* lab8;

  QMessageBox msgBox0;

  QFutureWatcher<void> watcher3;     //For decoder

  // Stopwatch around QtConcurrent::run(q65c_) — measures the actual
  // Fortran decoder kernel time (FFT + Q65 inner). Wall-clock from
  // decode() to decoderFinished(). The harness reads this via
  // "# decoder_ms=<n>" lines in the --decode-log file. Useful
  // separately from "did this decode at all" because at 256 kHz the
  // wider FFT / longer kernel can approach the TR-30 period budget.
  QElapsedTimer m_decoderTimer;
  bool          m_harnessQuitScheduled = false;
  // ihsym value that triggered the current decoder run (set in
  // decode(), read in decoderFinished). The harness quit gate uses
  // it to decide whether to wait for further cycles in the TR
  // period or call it done. Three decoder triggers per TR-60 cycle:
  // 130 (=19.5 s), 330 (=49.5 s), 390 (=58.5 s, m_hsymStop). On a
  // 130/330 cycle that decoded nothing, we wait for the next one;
  // on the 390 cycle we always quit (last chance).
  int           m_lastDecodeTriggerNhsym = 0;

  QString m_path;
  QString m_pbdecoding_style1;
  QString m_pbmonitor_style;
  QString m_pbmonitor_style2;
  QString m_pbAutoOn_style;
  QString m_myCall;
  QString m_myGrid;
  QString m_hisCall;
  QString m_hisGrid;
  QString m_saveDir;
  QString m_azelDir;
  QString m_linradHost="127.0.0.1";    // Bridge host for Linrad UDP+TCP
  QString m_palette;
  QString m_dateTime;
  QString m_mode;
  QString m_UTC0="";
  QString m_revision;
  QString m_saveFileName;

  QDateTime m_dateTimeSeqStart;        //Nominal start time of Rx sequence about to be decoded
  QHash<QString,bool> m_worked;
  SignalMeter *xSignalMeter;
  SoundInThread soundInThread;             //Instantiate the audio threads
  bool doLiveCQ = true;  //liveCQ
  QFile *cqlfi;          //liveCQ

  //---------------------------------------------------- private functions
  void readSettings();
  void writeSettings();
  void createStatusBar();
  void updateStatusBar();
  void msgBox(QString t);
  bool isGrid4(QString g);
  void CreateLiveCQ(QStringList cqliveText);           //liveCQ
  void sendLiveCQData(QList<QStringList> decodeList);  //liveCQ
  void updatePSKReporter();
  void spotToPSKReporter(QString const& sender, QString const& grid, qint64 rf_hz,
                         QString const& mode, int snr, QString const& hhmmss, bool is_cq);
  bool testCall(QString callsign); //liveCQ
};

extern void getfile(QString fname, bool xpol, int idInt);
extern void save_iq(QString fname);
extern int killbyname(const char* progName);

extern "C" {
//----------------------------------------------------- C and Fortran routines
  void symspec_(int* k, int* ndiskdat, int* nb, int* m_NBslider, int* nfsample,
                float* px, float s[], int* nkhz, int* nhsym,
                int* nzap, float* slimit, uchar lstrong[]);

  void astrosub00_ (int* nyear, int* month, int* nday, double* uth, int* nfreq,
                    const char* mygrid, int* ndop00, int len1);

  void q65c_();

  void all_done_();

  void zaptx_(float d4[], int* k0, int* k);

  void save_qm_(const char* fname, const char* prog_id, const char* mycall, const char* mygrid,
                float d4[], int* ntx30a, int* ntx30b, double* fcenter, int* nutc,
                int* dop00, int* dop58, int len1, int len2, int len3, int len4);

  void read_qm_(const char* fname, int* iret, int len);

  }

#endif // MAINWINDOW_H
