#include "ft8wide.h"

#include <fftw3.h>
#include "qt_helpers.hpp"      // SkipEmptyParts for Qt 5.12 (QString::) and 5.15 (Qt::)
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QStringList>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QThread>
#include <QtEndian>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <vector>

namespace {
  constexpr int OUT_RATE = 12000;                    // jt9 audio rate

  struct ModeParams { const char* name; const char* jt9Flag; double periodS; };
  ModeParams params(WideMode m) {
    if (m == WideMode::FT4) return { "FT4", "-5", 7.5 };
    return { "FT8", "-8", 15.0 };
  }

  // Persistent FFTW plans, one set per mode: the forward plan depends on the
  // IQ rate and the period length, the inverse one on the period length.
  struct Plans {
    int n = 0, outN = 0;
    fftwf_plan fwd = nullptr;
    fftwf_plan inv = nullptr;
    fftwf_complex* in = nullptr;
    fftwf_complex* spec = nullptr;
    fftwf_complex* ybins = nullptr;
    float* y = nullptr;
    ~Plans() { release(); }
    void release() {
      if (fwd) fftwf_destroy_plan(fwd);
      if (inv) fftwf_destroy_plan(inv);
      if (in) fftwf_free(in);
      if (spec) fftwf_free(spec);
      if (ybins) fftwf_free(ybins);
      if (y) fftwf_free(y);
      fwd = inv = nullptr; in = spec = ybins = nullptr; y = nullptr; n = outN = 0;
    }
    void ensure(int N, int outLen) {
      if (n == N && outN == outLen) return;
      release();
      n = N; outN = outLen;
      in    = fftwf_alloc_complex(N);
      spec  = fftwf_alloc_complex(N);
      ybins = fftwf_alloc_complex(outLen / 2 + 1);
      y     = fftwf_alloc_real(outLen);
      fwd = fftwf_plan_dft_1d(N, in, spec, FFTW_FORWARD, FFTW_ESTIMATE);
      inv = fftwf_plan_dft_c2r_1d(outLen, ybins, y, FFTW_ESTIMATE);
    }
  };
  Plans g_plans[2];
  Plans& plansFor(WideMode m) { return g_plans[m == WideMode::FT4 ? 1 : 0]; }

  void writeWav(const QString& path, const float* y, int n) {
    float peak = 1.0f;
    for (int i = 0; i < n; ++i) peak = std::max(peak, std::fabs(y[i]));
    const float scale = 8000.0f / peak;            // moderate level, as the prototype
    std::vector<qint16> s(n);
    for (int i = 0; i < n; ++i) {
      float v = y[i] * scale;
      v = std::max(-32767.0f, std::min(32767.0f, v));
      s[i] = qToLittleEndian(qint16(std::lround(v)));
    }
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return;
    const quint32 dataBytes = quint32(n * 2);
    char hdr[44];
    std::memcpy(hdr, "RIFF", 4);
    qToLittleEndian<quint32>(36 + dataBytes, hdr + 4);
    std::memcpy(hdr + 8, "WAVEfmt ", 8);
    qToLittleEndian<quint32>(16, hdr + 16);
    qToLittleEndian<quint16>(1, hdr + 20);            // PCM
    qToLittleEndian<quint16>(1, hdr + 22);            // mono
    qToLittleEndian<quint32>(OUT_RATE, hdr + 24);
    qToLittleEndian<quint32>(OUT_RATE * 2, hdr + 28);
    qToLittleEndian<quint16>(2, hdr + 32);
    qToLittleEndian<quint16>(16, hdr + 34);
    std::memcpy(hdr + 36, "data", 4);
    qToLittleEndian<quint32>(dataBytes, hdr + 40);
    f.write(hdr, 44);
    f.write(reinterpret_cast<const char*>(s.data()), dataBytes);
  }
}

namespace {
  // False decodes from a 12-slice x 4-per-minute skimmer at depth 3 look like
  // "TU; J7DTT DU1IFQ R 579 2960" or "HQ4QF5QEJQMC" at -21 dB, scattered over
  // the window (first hour of FT4 on 20 m, 2026-09-09: 25 lines, none real).
  // Two cheap tests remove them: an SNR floor a little below each mode's real
  // decode threshold (FT8 -21 dB, FT4 -17.5 dB), and the message must carry a
  // callsign-shaped word where a standard message has one.
  bool plausible(const QString& msg, int snr, WideMode mode)
  {
    if (snr < (mode == WideMode::FT4 ? -19 : -24)) return false;
    const QStringList w = msg.split(' ', SkipEmptyParts);   // qt_helpers.hpp: Qt 5.12 and 5.15 forms
    if (w.isEmpty()) return false;
    if (w[0].endsWith(';')) return false;            // RTTY-RU / contest exchange (i3=3)
    static const QRegularExpression call("^([A-Z0-9]{1,4}/)?[A-Z0-9]{1,3}[0-9][A-Z0-9]{0,3}[A-Z](/[A-Z0-9]+)?$");
    if (w[0] == "CQ" || w[0] == "QRZ" || w[0] == "DE") {
      for (int k = 1; k < std::min(3, int(w.size())); ++k)
        if (call.match(w[k]).hasMatch()) return true;
      return false;
    }
    return w.size() >= 2 && call.match(w[0]).hasMatch() && call.match(w[1]).hasMatch();
  }
}

QString Ft8Wide::modeName(WideMode mode) { return QString::fromLatin1(params(mode).name); }
double  Ft8Wide::periodSeconds(WideMode mode) { return params(mode).periodS; }
QString Ft8Wide::stamp(const QDateTime& periodStart) { return periodStart.addMSecs(500).toString("HHmmss"); }

// Status text goes to the status bar (signal) and to stderr, like the other
// "[qmap] ..." traces, so a headless or logged run shows what happened.
void Ft8Wide::say(const QString& text)
{
  std::fprintf(stderr, "[qmap-ft8] %s\n", text.toUtf8().constData());
  std::fflush(stderr);
  emit status(text);
}

Ft8Wide::Ft8Wide(QObject* parent) : QObject(parent)
{
  m_tempRoot = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                 .absoluteFilePath(QString("qmap-ft8-%1").arg(QCoreApplication::applicationPid()));
  QDir().mkpath(m_tempRoot);
}

void Ft8Wide::setInstanceId(int id)
{
  if (m_memFt8.isAttached()) m_memFt8.detach();
  m_ft8Shm = nullptr;
  const QString key = (id <= 1) ? QStringLiteral("mem_qmap_ft8") : QString("mem_qmap_ft8_%1").arg(id);
  m_memFt8.setKey(key);
  if (!m_memFt8.attach()) {
    if (m_memFt8.create(sizeof(QmapFt8Fwd))) {
      m_memFt8.lock(); std::memset(m_memFt8.data(), 0, sizeof(QmapFt8Fwd)); m_memFt8.unlock();
    }
  }
  if (m_memFt8.isAttached()) m_ft8Shm = static_cast<QmapFt8Fwd*>(m_memFt8.data());
  std::fprintf(stderr, "[qmap-ft8] handoff segment %s attached=%d\n",
               key.toUtf8().constData(), m_memFt8.isAttached());
}

void Ft8Wide::publish(const QList<Ft8Decode>& list, const QDateTime& periodStart)
{
  if (!m_ft8Shm || !m_memFt8.isAttached() || list.isEmpty()) return;
  const QString st = stamp(periodStart);
  // Lock-free on purpose.  QSharedMemory::lock() is a system semaphore that
  // a dying holder never releases: WSJT-X crashed mid-read and QMAP then sat
  // in lock() forever (2026-09-09, waterfall frozen for good).  With one
  // writer and a monotonic counter no lock is needed: fill the slot, then
  // publish it by bumping write_seq behind a release fence; the reader only
  // touches slots below the counter it has seen.
  volatile int* seqp = &m_ft8Shm->write_seq;
  int seq = *seqp;
  for (const Ft8Decode& d : list) {
    const QByteArray line = QString("%1 %2 %3 %4 %5 %6")
        .arg(st).arg(d.snr).arg(d.dt, 0, 'f', 1).arg(qint64(std::llround(d.rfHz)))
        .arg(d.mode.isEmpty() ? QStringLiteral("FT8") : d.mode).arg(d.msg).toLatin1();
    const int idx = ((seq % QMAPFT8_CAP) + QMAPFT8_CAP) % QMAPFT8_CAP;
    std::memset(m_ft8Shm->lines[idx], 0, QMAPFT8_LINE);
    std::memcpy(m_ft8Shm->lines[idx], line.constData(), std::min<size_t>(line.size(), QMAPFT8_LINE - 1));
    std::atomic_thread_fence(std::memory_order_release);
    *seqp = ++seq;
  }
}

Ft8Wide::~Ft8Wide()
{
  for (QProcess* p : m_running) { p->kill(); p->waitForFinished(500); }
  qDeleteAll(m_jobs);
  m_jobs.clear();
  QDir(m_tempRoot).removeRecursively();
}

QString Ft8Wide::jt9Executable() const
{
  const QString exe =
#ifdef Q_OS_WIN
    "jt9.exe";
#else
    "jt9";
#endif
  QStringList candidates;
  if (!m_cfg.jt9Path.isEmpty()) candidates << m_cfg.jt9Path;
  const QDir app(QCoreApplication::applicationDirPath());
  candidates << app.absoluteFilePath(exe);
  candidates << QDir(app.absolutePath() + "/..").absoluteFilePath(exe);   // build tree: build/qmap/qmap, build/jt9
  for (const QString& c : candidates)
    if (QFileInfo(c).isExecutable()) return QFileInfo(c).absoluteFilePath();
  return QString();
}

bool Ft8Wide::busy(WideMode mode) const
{
  for (const Job* j : m_jobs) if (j->mode == mode) return true;
  return false;
}

bool Ft8Wide::decodePeriod(WideMode mode, const float* iq, int nsamples, int rateHz,
                           double fcenterHz, const QDateTime& periodStart)
{
  const ModeParams mp = params(mode);
  const QString name = QString::fromLatin1(mp.name);
  if (busy(mode)) {
    int left = 0;
    for (const Job* j : m_jobs) if (j->mode == mode) left += j->pending.size() + j->running;
    say(QString("%1: period %2 skipped, previous decode still running (%3 slices left)")
                .arg(name).arg(stamp(periodStart)).arg(left));
    return false;
  }
  const QString jt9 = jt9Executable();
  if (jt9.isEmpty()) {
    say(name + ": jt9 not found (put jt9 next to WS-MAP, or set its path in FT8 settings)");
    return false;
  }
  if (rateHz <= 0) return false;
  const double nExact = mp.periodS * rateHz;
  const int N = int(std::lround(nExact));
  if (std::fabs(nExact - N) > 1e-6) return false;         // period must be a whole number of samples
  const int OUT_N = int(std::lround(mp.periodS * OUT_RATE));
  const double center = m_cfg.centerHz > 0.0 ? m_cfg.centerHz : fcenterHz;
  const double lo = center - m_cfg.widthHz / 2.0;
  const double hi = center + m_cfg.widthHz / 2.0;
  if (lo < fcenterHz - rateHz / 2.0 || hi > fcenterHz + rateHz / 2.0) {
    say(QString("%1: window %2-%3 MHz is outside the IQ stream (%4 MHz +/- %5 kHz)")
                .arg(name).arg(lo / 1e6, 0, 'f', 4).arg(hi / 1e6, 0, 'f', 4)
                .arg(fcenterHz / 1e6, 0, 'f', 4).arg(rateHz / 2000.0, 0, 'f', 0));
    return false;
  }

  Plans& P = plansFor(mode);
  P.ensure(N, OUT_N);
  const int have = std::max(0, std::min(nsamples, N));
  // The bridge sends the Linrad stream as (-1)^n * conj(x): spectrum
  // conjugated and rotated by half the sample rate, which is what QMAP's
  // symspec expects (backward FFT, bin 0 = bottom edge of the window, see
  // getcand2's fpk = i0*df).  Conjugating here and taking the forward FFT
  // yields the true spectrum with bin k at offset (k - N/2)*df from the
  // centre, upright and not time-reversed.  Without this the slices came
  // out mirrored about the centre and the audio inverted: nothing decoded
  // on air although the segment was 20-40 dB above the floor (2026-09-09).
  for (int i = 0; i < have; ++i) { P.in[i][0] = iq[2 * i]; P.in[i][1] = -iq[2 * i + 1]; }
  for (int i = have; i < N; ++i) { P.in[i][0] = 0.0f; P.in[i][1] = 0.0f; }
  fftwf_execute(P.fwd);

  // Slice grid.  Bin resolution is rate/N = 1/period Hz on both sides, so
  // bins copy 1:1 into the 12 kHz one-sided output spectrum (q65b's trick).
  const double df = double(rateHz) / N;
  const int nb = int(std::lround(m_cfg.sliceHz / df));
  const int nt = int(std::lround(200.0 / df));           // edge taper, 200 Hz
  const double step = m_cfg.sliceHz - m_cfg.overlapHz;
  Job* job = new Job;
  job->mode = mode;
  job->start = periodStart;
  const QString st = periodStart.addMSecs(500).toString("yyMMdd_HHmmss");
  int idx = 0;
  for (double f = lo; f < hi - 1.0; f += step, ++idx) {
    const double rel = f - fcenterHz;                   // Hz relative to the stream centre
    long k0 = std::lround(rel / df) + N / 2;            // bin N/2 = centre, 0 = bottom edge
    k0 %= N; if (k0 < 0) k0 += N;
    for (int j = 0; j <= OUT_N / 2; ++j) { P.ybins[j][0] = 0.0f; P.ybins[j][1] = 0.0f; }
    for (int j = 0; j < nb && j < OUT_N / 2; ++j) {
      const long k = (k0 + j) % N;
      float w = 1.0f;
      if (nt > 0 && j < nt) w = 0.5f * (1.0f - std::cos(float(M_PI) * j / nt));
      if (nt > 0 && j >= nb - nt) w = 0.5f * (1.0f - std::cos(float(M_PI) * (nb - 1 - j) / nt));
      P.ybins[j][0] = P.spec[k][0] * w;
      P.ybins[j][1] = P.spec[k][1] * w;
    }
    P.ybins[0][0] = 0.0f; P.ybins[0][1] = 0.0f;
    fftwf_execute(P.inv);
    Slice s;
    s.index = idx; s.rfLo = f;
    // One directory per mode and slice: jt9 writes its scratch files there,
    // and an FT4 half may decode while the FT8 period is still running.
    s.dir = QDir(m_tempRoot).absoluteFilePath(QString("%1_s%2").arg(name.toLower()).arg(idx, 2, 10, QChar('0')));
    QDir().mkpath(s.dir);
    s.wav = QDir(s.dir).absoluteFilePath(st + ".wav");
    writeWav(s.wav, P.y, OUT_N);
    job->pending << s;
  }
  job->nslices = job->pending.size();
  job->wall.start();
  m_jobs << job;
  while (startNext()) {}
  const int workers = m_cfg.workers > 0 ? m_cfg.workers : std::max(1, QThread::idealThreadCount());
  say(QString("%1: %2 slices of %3 kHz over %4-%5 MHz, depth %6, %7 workers")
              .arg(name).arg(job->nslices).arg(m_cfg.sliceHz / 1000.0, 0, 'f', 1)
              .arg(lo / 1e6, 0, 'f', 4).arg(hi / 1e6, 0, 'f', 4).arg(m_cfg.depth).arg(workers));
  return true;
}

// Start one worker on the oldest job that still has slices waiting.
// Returns false when nothing was started (no free worker or nothing pending).
bool Ft8Wide::startNext()
{
  const int workers = m_cfg.workers > 0 ? m_cfg.workers : std::max(1, QThread::idealThreadCount());
  if (m_running.size() >= workers) return false;
  Job* job = nullptr;
  for (Job* j : m_jobs) if (!j->pending.isEmpty()) { job = j; break; }
  if (!job) return false;
  Slice s = job->pending.takeFirst();
  QProcess* p = new QProcess(this);
  p->setWorkingDirectory(s.dir);
  p->setProcessChannelMode(QProcess::SeparateChannels);
  connect(p, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this, &Ft8Wide::workerFinished);
  m_sliceOf.insert(p, qMakePair(job, s));
  m_running << p;
  job->running++;
  p->setProperty("t0", QDateTime::currentMSecsSinceEpoch());
  // jt9 decodes only up to ~4.9 kHz audio; state the band explicitly anyway.
  p->start(jt9Executable(), QStringList()
           << params(job->mode).jt9Flag << "-d" << QString::number(m_cfg.depth) << "-m" << "1"
           << "-L" << "100" << "-H" << QString::number(int(m_cfg.sliceHz))
           << "-a" << s.dir << "-t" << s.dir << QFileInfo(s.wav).fileName());
  return true;
}

void Ft8Wide::workerFinished(int exitCode, QProcess::ExitStatus st)
{
  QProcess* p = qobject_cast<QProcess*>(sender());
  if (!p) return;
  const QPair<Job*, Slice> js = m_sliceOf.take(p);
  Job* job = js.first;
  const Slice s = js.second;
  m_running.removeAll(p);
  if (!job) { p->deleteLater(); return; }
  job->running--;
  job->cpuSec += (QDateTime::currentMSecsSinceEpoch() - p->property("t0").toLongLong()) / 1000.0;
  if (st != QProcess::NormalExit || exitCode != 0) ++job->failed;
  // jt9 line: "HHMMSS SNR DT FREQ ~  MESSAGE"  (FT4 lines carry "+" instead of "~")
  static const QRegularExpression re("^(\\d{6})\\s+(-?\\d+)\\s+(-?\\d+\\.\\d)\\s+(\\d+)\\s+[~+]\\s+(.*?)\\s*$");
  static const QRegularExpression annot("^(a[1-6]|q[0-9]|\\?)$");
  const QString out = QString::fromLatin1(p->readAllStandardOutput());
  for (const QString& line : out.split('\n')) {
    const auto m = re.match(line);
    if (!m.hasMatch()) continue;
    Ft8Decode d;
    d.utc = m.captured(1);
    d.snr = m.captured(2).toInt();
    d.dt = m.captured(3).toDouble();
    d.rfHz = s.rfLo + m.captured(4).toDouble();
    d.slice = s.index;
    d.mode = modeName(job->mode);
    // jt9 appends its decode annotation after the message: "a1".."a6" for
    // an a-priori decode, "?" for low confidence.  Strip it; drop the "?"
    // ones as WSJT-X does before spotting (DecodedText::isLowConfidence).
    QStringList words = m.captured(5).split(' ', SkipEmptyParts);
    bool lowConf = false;
    while (!words.isEmpty() && annot.match(words.last()).hasMatch()) {
      if (words.last() == "?") lowConf = true;
      words.removeLast();
    }
    if (lowConf || words.isEmpty()) { ++job->lowConf; continue; }
    d.msg = words.join(' ');
    // A standalone jt9 has no hash table, so every message with a hashed
    // call shows as "<...>"; those carry no station to report, and at depth
    // 3 they are also where the noise-fits land ("<...> 2YJWAQAXWUT").
    if (d.msg.contains("<...>")) { ++job->hashOnly; continue; }
    if (!plausible(d.msg, d.snr, job->mode)) { ++job->lowConf; continue; }
    job->collected << d;
  }
  p->deleteLater();
  QFile::remove(s.wav);
  startNext();
  if (job->pending.isEmpty() && job->running == 0) finishJob(job);
}

void Ft8Wide::finishJob(Job* job)
{
  // De-dupe across overlapping slices: same message within 3 Hz, keep best SNR.
  std::sort(job->collected.begin(), job->collected.end(),
            [](const Ft8Decode& a, const Ft8Decode& b) { return a.rfHz < b.rfHz; });
  QList<Ft8Decode> uniq;
  for (const Ft8Decode& d : job->collected) {
    bool dup = false;
    for (Ft8Decode& u : uniq) {
      if (u.msg == d.msg && std::fabs(u.rfHz - d.rfHz) <= 3.0) {
        if (d.snr > u.snr) u = d;
        dup = true; break;
      }
    }
    if (!dup) uniq << d;
  }
  const double wall = job->wall.elapsed() / 1000.0;
  const QString name = modeName(job->mode);
  m_jobs.removeAll(job);
  emit decodes(uniq, job->start, name, job->nslices, wall, job->cpuSec);
  say(QString("%1: %2 decodes (%3 raw) in %4 s wall, %5 worker-s%6")
              .arg(name).arg(uniq.size()).arg(job->collected.size()).arg(wall, 0, 'f', 1).arg(job->cpuSec, 0, 'f', 1)
              .arg((job->hashOnly ? QString(", %1 hash-only dropped").arg(job->hashOnly) : QString())
                   + (job->lowConf ? QString(", %1 low-confidence dropped").arg(job->lowConf) : QString())
                   + (job->failed ? QString(", %1 slice(s) failed").arg(job->failed) : QString())));
  delete job;
}
