#include "widegraph.h"
#include <QSettings>
#include <QMessageBox>
#include <QTimer>
#include <QDateTime>
#include <QTcpSocket>
#include <QHostAddress>
#include <QByteArray>
#include <QtEndian>
#include <QProcess>
#include <QCoreApplication>
#include <QStringList>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QSpinBox>
#include <QPushButton>
#include <QCheckBox>
#include <QLabel>
#include <cstdio>
#include <cstring>
#include "SettingsGroup.hpp"
#include "ui_widegraph.h"
#include "qmap_runtime_config.h"
#include "soundin.h"   // soundinLastUdpPacketMs() for no-stream detector

namespace {
// Re-runnable TCP handshake against the bridge's Linrad parameter
// server. Mirrors detectBridgeRateViaLinradTcp() in main.cpp; duped
// here (small + self-contained) so WideGraph can re-probe when the
// bridge starts AFTER QMAP did. Returns 0 on any failure (timeout,
// short reply, malformed rate). Default timeout is 300 ms — short
// because this runs on the GUI thread.
int probeBridgeRateViaLinradTcp(const QHostAddress& host, quint16 port,
                                int timeout_ms = 300) {
  QTcpSocket sock;
  sock.connectToHost(host, port);
  if (!sock.waitForConnected(timeout_ms)) return 0;
  const char request = '\xb8';
  if (sock.write(&request, 1) != 1 || !sock.waitForBytesWritten(timeout_ms)) {
    return 0;
  }
  while (sock.bytesAvailable() < 32) {
    if (!sock.waitForReadyRead(timeout_ms)) return 0;
  }
  QByteArray reply = sock.read(32);
  if (reply.size() < 32) return 0;
  qint32 rate_le;
  std::memcpy(&rate_le, reply.constData(), 4);
  const int rate = qFromLittleEndian<qint32>(rate_le);
  if (rate <= 0 || rate > qmap_runtime::MAX_IQ_RATE_HZ) return 0;
  return rate;
}
}

#define NFFT 32768   // 96 kHz baseline; runtime active size = qmap_runtime::activeNfft()

WideGraph::WideGraph (QString const& settings_filename, QWidget * parent)
  : QDialog {parent},
    ui {new Ui::WideGraph},
    m_settings_filename {settings_filename}
{
  ui->setupUi(this);
  setWindowTitle("Wideband Waterfall");
  {
    // Multi-instance identity, read directly from the same INI (same
    // pattern as every other settings read in this constructor) --
    // independent of MainWindow's own readSettings() timing since this
    // is a fresh QSettings read of already-persisted data.
    QSettings id_settings {m_settings_filename, QSettings::IniFormat};
    SettingsGroup g {&id_settings, "Common"};
    const QString name = id_settings.value("ConfigName", "").toString();
    if (!name.isEmpty()) {
      const int id = id_settings.value("InstanceId", 1).toInt();
      setInstanceLabel(id, name);
    }
  }
  setWindowFlags(Qt::WindowCloseButtonHint | Qt::WindowMinimizeButtonHint);
  installEventFilter(parent); //Installing the filter
  ui->widePlot->setCursor(Qt::CrossCursor);
  setMaximumWidth(2048);
  setMaximumHeight(880);
  ui->widePlot->setMaximumHeight(800);
  connect(ui->widePlot, SIGNAL(freezeDecode1(int)),this,
          SLOT(wideFreezeDecode(int)));
  // Click-to-work: relay CPlotter's callsignClicked up to MainWindow.
  // Using the new connect syntax for type-safety since both sides are
  // QObject members we control.
  connect(ui->widePlot, &CPlotter::callsignClicked,
          this,         &WideGraph::callsignClicked);

  //Restore user's settings
  QSettings settings {m_settings_filename, QSettings::IniFormat};
  {
    SettingsGroup g {&settings, "MainWindow"}; // historical reasons
    setGeometry (settings.value ("WideGraphGeom", QRect {45,30,1023,340}).toRect ());
  }
  SettingsGroup g {&settings, "WideGraph"};
  ui->widePlot->setPlotZero(settings.value("PlotZero", 20).toInt());
  ui->widePlot->setPlotGain(settings.value("PlotGain", 0).toInt());
  // Wide-waterfall tick spacing (KB2SA suggestion 2026-05-10).
  m_tickSpacingKhz = settings.value("TickSpacingKhz", 5).toInt();
  if (m_tickSpacingKhz != 5 && m_tickSpacingKhz != 10 &&
      m_tickSpacingKhz != 20 && m_tickSpacingKhz != 50) {
    m_tickSpacingKhz = 5;
  }
  ui->widePlot->setFreqPerDiv(static_cast<double>(m_tickSpacingKhz));
  ui->zeroSpinBox->setValue(ui->widePlot->getPlotZero());
  ui->gainSpinBox->setValue(ui->widePlot->getPlotGain());
  // Allow the FreqSpan spinbox to span the entire active band; the UI
  // file caps it at 90 (= 96 kHz baseline) which prevents the user from
  // seeing wider than ~90 kHz at --samplerate 256000. Bump the upper
  // bound to the active rate in kHz so the full band is selectable.
  // FreqSpan mode + user value. Backwards compat: if the legacy
  // "FreqSpan" key exists (older releases stored a literal kHz), use
  // it as the User-mode value default and migrate the mode to "user".
  // Default mode otherwise = Auto (full active band).
  const int legacy_span = settings.value("FreqSpan", -1).toInt();
  m_freqSpanUserValue   = settings.value("FreqSpanUserValue",
                                         legacy_span > 0 ? legacy_span : 96).toInt();
  if (m_freqSpanUserValue < 90)  m_freqSpanUserValue = 90;
  if (m_freqSpanUserValue > 256) m_freqSpanUserValue = 256;
  const QString mode_str = settings.value("FreqSpanMode",
                                          legacy_span > 0 ? "user" : "auto").toString();
  int mode_idx = SpanAuto;
  if      (mode_str == "96")   mode_idx = Span96;
  else if (mode_str == "256")  mode_idx = Span256;
  else if (mode_str == "user") mode_idx = SpanUser;
  // Block signals during initial widget setup; applyFreqSpan() at the
  // end pushes the chosen state into the plotter explicitly.
  {
    QSignalBlocker bm(ui->cbFreqSpanMode);
    QSignalBlocker bv(ui->freqSpanValueSpinBox);
    ui->cbFreqSpanMode->setCurrentIndex(mode_idx);
    ui->freqSpanValueSpinBox->setValue(effectiveSpanKhz());
    ui->freqSpanValueSpinBox->setEnabled(mode_idx == SpanUser);
  }
  applyFreqSpan();
  m_waterfallAvg = settings.value("WaterfallAvg",10).toInt();
  ui->waterfallAvgSpinBox->setValue(m_waterfallAvg);

  // Cache the Linrad endpoint so the late-bridge probe in
  // checkStreamLive() can hit the same host/port that main.cpp tried
  // at startup. Same defaults as main.cpp (Linrad/host = 127.0.0.1,
  // Linrad/tcp_port = 49812).
  {
    QSettings s {m_settings_filename, QSettings::IniFormat};
    SettingsGroup lg {&s, "Linrad"};
    m_linradHost     = s.value("host", "127.0.0.1").toString();
    m_linradTcpPort  = static_cast<quint16>(s.value("tcp_port", 49812).toInt());
  }

  // 1 Hz no-stream poll: when soundin's last UDP packet is > 2 s stale,
  // checkStreamLive() greys the IQ-rate label and appends "(no stream)".
  // Also re-probes the Linrad TCP handshake on the rising edge (stream
  // just came back up) when the bridge's advertised rate is still
  // unknown — this catches the "bridge launched after QMAP" case.
  // Cheap on idle (one atomic load + arithmetic per tick).
  m_streamCheckTimer = new QTimer(this);
  m_streamCheckTimer->setInterval(1000);
  connect(m_streamCheckTimer, &QTimer::timeout,
          this, &WideGraph::checkStreamLive);
  m_streamCheckTimer->start();

  // ── Decoded-callsign overlay controls ──
  // Three widgets appended to the existing horizontalLayout_3 row
  // (FreqSpan / WaterfallAvg / etc.) so they share the same line:
  //   [✓ Show callsigns]  [age N × TR]  [Clear]
  // Persisted to qmap.ini under [WideGraph]/{decode_labels_enabled,
  // decode_label_periods}.
  m_decodeLabelsEnabled =
      settings.value("decode_labels_enabled", true).toBool();
  m_decodeLabelPeriods =
      settings.value("decode_label_periods", 5).toInt();
  if (m_decodeLabelPeriods < 1) m_decodeLabelPeriods = 1;
  if (m_decodeLabelPeriods > 5) m_decodeLabelPeriods = 5;
  m_decodeLabelAlpha =
      settings.value("decode_label_alpha", 255).toInt();
  // Snap stored value to one of the offered presets so a hand-edited
  // INI doesn't leave the combo without a matching selection.
  if      (m_decodeLabelAlpha <= 187) m_decodeLabelAlpha = 175;
  else if (m_decodeLabelAlpha <= 210) m_decodeLabelAlpha = 200;
  else if (m_decodeLabelAlpha <= 237) m_decodeLabelAlpha = 220;
  else                                m_decodeLabelAlpha = 255;
  ui->widePlot->setDecodeLabelAlpha(m_decodeLabelAlpha);
  {
    const int fs = settings.value("decode_label_font_size",
                                  static_cast<int>(DecodeLabelFontSize::Normal)).toInt();
    if (fs == 7 || fs == 8 || fs == 10 || fs == 12) {
      m_decodeFontSize = static_cast<DecodeLabelFontSize>(fs);
    } else {
      m_decodeFontSize = DecodeLabelFontSize::Normal;
    }
    ui->widePlot->setDecodeLabelFontSize(m_decodeFontSize);
  }
  {
    const int pos = settings.value("decode_label_position",
                                   static_cast<int>(DecodeLabelPosition::Top)).toInt();
    m_decodeLabelPosition = (pos == static_cast<int>(DecodeLabelPosition::Bottom))
        ? DecodeLabelPosition::Bottom
        : DecodeLabelPosition::Top;
    ui->widePlot->setDecodeLabelPosition(m_decodeLabelPosition);
  }
  {
    auto* showCb = new QCheckBox("Show callsigns", this);
    showCb->setObjectName("decodeLabelShowCb");
    showCb->setChecked(m_decodeLabelsEnabled);
    showCb->setToolTip("Overlay decoded callsigns on the waterfall at "
                       "their decoded frequency. Uncheck to hide all "
                       "labels (existing entries are kept and reappear "
                       "when re-enabled).");
    connect(showCb, &QCheckBox::toggled, this, [this](bool on) {
        m_decodeLabelsEnabled = on;
        // Push current list (or empty) to the plotter so it repaints.
        ui->widePlot->setDecodeLabels(on ? m_decodeLabels
                                         : QList<DecodeLabel>{});
        // Tell MainWindow so the View menu mirror updates without
        // either side polling.
        emit decodeLabelsEnabledChanged(on);
    });

    auto* ageSpin = new QSpinBox(this);
    ageSpin->setObjectName("decodeLabelAgeSpin");
    ageSpin->setRange(1, 5);
    ageSpin->setValue(m_decodeLabelPeriods);
    ageSpin->setSuffix(" × TR");
    ageSpin->setToolTip("How many TR periods a decoded callsign label "
                        "stays on the waterfall after its last fresh "
                        "decode. Default 5 = 5 minutes for Q65-60.");
    connect(ageSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [this](int v) {
                m_decodeLabelPeriods = v;
                ageDecodeLabels();   // immediate sweep with new lifetime
            });

    auto* clearBtn = new QPushButton("Clear", this);
    clearBtn->setObjectName("decodeLabelClearBtn");
    clearBtn->setToolTip("Remove all decoded callsign labels from the "
                         "waterfall. Existing decodes will re-appear "
                         "if WS-MAP decodes them again.");
    connect(clearBtn, &QPushButton::clicked, this, [this]() {
        m_decodeLabels.clear();
        ui->widePlot->setDecodeLabels(m_decodeLabels);
    });

    // Insert into the existing controls row (horizontalLayout_3) just
    // before its trailing stretch, so the new widgets share the line
    // with FreqSpan/WaterfallAvg instead of wrapping below.
    if (auto* row = findChild<QHBoxLayout*>("horizontalLayout_3")) {
      row->addWidget(showCb);
      row->addWidget(ageSpin);
      row->addWidget(clearBtn);
    } else {
      // Fallback: own row appended to inner verticalLayout.
      auto* row2 = new QHBoxLayout;
      row2->setContentsMargins(0, 0, 0, 0);
      row2->addWidget(showCb);
      row2->addWidget(ageSpin);
      row2->addWidget(clearBtn);
      row2->addStretch();
      if (auto* inner = findChild<QVBoxLayout*>("verticalLayout")) {
        inner->addLayout(row2);
      }
    }
  }
}

WideGraph::~WideGraph()
{
  saveSettings();
  delete ui;
}

void WideGraph::resizeEvent(QResizeEvent* )                    //resizeEvent()
{
  if(!size().isValid()) return;
  int w = size().width();
  int h = size().height();
  ui->labFreq->setGeometry(QRect(w-256,h-100,227,41));
}

void WideGraph::saveSettings()
{
  //Save user's settings
  QSettings settings {m_settings_filename, QSettings::IniFormat};
  {
    SettingsGroup g {&settings, "MainWindow"}; // for historical reasons
    settings.setValue ("WideGraphGeom", geometry());
  }
  SettingsGroup g {&settings, "WideGraph"};
  settings.setValue("PlotZero",ui->widePlot->m_plotZero);
  settings.setValue("PlotGain",ui->widePlot->m_plotGain);
  settings.setValue("PlotWidth",ui->widePlot->plotWidth());
  // Persist mode + the User-mode value separately. Mode is what the
  // user picked (Auto/96/256/User); the User-mode kHz is preserved
  // even when not currently in User mode so toggling back to User
  // restores the prior choice. Range-clamp to 90..256 on save in
  // case some other path mutated it.
  const char* mode_label = "auto";
  switch (ui->cbFreqSpanMode->currentIndex()) {
    case Span96:   mode_label = "96";   break;
    case Span256:  mode_label = "256";  break;
    case SpanUser: mode_label = "user"; break;
    default:       mode_label = "auto"; break;
  }
  settings.setValue("FreqSpanMode", QString::fromLatin1(mode_label));
  int user_v = m_freqSpanUserValue;
  if (user_v < 90)  user_v = 90;
  if (user_v > 256) user_v = 256;
  settings.setValue("FreqSpanUserValue", user_v);
  settings.remove("FreqSpan");   // legacy key superseded
  settings.setValue("WaterfallAvg",ui->waterfallAvgSpinBox->value());
  settings.setValue("decode_label_periods", m_decodeLabelPeriods);
  settings.setValue("decode_labels_enabled", m_decodeLabelsEnabled);
  settings.setValue("decode_label_alpha",    m_decodeLabelAlpha);
  settings.setValue("decode_label_font_size", static_cast<int>(m_decodeFontSize));
  settings.setValue("decode_label_position", static_cast<int>(m_decodeLabelPosition));
}

void WideGraph::dataSink2(float s[], int nkhz, int ihsym, int ndiskdata,
                          uchar lstrong[])
{
  static float splot[qmap_runtime::MAX_NFFT];
  const int nfft_a = qmap_runtime::activeNfft();
  const int dc_bin = nfft_a / 2;            // FFTW-shifted DC index
  // lstrong has 1024 entries spanning the active spectrum; divisor was
  // i/32 = i/(NFFT/1024) at NFFT=32768.
  const int lstrong_div = (nfft_a >= 1024) ? (nfft_a / 1024) : 1;
  float swide[2048];
  float smax;
  double df;
  int nbpp = ui->widePlot->binsPerPixel();
  static int n=0;
  static int nkhz0=-999;
  static int ntrz=0;

  df = m_fSample / static_cast<double>(nfft_a);   // was: m_fSample/32768.0
  if(nkhz != nkhz0) {
    ui->widePlot->setNkhz(nkhz);                   //Why do we need both?
    ui->widePlot->SetCenterFreq(nkhz);             //Why do we need both?
    ui->widePlot->setFQSO(nkhz,true);
    nkhz0 = nkhz;
  }

  //Average spectra over specified number, m_waterfallAvg
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
      splot[i] /= n;                       //Normalize the average
    }
    n=0;

    int w=ui->widePlot->plotWidth();
    qint64 sf = nkhz - 0.5*w*nbpp*df/1000.0;
    // N6NU 2026-05-25: clamp sf to the actual FFT bin-0 frequency so
    // the i0 calc below never goes negative. When w*nbpp > nfft (the
    // common case after a window resize, because nbpp rounds up to
    // integer), the un-clamped sf is BELOW the lowest FFT freq, i0
    // would compute negative, then the `if (i0 < 0) i0 = 0;` line
    // would silently shift the waterfall data RIGHT by (-i0/nbpp)
    // pixels — leaving overlay labels (which use XfromFreq via the
    // un-clamped m_StartFreq) ~1 kHz LEFT of their signal blobs.
    // Clamping sf here keeps blob and label coordinates in lock-step
    // for any window size.
    const qint64 sf_min = nkhz - static_cast<qint64>(0.5 * m_fSample / 1000.0);
    if (sf < sf_min) sf = sf_min;
    if(sf != ui->widePlot->startFreq()) ui->widePlot->SetStartFreq(sf);
    int i0=dc_bin+(ui->widePlot->startFreq()-nkhz+0.001*m_fCal) * 1000.0/df + 0.5;
    if (i0 < 0) i0 = 0;
    if (i0 > nfft_a - 1) i0 = nfft_a - 1;
    int i=i0;
    // swide[] is sized 2048 but the visible waterfall is only `w` pixels
    // wide. At wide-mode the (i0 + 2048*nbpp) read range can run off the
    // end of nfft_active and read uninitialised memory past splot[] —
    // that's the red/black band N6NU saw at the band edge. Stop the
    // per-pixel loop the moment the inner loop would step past
    // nfft_active, and zero the swide tail so the painter doesn't carry
    // stale garbage from earlier (narrower) frames.
    int j=0;
    for (; j<2048 && i+nbpp<=nfft_a; ++j) {
        smax=0;
        for (int k=0; k<nbpp; k++) {
            i++;
            if(splot[i]>smax) smax=splot[i];
        }
        swide[j]=smax;
        const int li = 1 + i/lstrong_div;
        if(li >= 0 && li < 1024 && lstrong[li] != 0) swide[j]=-smax;   //Tag strong signals
    }
    for (; j<2048; ++j) swide[j]=0;

// Time according to this computer
    qint64 ms = QDateTime::currentMSecsSinceEpoch() % 86400000;
    int ntr = (ms/1000) % m_TRperiod;

    if((ndiskdata && ihsym <= m_waterfallAvg) || (!ndiskdata && ntr<ntrz)) {
      const int swide_n = sizeof(swide) / sizeof(swide[0]);
      for (int i=0; i<swide_n; i++) {
        swide[i] = 1.e30;
      }
      for (int i=0; i<nfft_a; i++) {
        splot[i] = 1.e30;
      }
    }
    ntrz=ntr;
    ui->widePlot->draw(swide,i0,splot);
  }
}

void WideGraph::on_waterfallAvgSpinBox_valueChanged(int n)
{
  m_waterfallAvg = n;
}

int WideGraph::effectiveSpanKhz() const
{
  switch (ui->cbFreqSpanMode->currentIndex()) {
    case Span96:   return 96;
    case Span256:  return 256;
    case SpanUser: return m_freqSpanUserValue;
    case SpanAuto:
    default:       return qmap_runtime::activeRateHz() / 1000;
  }
}

void WideGraph::applyFreqSpan()
{
  const int n = effectiveSpanKhz();
  ui->widePlot->setNSpan(n);
  const int w = ui->widePlot->plotWidth();
  if (w > 0) {
    const double rate_khz = qmap_runtime::activeRateHz() / 1000.0;
    int nbpp = n * static_cast<double>(qmap_runtime::activeNfft()) / (w * rate_khz) + 0.5;
    if (nbpp < 1) nbpp = 1;
    ui->widePlot->setBinsPerPixel(nbpp);
  }
  ui->widePlot->update();
}

void WideGraph::on_cbFreqSpanMode_currentIndexChanged(int idx)
{
  // Refresh the right-side readout to the new mode's value, and only
  // enable editing in User mode. setValue's signal is blocked because
  // we don't want it interpreted as the user typing into User mode.
  const bool user_mode = (idx == SpanUser);
  {
    QSignalBlocker bv(ui->freqSpanValueSpinBox);
    ui->freqSpanValueSpinBox->setValue(effectiveSpanKhz());
  }
  ui->freqSpanValueSpinBox->setEnabled(user_mode);
  applyFreqSpan();
}

void WideGraph::on_freqSpanValueSpinBox_valueChanged(int n)
{
  // Editable only when User mode is active; cache the value and
  // re-apply. Clamp defensively (UI already enforces min=90, max=256
  // but the cached value is also persisted).
  if (n < 90)  n = 90;
  if (n > 256) n = 256;
  m_freqSpanUserValue = n;
  if (ui->cbFreqSpanMode->currentIndex() == SpanUser) {
    applyFreqSpan();
  }
}

void WideGraph::on_zeroSpinBox_valueChanged(int value)
{
  ui->widePlot->setPlotZero(value);
}

void WideGraph::on_gainSpinBox_valueChanged(int value)
{
  ui->widePlot->setPlotGain(value);
}

void WideGraph::keyPressEvent(QKeyEvent *e)
{  
  switch(e->key())
  {
  case Qt::Key_F11:
    emit f11f12(11);
    break;
  case Qt::Key_F12:
    emit f11f12(12);
    break;
  default:
    e->ignore();
  }
}

int WideGraph::QSOfreq()
{
  return ui->widePlot->fQSO();
}

int WideGraph::nSpan()
{
  return ui->widePlot->m_nSpan;
}

float WideGraph::fSpan()
{
  return ui->widePlot->m_fSpan;
}

int WideGraph::nStartFreq()
{
  return ui->widePlot->startFreq();
}

void WideGraph::wideFreezeDecode(int n)
{
  emit freezeDecode2(n);
}

void WideGraph::setTol(int n)
{
  ui->widePlot->m_tol=n;
  ui->widePlot->DrawOverlay();
  ui->widePlot->update();
}

int WideGraph::Tol()
{
  return ui->widePlot->m_tol;
}

void WideGraph::setDF(int n)
{
  ui->widePlot->m_DF=n;
  ui->widePlot->DrawOverlay();
  ui->widePlot->update();
}

void WideGraph::setFcal(int n)
{
  m_fCal=n;
  ui->widePlot->setFcal(n);
}

void WideGraph::setDecodeFinished()
{
  ui->widePlot->DecodeFinished();
}

int WideGraph::DF()
{
  return ui->widePlot->m_DF;
}

void WideGraph::on_autoZeroPushButton_clicked()
{
   int nzero=ui->widePlot->autoZero();
   ui->zeroSpinBox->setValue(nzero);
}

void WideGraph::setPalette(QString palette)
{
  ui->widePlot->setPalette(palette);
}
void WideGraph::setFsample(int n)
{
  m_fSample=n;
  ui->widePlot->setFsample(n);
}

void WideGraph::setInstanceLabel(int id, const QString& name)
{
  QString title = "Wideband Waterfall";
  if (!name.isEmpty()) {
    title += QString(" \xe2\x80\x94 %1  %2").arg(id).arg(name);   // em-dash
  }
  setWindowTitle(title);
}

void WideGraph::setIqRateLabel(int rate_hz, const QString& source)
{
  // rate_hz must already be the active runtime rate from
  // qmap_runtime::activeRateHz(); source is one of "auto", "manual",
  // "cli", "baseline". If the bridge advertised a different rate
  // (qmap_runtime::rateMismatch()), colorize yellow to flag the
  // override-vs-bridge mismatch. Live rendering (mismatch + no-stream
  // composition) lives in renderIqRateLabel(); this method just caches
  // the inputs for the 1 Hz no-stream poll to re-render against.
  if (rate_hz <= 0) return;
  m_iqRateHz       = rate_hz;
  m_iqRateSource   = source;
  m_iqRateLabelSet = true;
  renderIqRateLabel();
}

void WideGraph::renderIqRateLabel()
{
  if (!m_iqRateLabelSet) return;
  const int rate_khz = m_iqRateHz / 1000;
  QString text;
  QString style;
  QString tooltip;
  if (qmap_runtime::rateMismatch()) {
    const int adv_khz = qmap_runtime::bridgeAdvertisedRateHz() / 1000;
    text = QString {"IQ rate: %1 kHz (%2, bridge says %3 kHz!)"}
           .arg (rate_khz).arg (m_iqRateSource).arg (adv_khz);
    style = "color: #b58900; font-weight: bold;";  // yellow (Solarized)
    tooltip = QString {
      "Active rate (%1 kHz, %2) does not match what the bridge "
      "advertised on its Linrad TCP handshake (%3 kHz). Decodes will "
      "be garbled until the two agree. Either drop the manual override "
      "(DevSetup → Auto) or restart the bridge with --linrad-rate %1000."
    }.arg (rate_khz).arg (m_iqRateSource).arg (adv_khz);
  } else if (qmap_runtime::rateSource() == 0 && m_linradTcpPort > 0) {
    // Baseline = startup TCP probe failed AND user's mode_str was
    // "auto". Most common cause is a stale Linrad/tcp_port in the
    // INI from prior testing pointing at a port the bridge isn't on.
    // Surface the endpoint we tried so the misconfig is visible at a
    // glance instead of needing an INI excavation.
    text = QString {"IQ rate: %1 kHz (baseline — probe failed at %2:%3)"}
           .arg (rate_khz).arg (m_linradHost).arg (m_linradTcpPort);
    style = "color: #b58900;";   // yellow — this is a problem, not a state
    tooltip = QString {
      "WS-MAP couldn't reach the Linrad parameter server at %1:%2 at "
      "startup, so it fell back to the 96 kHz baseline. Common cause: "
      "the Linrad/tcp_port in wsmap.ini doesn't match the bridge's "
      "actual TCP port (default 49812). Fix in DevSetup → I/O Devices "
      "→ TCP port, then restart WS-MAP."
    }.arg (m_linradHost).arg (m_linradTcpPort);
  } else {
    text = QString {"IQ rate: %1 kHz (%2)"}
           .arg (rate_khz).arg (m_iqRateSource);
    tooltip = QString {
      "Active Linrad IQ sample rate. '%1' = how WS-MAP picked this value."
    }.arg (m_iqRateSource);
  }
  if (!m_streamLive) {
    // No-stream wins visually over a mismatch: if the bridge isn't
    // sending anything, the rate it advertised is moot.
    text += " (no stream)";
    style = "color: #888; font-style: italic;";
    tooltip = QString {
      "No UDP packets received from the bridge in the last 2+ seconds. "
      "Is the bridge running and pointing at port %1?"
    }.arg (50004);  // soundin binds wsmap.ini [Common]/UDPport, default 50004
  }
  ui->labIqRate->setText(text);
  ui->labIqRate->setStyleSheet(style);
  ui->labIqRate->setToolTip(tooltip);
}

void WideGraph::checkStreamLive()
{
  // Considered live if a UDP packet arrived within the last 2 s. The
  // first-packet edge case: g_lastUdpPacketMs starts at 0, so until the
  // very first packet the label correctly shows "(no stream)".
  const qint64 now_ms  = QDateTime::currentMSecsSinceEpoch();
  const qint64 last_ms = soundinLastUdpPacketMs();
  const bool live = (last_ms > 0) && (now_ms - last_ms <= 2000);
  if (live != m_streamLive) {
    m_streamLive = live;
    // Late-bridge probe: when the stream comes up AND the startup TCP
    // handshake never got a reply (bridge wasn't running yet at QMAP
    // launch), re-probe now. If the bridge advertises a rate that
    // doesn't match QMAP's active rate, the existing rateMismatch()
    // path turns the label yellow with "bridge says N kHz!". One-shot:
    // once any non-zero rate is recorded we never re-probe (a bridge
    // restart with a different --linrad-rate would need a QMAP restart
    // anyway, since FFT plans + Fortran allocations are baked at
    // startup).
    // On every stream-live rising edge, re-probe the Linrad TCP
    // handshake. Catches both the first-time case (bridge launched
    // after QMAP) and bridge-restart-with-different-rate. The probe
    // is a 300 ms blocking call on the GUI thread, but transitions
    // are rare. We only update the cached advertised rate on a
    // successful probe; a TCP failure leaves the prior value alone
    // so renderIqRateLabel() doesn't lose the mismatch warning over
    // a transient hiccup.
    if (live && m_linradTcpPort > 0) {
      const QHostAddress host {m_linradHost};
      const int detected = probeBridgeRateViaLinradTcp(host, m_linradTcpPort);
      if (detected > 0) {
        const int prev = qmap_runtime::bridgeAdvertisedRateHz();
        qmap_runtime::setBridgeAdvertisedRateHz(detected);
        if (detected != prev) {
          std::fprintf(stderr,
                       "[qmap] bridge probe: %s:%u advertises %d Hz "
                       "(active %d, was %d)\n",
                       m_linradHost.toUtf8().constData(), m_linradTcpPort,
                       detected, qmap_runtime::activeRateHz(), prev);
        }

        // Auto-relaunch when the user chose auto (rateSource 0 = auto
        // requested but startup TCP missed; 3 = auto succeeded —
        // either at startup or via a previous auto-relaunch's
        // synthesised --samplerate) AND the bridge's current rate
        // disagrees with active. CLI / INI explicit overrides
        // (rateSource 1, 2) keep the yellow mismatch warning instead;
        // we won't trample an explicit user choice.
        const int rs = qmap_runtime::rateSource();
        if (detected != qmap_runtime::activeRateHz()
            && (rs == 0 || rs == 3)) {
          std::fprintf(stderr,
                       "[qmap] auto mode: relaunching with --samplerate %d "
                       "(active %d Hz, bridge now %d Hz)\n",
                       detected, qmap_runtime::activeRateHz(), detected);
          saveSettings();
          QStringList args;
          args << "--samplerate" << QString::number(detected);
          // Preserve --config so instance B doesn't relaunch into the
          // default qmap.ini (which would clobber instance A's slot).
          if (!qmap_runtime::g_config_path.isEmpty()) {
            args << "--config" << qmap_runtime::g_config_path;
          }
          QProcess::startDetached(QCoreApplication::applicationFilePath(), args);
          // QApplication::quit() walks the event loop unwind so
          // top-level windows (MainWindow → its own saveSettings)
          // still get a chance to persist state.
          QCoreApplication::quit();
          return;
        }
      }
    }
    renderIqRateLabel();
  }
  // Sweep aged-out decode labels on every tick (1 Hz). Cheap when the
  // list is empty; bounded by kDecodeLabelMax otherwise.
  ageDecodeLabels();
}

void WideGraph::setTickSpacingKhz(int khz)
{
  // Clamp to known menu values; ignore anything outside the set so a
  // stale or hand-edited INI value can't break the plotter.
  if (khz != 5 && khz != 10 && khz != 20 && khz != 50) khz = 5;
  if (m_tickSpacingKhz == khz) return;
  m_tickSpacingKhz = khz;
  ui->widePlot->setFreqPerDiv(static_cast<double>(khz));
  QSettings settings {m_settings_filename, QSettings::IniFormat};
  SettingsGroup g {&settings, "WideGraph"};
  settings.setValue("TickSpacingKhz", m_tickSpacingKhz);
}

void WideGraph::setDecodeLabelAlpha(int alpha)
{
  // Clamp to the three menu presets: None=255 (opaque), Medium=200,
  // High=175. Anything outside snaps to the nearest, so a stale INI
  // can't desync the View menu's checked state.
  if      (alpha <= 187) alpha = 175;
  else if (alpha <= 210) alpha = 200;
  else if (alpha <= 237) alpha = 220;
  else                   alpha = 255;
  if (m_decodeLabelAlpha == alpha) return;
  m_decodeLabelAlpha = alpha;
  ui->widePlot->setDecodeLabelAlpha(m_decodeLabelAlpha);
  QSettings settings {m_settings_filename, QSettings::IniFormat};
  SettingsGroup g {&settings, "WideGraph"};
  settings.setValue("decode_label_alpha", m_decodeLabelAlpha);
}

void WideGraph::setDecodeLabelFontSize(DecodeLabelFontSize sz)
{
  if (m_decodeFontSize == sz) return;
  m_decodeFontSize = sz;
  ui->widePlot->setDecodeLabelFontSize(sz);
  QSettings settings {m_settings_filename, QSettings::IniFormat};
  SettingsGroup g {&settings, "WideGraph"};
  settings.setValue("decode_label_font_size", static_cast<int>(sz));
}

void WideGraph::setDecodeLabelPosition(DecodeLabelPosition p)
{
  if (m_decodeLabelPosition == p) return;
  m_decodeLabelPosition = p;
  ui->widePlot->setDecodeLabelPosition(p);
  QSettings settings {m_settings_filename, QSettings::IniFormat};
  SettingsGroup g {&settings, "WideGraph"};
  settings.setValue("decode_label_position", static_cast<int>(p));
}

void WideGraph::setDecodeLabelsEnabled(bool on)
{
  // Programmatic setter — MainWindow uses this when the View menu
  // action is toggled. Forwards through to the existing checkbox so
  // its toggled signal fires (which keeps m_decodeLabelsEnabled, the
  // plotter, and the menu mirror all in sync via the same code path
  // the user-click case takes).
  if (auto* cb = findChild<QCheckBox*>("decodeLabelShowCb")) {
    if (cb->isChecked() != on) cb->setChecked(on);
  } else {
    // Fallback if the checkbox isn't there for some reason — still
    // update state and push to plotter.
    m_decodeLabelsEnabled = on;
    ui->widePlot->setDecodeLabels(on ? m_decodeLabels
                                     : QList<DecodeLabel>{});
    emit decodeLabelsEnabledChanged(on);
  }
}

void WideGraph::addDecodeLabel(double freq_khz, const QString& callsign, bool is_cq,
                                const QString& grid, bool is_active, const QString& mode,
                                int even_period, double fsked_khz)
{
  if (!m_decodeLabelsEnabled || callsign.isEmpty()) return;
  const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
  // Two-pass over the list: (1) if the same callsign appears at the
  // same freq (within 50 Hz — tighter than Q65's ~10 Hz sync but loose
  // enough to absorb cycle-to-cycle wobble), refresh in place. (2) if
  // it appears at a *different* freq, that's a station that QSY'd —
  // drop the old entry so the label moves rather than leaving a stale
  // ghost on the waterfall.
  auto it = m_decodeLabels.begin();
  while (it != m_decodeLabels.end()) {
    if (it->callsign == callsign) {
      if (std::abs(it->freq_khz - freq_khz) < 0.05) {
        it->last_seen_ms = now_ms;
        it->hits++;
        it->is_cq = is_cq;
        it->is_active = is_active;
        // Sticky grid: only overwrite when the new emit carries one,
        // so a CQ (which has the grid) is not stomped by a later reply
        // that omits it.
        if (!grid.isEmpty()) it->grid = grid;
        // Mode is present on every Q65 decode line, so unlike grid it
        // refreshes unconditionally to track a station that changed
        // sub-mode between cycles.
        if (!mode.isEmpty()) it->mode = mode;
        // Sticky parity: only update when this emit knows it (>=0), so a
        // re-highlight redraw (which passes -1) never clobbers a real value.
        if (even_period >= 0) it->even_period = even_period;
        // Sticky fsked: refresh when this emit carries one (>=0); re-highlight
        // redraws pass -1 and must not clobber a real value.
        if (fsked_khz >= 0) it->fsked_khz = fsked_khz;
        ui->widePlot->setDecodeLabels(m_decodeLabels);
        return;
      }
      // Same call, different freq → station moved. Drop and continue.
      it = m_decodeLabels.erase(it);
    } else {
      ++it;
    }
  }
  // New entry. Evict oldest if at cap (defensive — 200 labels is more
  // than any plausible EME minute).
  if (m_decodeLabels.size() >= kDecodeLabelMax) {
    int oldest_idx = 0;
    qint64 oldest = m_decodeLabels[0].last_seen_ms;
    for (int i = 1; i < m_decodeLabels.size(); ++i) {
      if (m_decodeLabels[i].last_seen_ms < oldest) {
        oldest = m_decodeLabels[i].last_seen_ms;
        oldest_idx = i;
      }
    }
    m_decodeLabels.removeAt(oldest_idx);
  }
  m_decodeLabels.append(DecodeLabel(freq_khz, callsign, now_ms, 1, is_cq, grid, is_active, mode, even_period, fsked_khz));
  ui->widePlot->setDecodeLabels(m_decodeLabels);
}

void WideGraph::updateActiveCallsign(const QString& call)
{
  // Called whenever QMAP's DXcall changes (set on a click-to-work tap,
  // cleared on reset). Re-derives is_active for every existing label
  // immediately, instead of waiting for each station's next decode --
  // otherwise the old call stays red and the newly-selected one doesn't
  // turn red until both happen to decode again. Mirrors MAP65's
  // WideGraph::updateActiveCallsign.
  bool changed = false;
  for (auto& lab : m_decodeLabels) {
    const bool active = !call.isEmpty() && lab.callsign == call;
    if (lab.is_active != active) {
      lab.is_active = active;
      changed = true;
    }
  }
  if (changed && ui && ui->widePlot) ui->widePlot->setDecodeLabels(m_decodeLabels);
}

void WideGraph::clearDecodeLabels()
{
  if (m_decodeLabels.isEmpty()) return;
  m_decodeLabels.clear();
  if (ui && ui->widePlot) ui->widePlot->setDecodeLabels(m_decodeLabels);
}

void WideGraph::ageDecodeLabels()
{
  if (m_decodeLabels.isEmpty()) return;
  const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
  const qint64 lifetime_ms =
      static_cast<qint64>(m_decodeLabelPeriods) * m_TRperiod * 1000;
  bool changed = false;
  auto it = m_decodeLabels.begin();
  while (it != m_decodeLabels.end()) {
    if (now_ms - it->last_seen_ms > lifetime_ms) {
      it = m_decodeLabels.erase(it);
      changed = true;
    } else {
      ++it;
    }
  }
  if (changed) ui->widePlot->setDecodeLabels(m_decodeLabels);
}

void WideGraph::setMode65(int n)
{
  m_mode65=n;
  ui->widePlot->setMode65(n);
}

void WideGraph::on_cbSpec2d_toggled(bool b)
{
  ui->widePlot->set2Dspec(b);
}

double WideGraph::fGreen()
{
  return ui->widePlot->fGreen();
}

void WideGraph::setPeriod(int n)
{
  m_TRperiod=n;
}

void WideGraph::updateFreqLabel()
{
  auto rxFreq = QString {"%1"}.arg (ui->widePlot->rxFreq (), 10, 'f', 6);
  rxFreq.insert (rxFreq.size () - 3, '.');
  ui->labFreq->setText (QString {"Center freq:  %1"}.arg (rxFreq));
}
