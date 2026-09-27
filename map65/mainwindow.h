#ifndef MAINWINDOW_H
#define MAINWINDOW_H
#include <QtGui>
#include <QtWidgets>
#include <QPointer>
#include <QScopedPointer>
#include <QLabel>
#include <QDateTime>
#include <QHash>
#include <QProcess>
#include "soundin.h"
#include "soundout.h"
#include "signalmeter.h"
#include "commons.h"
#include "sleep.h"
#include <QtConcurrent/QtConcurrent>
#include <QByteArray>
#include <QSharedMemory>
#include "qmap_shared.h"

#define NSMAX 5760000


struct StdoutChannel;

//--------------------------------------------------------------- MainWindow
namespace Ui {
  class MainWindow;
}

class QTimer;
class Astro;
class BandMap;
class Messages;
class WideGraph;
extern QByteArray g_TxTuneGeometry;

class MainWindow : public QMainWindow
{
  Q_OBJECT

public:
  explicit MainWindow(QWidget *parent = 0);
  ~MainWindow();
  

private:
  Ui::MainWindow *ui;

  struct DecoderContext;
  DecoderContext* decoderCtx;

public:    
  float* getDd() const;
  std::thread stdoutReaderThread;
  std::atomic<bool> stdoutReaderStop {false};
  void writeSettings();

  bool m_network;
  int m_pttPortNumber = 0;
  
  QString m_pttPath;
  QString m_appDir;
  QString m_dataDir;    // writable per-user dir (AppData/Local/MAP65): ini, CALL3.TXT, decoder output
  QString m_settings_filename;
  qint32  m_nDevIn;
  qint32  m_nDevOut;
  qint32  m_idInt;
  qint32  m_astroFont;
  qint32  m_timeout;
  qint32  m_dPhi;
  qint32  m_fCal;
  qint32  m_paInDevice;
  qint32  m_paOutDevice;
  qint32  m_udpPort;
  qint32  m_NBslider;
  qint32  m_mult570;
  qint32  m_mult570Tx;
  qint32  m_dB;
  
  double  m_fAdd;
  //    double  m_IQamp;
  //    double  m_IQphase;
  double  m_cal570;
  double  m_TxOffset;
  
  bool    m_xpol;
  bool    m_xpolx;
  int     m_fs96000;
  bool    m_IQswap;
  bool    m_initIQplus;
  bool    m_bIQxt;
  // LiveCQ destinations are independent checkboxes; each CQ decode fans
  // out to every enabled URL (Messages::sendLiveCQData re-reads these
  // from the ini per decode pass). Custom is inert while m_otherUrl is
  // empty.
  bool    m_livecqEnabled = true;   //liveCQ master switch
  bool    m_livecqOfficial = false; //liveCQ -> w3sz.com
  bool    m_livecqN6NU = true;      //liveCQ -> livecq.n6nu.org
  bool    m_livecqCustom = false;   //liveCQ -> m_otherUrl
  QString m_livecqW3szAppId {"MAP65"};  //liveCQ program name reported to w3sz.com
  bool    m_spot_to_psk_reporter;
  bool    m_psk_reporter_tcpip = false;
  
  QString m_myCall;
  QString m_myGrid;
  QString m_saveDir;
  QString m_azelDir;
  // User-selectable CALL3.TXT location (Settings -> CALL3.TXT file).
  // Empty = default: CALL3.TXT in the writable data dir.
  QString m_call3PathUser;
  // True from the moment decode() raises decoder_ready until the decoder
  // reports <EarlyFinished> or <DecodeFinished>. Precise, unlike
  // m_decoderBusy, which stays set across BOTH passes of a period (only
  // <DecodeFinished> lowers it) and so cannot tell "a decode is in flight".
  bool m_decoderRunning = false;
  // Keep last period in RAM. m_holdSnapshot is armed by freezeDecode for exactly
  // one decode() call: that decode keeps the decoder-side snapshot instead of
  // refreshing it from the live buffers, so the click re-decodes the held
  // (last decoded) period. m_snapshotValid guards the very first decode:
  // until one live->snapshot copy has happened the snapshot is zeros and
  // holding it would decode nothing.
  bool m_holdSnapshot = false;
  bool m_snapshotValid = false;
  // Hand the resolved CALL3.TXT path to the Fortran decoder. Public because
  // DevSetup calls it when the operator changes the path in Settings.
  void pushCall3PathToDecoder() const;
  QString m_dxccPfx;
  QString m_colors;
  QString m_colorPreset;  // "Classic"/"Black on white"/"White on Blue"/"Custom"
  QString m_editorCommand;
  QString m_otherUrl;

public slots:
  void showSoundInError(const QString& errorMsg);
  void showStatusMessage(const QString& statusMsg);
  void dataSink(int k);
  void diskDat();
  // Past-period decode helpers
  void disarmPastPeriod();
  void decodePastPeriod();

  void diskWriteFinished();
  void freezeDecode(int n);
  void editor_error();
  void guiUpdate();
  void doubleClickOnCall(QString rawLine, bool ctrl, bool isDoubleClick);
  void doubleClickOnMessages(QString t2, bool ctrl, bool isDoubleClick);
  void handleCallsignClick(const QString& call, double freq_khz, bool is_jt65, const QString& grid, bool start_qso);
  void handleBandMapCallsignClick(const QString& call, bool isDoubleClick);
  void startDecoder();
  
private slots:
  void onDiskDecodeFinished();
  // Past-period decode (View -> Show/Decode past decodes on Wide Graph)
  void on_actionPast_periods_on_Wide_Graph_toggled(bool b);
  void onPastPeriodClicked(QString tag);
  void onRunM65Finished();
  void on_tx1_editingFinished();
  void on_tx2_editingFinished();
  void on_tx3_editingFinished();
  void on_tx4_editingFinished();
  void on_tx5_editingFinished();
  void on_tx6_editingFinished();
  void on_actionDeviceSetup_triggered();
  void on_monitorButton_clicked();
  void on_actionExit_triggered();
  void on_actionAbout_triggered();
  void on_actionLinrad_triggered();
  void on_actionCuteSDR_triggered();
  void on_autoButton_clicked();
  void on_stopTxButton_clicked();
  void on_tolSpinBox_valueChanged(int arg1);
  void on_actionAstro_Data_triggered();
  void on_stopButton_clicked();
  void on_actionRelease_Notes_triggered();
  void on_actionOnline_Users_Guide_triggered();
  void on_actionQSG_Q65_triggered();
  void on_actionQSG_MAP65_v3_triggered();
  void on_actionQ65_Sensitivity_in_MAP65_3_0_triggered();
  void on_actionWide_Waterfall_triggered();
  void on_actionBand_Map_triggered();
  void on_actionMessages_triggered();
  void on_actionOpen_MAP65_data_directory_triggered();
  void on_actionOpen_triggered();
  void on_actionOpen_next_in_directory_triggered();
  void on_actionDecode_remaining_files_in_directory_triggered();
  void on_actionDelete_all_tf2_files_in_SaveDir_triggered();
  void on_actionErase_Band_Map_and_Messages_triggered();
  void on_actionFind_Delta_Phi_triggered();
  void on_actionF4_sets_Tx6_triggered();
  void on_actionOnly_EME_calls_triggered();
  void on_actionNo_shorthands_if_Tx1_triggered();
  void on_actionNo_Deep_Search_triggered();
  void on_actionNormal_Deep_Search_triggered();
  void on_actionAggressive_Deep_Search_triggered();
  void on_actionQ65_Fast_triggered();
  void on_actionQ65_Normal_triggered();
  void on_actionQ65_Deep_triggered();
  void on_actionNone_triggered();
  void on_actionSave_all_triggered();
  void on_actionKeyboard_shortcuts_triggered();
  void on_actionSpecial_mouse_commands_triggered();
  void on_actionAvailable_suffixes_and_add_on_prefixes_triggered();
  void on_DecodeButton_clicked();
  void decode();
  void decodeBusy(bool b);
  void on_EraseButton_clicked();
  void on_txb1_clicked();
  void on_txFirstCheckBox_stateChanged(int arg1);
  void set_ntx(int n);
  void on_txb2_clicked();
  void on_txb3_clicked();
  void on_txb4_clicked();
  void on_txb5_clicked();
  void on_txb6_clicked();
  void on_lookupButton_clicked();
  void on_addButton_clicked();
  void on_setTxFreqButton_clicked();
  void on_dxCallEntry_textChanged(const QString &arg1);
  void on_dxGridEntry_textChanged(const QString &arg1);
  void selectCall2(bool ctrl, bool isDoubleClick);
  void on_genStdMsgsPushButton_clicked();
  void bumpDF(int n);
  void on_logQSOButton_clicked();
  void on_actionErase_map65_rx_log_triggered();
  void on_actionErase_map65_tx_log_triggered();
  void on_NBcheckBox_toggled(bool checked);
  void on_actionJT65A_triggered();
  void on_actionJT65B_triggered();
  void on_actionJT65C_triggered();
  void on_NBslider_valueChanged(int value);
  void on_actionAdjust_IQ_Calibration_triggered();
  void on_actionApply_IQ_Calibration_triggered();
  void on_actionAFMHot_triggered();
  void on_actionBlue_triggered();
  void on_actionFUNcube_Dongle_triggered();
  void on_actionEdit_wsjt_log_triggered();
  void on_actionTx_Tune_triggered();
  void on_actionQ65A_triggered();
  void on_actionQ65B_triggered();
  void on_actionNoJT65_triggered();
  void on_actionNoQ65_triggered();
  void on_actionQ65C_triggered();
  void on_actionQ65D_triggered();
  void on_actionQ65E_triggered();
  void on_pbTxMode_clicked();
  void onSampleRateChanged(int newRate);

private:
  virtual void keyPressEvent (QKeyEvent *) override;
  virtual bool eventFilter (QObject *, QEvent *) override;
  virtual void closeEvent (QCloseEvent *) override;

  QScopedPointer<Astro> m_astro_window;
  QScopedPointer<BandMap> m_band_map_window;
  QScopedPointer<Messages> m_messages_window;
  QScopedPointer<WideGraph> m_wide_graph_window;
  QPointer<QTimer> m_gui_timer;

  // Open/closed state of the four satellite windows, persisted across
  // sessions. Captured at the top of closeEvent(): the shutdown path
  // close()s the windows before ~MainWindow's writeSettings runs, so a
  // query at save time would always read false.
  bool m_astroOpen {true};
  bool m_wideGraphOpen {true};
  bool m_bandMapOpen {true};
  bool m_messagesOpen {true};
  bool m_windowOpenStatesCaptured {false};
  void captureWindowOpenStates();

  // Click-to-work relay to WSJT-X over the shared "mem_qmap"[_N] segment,
  // impersonating a QMAP client (WSJT-X needs no client-type awareness).
  // m_instanceId (1..4) selects the segment; m_syncWsjtx gates the relay.
  QSharedMemory m_memQmap;
  QmapShared*   m_qmapShm {nullptr};   // maps m_memQmap.data()
  // Item 4: full decode-stream forward to WSJT-X (separate mem_qmap_fwd segment).
  QSharedMemory m_memFwd;
  QmapFwd*      m_fwdShm {nullptr};     // maps m_memFwd.data()
  int   m_instanceId {1};
  bool  m_syncWsjtx  {false};          // relay clicks to WSJT-X when true
  bool  m_txViaWsjtx {false};          // double-click keys WSJT-X (true) vs MAP65 (false)
  // Rx mute while WSJT-X transmits (QMAP's model). WSJT-X publishes
  // nWTransmitting (= its T/R period) in the shared segment every tick while
  // it keys; MAP65 samples it once a second, blanks the samples captured
  // meanwhile, holds the waterfall, and skips the decode of a period that
  // was mostly Tx. "Continuous Waterfall" (WSJT-X menu) keeps Rx running for
  // test setups and RX-only stations beside another transmitter.
  bool  m_bWTransmitting {false};      // WSJT-X is keyed right now
  int   m_nWsjtxTxSec {0};             // seconds of WSJT-X Tx seen in this period
  bool  rxMutedByWsjtx() const;        // flag set AND the option allows muting
  void  zapWsjtxTx(int k0, int k);     // zero dd[] frames k0..k-1 (all channels)
  // JT65 shorthand (RO/RRR/73) carries NO callsign and is always forwarded
  // bare (Send-data gate only). This toggle additionally ATTACHES the sender
  // inferred from frequency proximity to the DX Call -- the form the
  // auto-sequencer acts on. Off by default: it is an inference, not a decode.
  // Also gates the inferred-callsign display on MAP65's own decoded text.
  bool  m_addCallToSh {false};
  // Every frequency (kHz within the MHz) each station has been decoded on this
  // session, newest last, deduped at 50 Hz and capped. A shorthand is matched
  // against the NEAREST of these: one last-write-wins value is not enough, as
  // the same station appears on more than one frequency (images/aliases).
  QHash<QString, QVector<double>> m_callFreqKHz;
  int   m_lastRxDxcallSeq {0};         // reverse-channel (WSJT-X->MAP65) edge detector
  // Write a click into the shared segment for WSJT-X to act on (gated by
  // m_syncWsjtx). start_qso routes Tx: only asserted to WSJT-X when the
  // Tx target is WSJT-X.
  void openIQFile(QString fname);
  void relayClickToWsjtx(const QString& call, double freq_khz,
                         const QString& grid, const QString& mode,
                         int even_period, bool start_qso);
  // Wire designator for click_mode: "JT65", or the Q65 "<period><letter>"
  // (e.g. "60A") from the current TR period + sub-mode. WSJT-X switches
  // mode to match. Empty if Q65 with no sub-mode selected.
  // Attribute a shorthand decode at shKHzRaw (kHz within the MHz, straight from
  // the decode line) to the selected DX Call by frequency proximity. Returns
  // the callsign, or empty when it can't be attributed.
  QString inferShorthandCall(double shKHzRaw) const;
  void    recordCallFreqs(const QStringList& words, double kHz);
  QString clickModeDesignator(bool is_jt65, const QString& call = QString()) const;
  int     clickEvenPeriod(const QString& call) const;
  QString call3Path() const;
  // Item 4: reformat one MAP65 decode line ("@" Q65 / "!" JT65) into a
  // WSJT-X DecodedText line, cache it by callsign, and (when it involves the
  // selected DX Call) append it to the mem_qmap_fwd ring for WSJT-X's
  // processMessage(). Gated by m_syncWsjtx.
  void forwardDecodeToWsjtx(const QString& rawLine);
  // Append one ready DecodedText line to the mem_qmap_fwd ring.
  void appendFwdLine(const QString& line);
  // Forward the cached decode line for `call` (called on a click, so the
  // clicked station's decode reaches WSJT-X immediately).
  void forwardCachedDecode(const QString& call);
  // callsign -> most recent reformatted DecodedText line (for click-forward).
  QHash<QString, QString> m_fwdLineByCall;
  // Decodes already forwarded to WSJT-X this session, HHMM|message -> minute
  // of day. display.f90 re-emits its whole aged list every decode cycle, so
  // without this every old line matching the DX Call went across again each
  // period (SM4GGC, 260908). Pruned when it grows; see fwdFirstTime().
  QHash<QString, int> m_fwdSentKeys;
  bool fwdFirstTime(const QString& hhmm, const QString& msg);

  qint64  m_msErase;
  qint32  m_waterfallAvg;
  qint32  m_DF;
  qint32  m_tol;
  qint32  m_QSOfreq0;
  qint32  m_ntx;
  qint32  m_txFreq;
  qint32  m_setftx;
  qint32  m_ndepth;
  qint32  m_q65depth;
  qint32  m_sec0;
  qint32  m_map65RxLog;
  qint32  m_nutc0;
  qint32  m_mode65;
  qint32  m_nrx;
  qint32  m_hsym0;
  qint32  m_adjustIQ;
  qint32  m_applyIQcal;
  qint32  m_nsum;
  qint32  m_nsave;
  qint32  m_TRperiod;
  qint32  m_modeJT65;
  qint32  m_modeQ65;
  qint32  m_RxState;

  double  m_xavg;

  bool    m_monitoring;
  bool    m_transmitting;
  bool    m_diskData;
  bool    m_loopall;
  bool    m_decoderBusy;
  bool    m_txFirst;
  bool    m_auto;
  bool    m_txMute;
  bool    m_restart;
  bool    m_call3Modified;
  bool    m_saveAll;
  // Past-period decode state: feature toggle, armed period tag/file, and
  // replay-in-progress flag (drives Monitor restore + button text).
  bool    m_pastPeriods=false;
  bool    m_pastReplayActive=false;
  bool    m_wasMonitoring=false;
  QString m_pastArmedTag;
  QString m_pastArmedPath;
  bool    m_onlyEME;
  bool    m_widebandDecode;
  bool    m_kb8rq;
  bool    m_NB;
  bool    m_pttErrorShown = false;

  QString m_path;
  QString m_pbdecoding_style1;
  QString m_pbmonitor_style;
  QString m_pbAutoOn_style;
  QString m_messagesText;
  QString m_bandmapText;
  // Side channel: callsign -> last-known decoded grid, populated from the
  // "&" bandmap line's trailing message-text field. BandMap's own visible
  // text stays freq+callsign only (fixed-width display format); this map
  // is what handleBandMapCallsignClick consults for a real grid instead
  // of always falling back to lookup().
  QHash<QString, QString> m_bandmapGrid;
  // Side channel: callsign -> is_jt65, populated alongside m_bandmapGrid
  // from the same "&" bandmap line's trailing cmode marker. Absence from
  // this map (rather than a false default) means "no mode info yet" --
  // checked via contains() before switching mode on a BandMap click.
  QHash<QString, bool> m_bandmapMode;
  // Side channel: callsign -> audio-offset freq (kHz), populated alongside
  // m_bandmapGrid so a BandMap click can relay the signal frequency to
  // WSJT-X (the BandMap click signal itself carries only the callsign).
  QHash<QString, double> m_bandmapFreq;
  // Side channel: callsign -> the decode's OWN sub-mode + UTC minute, taken
  // from its "@" wideband line (trailing cmode marker "#A".."#C"/":A"..":E",
  // and the HHMM field). A wideband decode list can hold stations heard at a
  // different sub-mode/period than the ones now selected in the menus, so a
  // click relays the decode's own values rather than MAP65's current state.
  struct DecodeMeta { char submode = 0; int minute = -1; };
  QHash<QString, DecodeMeta> m_decodeMeta;
  QString m_hisCall;
  QString m_hisGrid;
  QString m_palette;
  QString m_dateTime;
  QString m_mode;
  QString m_modeTx;

  float   m_gainx;
  float   m_gainy;
  float   m_phasex;
  float   m_phasey;
  float   m_pctZap;
  QRect   m_wideGraphGeom;

  int ddSize = 0;
  // --- Minute-boundary tracking for symspec ---
  int ntr0 = -1;          // previous ntr value (seconds into TR period)
  int m_TRperiod0 = 0;    // previous TR period (usually 60)
  int nhsym0 = 0;         // ihsym at start of minute (reset when newmin=1)

  QLabel* lab1;                            // labels in status bar
  QLabel* lab2;
  QLabel* lab3;
  QLabel* lab4;
  QLabel* lab5;
  QLabel* lab6;
  QLabel* lab7;
  QLabel* lab8;

  QMessageBox msgBox0;

  QFuture<void>* future1;
  QFuture<void>* future2;
  QFutureWatcher<void>* watcher1;
  QFutureWatcher<void>* watcher2;

  QProcess proc_m65;
  QProcess proc_qthid;
  QProcess proc_editor;

  double fcenter = 0.0;
  char mycall[12] = {};
  char mygrid[6] = {};
  char hiscall[12] = {};
  char hisgrid[6] = {};
  char datetime[17] = {};

  QHash<QString,bool> m_worked;

  SignalMeter *xSignalMeter;
  SignalMeter *ySignalMeter;

  SoundInThread soundInThread;             //Instantiate the audio threads
  SoundOutThread soundOutThread;

  QTimer* m_decodeIdleTimer = nullptr;
  int     m_decodeIdleTimeoutMs = 2500;  // tweak 150–300 as needed
  int m_decodeFinishedCount = 0;

  QSet<QString> m_seenLabels;

  //---------------------------------------------------- private functions
  void readSettings();
  void createStatusBar();
  void updateStatusBar();
  void msgBox(QString t);
  void genStdMsgs(QString rpt);
  void lookup();
  void ba2msg(QByteArray ba, char* message);
  void msgtype(QString t, QLineEdit* tx);
  void stub();
  bool isGrid4(QString g);
  // Parse a Messages-window decode line into its sender ("owner" of the
  // line) + grid, mirroring the same body/column extraction the Universal
  // Decode Label Parser uses for the live waterfall overlay (JT65
  // hash-column / Q65 colon-split). Returns false if no sender was found.
  bool parseMessageLine(const QString& t2, QString& sender, QString& grid);
  // Given already-tokenized message-body words, extract sender + grid.
  // Shared by parseMessageLine() and the "&" bandmap-line handler in
  // processStdOut().
  bool senderAndGridFromMsgCols(const QStringList& msg_cols, QString& sender, QString& grid);
  bool subProcessFailed (QProcess *, int exit_code, QProcess::ExitStatus);
  void read_log();
  void writeCrashData();
  void savetf2(QString fname, bool xpol);
  void getfile(QString fname, bool m_xpol, int dbDgrd);
  void processStdOut(QString text);
  void startSharedMemoryStdoutReader(DecoderContext* ctx);
  void createMessagesWindow();
  void mousePressEvent(QMouseEvent *event) override;
};

extern int g_sampleRate;
extern int active_nfft;
extern std::vector<qint16> id;

extern void getDev(int* numDevices,char hostAPI_DeviceName[][50],
                   int minChan[], int maxChan[],
                   int minSpeed[], int maxSpeed[]);

extern "C" {
//----------------------------------------------------- C and Fortran routines
  void symspec_(int* k, int* nxpol, int* ndiskdat, int* nb,
                int* m_NBslider, int* idphi, int* nfsample,
                int* iqadjust, int* iqapply, float* gainx, float* gainy,
                float* phasex, float* phasey, float* rejectx, float* rejecty,
                float* px, float* py, float s[], int* nkhz, int* nhsym,
                int* nzap, float* slimit, uchar lstrong[]);

  void gen65_(char* msg, int* mode65, double* samfac,
              int* nsendingsh, char* msgsent, short iwave[], int* nwave,
              int len1, int len2);

  void gen_q65_wave_(char* msg, int* ntxFreq, int* mode64,
              char* msgsent, short iwave[], int* nwave,
              int len1, int len2);

  int ptt_(int* nport, int* itx, int* iptt);

  void astrosub00_ (int* nyear, int* month, int* nday, double* uth, int* nfreq,
                    const char* mygrid, int* ndop00, int len1);
  }

#endif // MAINWINDOW_H
