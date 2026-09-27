#include "bandmap.h"

#include <QComboBox>
#include <QDateTime>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QSettings>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

#include "SettingsGroup.hpp"

//----------------------------------------------------------- BandMap
BandMap::BandMap(QString const& settings_filename, QWidget* parent)
  : QDialog(parent),
    m_settings_filename(settings_filename)
{
  setWindowTitle("Band Map");
  setWindowFlags(Qt::WindowCloseButtonHint | Qt::WindowMinimizeButtonHint | Qt::WindowMaximizeButtonHint);
  // Being left open must never keep the app running by itself -- without
  // this, closing the main window while Band Map is still open leaves a
  // zombie qmap.exe with only this dialog visible, since Qt's default
  // quitOnLastWindowClosed policy counts it as a top-level window.
  setAttribute(Qt::WA_QuitOnClose, false);
  resize(260, 600);

  auto* topRow = new QHBoxLayout;
  topRow->addWidget(new QLabel("Retain:"));
  m_retentionCombo = new QComboBox(this);
  m_retentionCombo->addItem("10 min", 10);
  m_retentionCombo->addItem("15 min", 15);
  m_retentionCombo->addItem("30 min", 30);
  topRow->addWidget(m_retentionCombo);
  topRow->addStretch();
  m_countLabel = new QLabel("0 stations", this);
  topRow->addWidget(m_countLabel);

  m_plot = new BandMapPlot(this);
  connect(m_plot, &BandMapPlot::callsignClicked, this, &BandMap::callsignClicked);

  auto* mainLayout = new QVBoxLayout(this);
  mainLayout->addLayout(topRow);
  mainLayout->addWidget(m_plot, 1);

  loadSettings();
  int idx = m_retentionCombo->findData(m_retentionMinutes);
  if (idx < 0) idx = 0;
  m_retentionCombo->setCurrentIndex(idx);
  connect(m_retentionCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
          this, &BandMap::onRetentionChanged);

  m_ageTimer = new QTimer(this);
  connect(m_ageTimer, &QTimer::timeout, this, &BandMap::ageEntries);
  m_ageTimer->start(1000);

  // Reopen at the same position/size if it was left open last session.
  if (m_wasOpen) show();
}

BandMap::~BandMap()
{
  saveSettings();
}

void BandMap::loadSettings()
{
  QSettings settings(m_settings_filename, QSettings::IniFormat);
  SettingsGroup g(&settings, "BandMap");
  m_retentionMinutes = settings.value("RetentionMinutes", 10).toInt();
  if (m_retentionMinutes != 10 && m_retentionMinutes != 15 && m_retentionMinutes != 30) {
    m_retentionMinutes = 10;
  }
  restoreGeometry(settings.value("geometry").toByteArray());
  m_wasOpen = settings.value("WasOpen", false).toBool();
}

void BandMap::saveSettings()
{
  QSettings settings(m_settings_filename, QSettings::IniFormat);
  SettingsGroup g(&settings, "BandMap");
  settings.setValue("RetentionMinutes", m_retentionMinutes);
  settings.setValue("geometry", saveGeometry());
  settings.setValue("WasOpen", m_wasOpen);
}

void BandMap::showEvent(QShowEvent* e)
{
  m_wasOpen = true;
  QDialog::showEvent(e);
}

void BandMap::closeEvent(QCloseEvent* e)
{
  if (!m_shuttingDown) m_wasOpen = false;   // user's X, not the app quitting
  saveSettings();
  QDialog::closeEvent(e);
}

void BandMap::onRetentionChanged(int index)
{
  m_retentionMinutes = m_retentionCombo->itemData(index).toInt();
  saveSettings();
  ageEntries();   // immediate sweep with the new window
}

void BandMap::updateCountLabel()
{
  m_countLabel->setText(QString("%1 station%2").arg(m_entries.size())
                         .arg(m_entries.size() == 1 ? "" : "s"));
}

// Same two-pass dedupe/upsert logic as WideGraph::addDecodeLabel:
// refresh in place if the same call reappears near the same frequency,
// drop-and-re-add if it QSY'd, else append fresh. Deliberately a
// separate, longer-retention list from WideGraph's own m_decodeLabels.
void BandMap::addEntry(double freq_khz, const QString& callsign, bool is_cq,
                       const QString& grid, bool is_active, const QString& mode,
                       int even_period, double fsked_khz)
{
  if (callsign.isEmpty()) return;
  const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();

  auto it = m_entries.begin();
  while (it != m_entries.end()) {
    if (it->callsign == callsign) {
      if (std::abs(it->freq_khz - freq_khz) < 0.05) {
        it->last_seen_ms = now_ms;
        it->hits++;
        it->is_cq = is_cq;
        it->is_active = is_active;
        if (!grid.isEmpty()) it->grid = grid;
        if (!mode.isEmpty()) it->mode = mode;
        if (even_period >= 0) it->even_period = even_period;
        if (fsked_khz >= 0) it->fsked_khz = fsked_khz;
        m_plot->setEntries(m_entries, m_retentionMinutes);
        updateCountLabel();
        return;
      }
      it = m_entries.erase(it);
    } else {
      ++it;
    }
  }
  m_entries.append(DecodeLabel(freq_khz, callsign, now_ms, 1, is_cq, grid,
                                is_active, mode, even_period, fsked_khz));
  m_plot->setEntries(m_entries, m_retentionMinutes);
  updateCountLabel();
}

void BandMap::ageEntries()
{
  const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
  const qint64 maxAge = static_cast<qint64>(m_retentionMinutes) * 60 * 1000;
  bool changed = false;
  auto it = m_entries.begin();
  while (it != m_entries.end()) {
    if (now_ms - it->last_seen_ms > maxAge) {
      it = m_entries.erase(it);
      changed = true;
    } else {
      ++it;
    }
  }
  if (changed) {
    m_plot->setEntries(m_entries, m_retentionMinutes);
    updateCountLabel();
  }
}

void BandMap::clearEntries()
{
  if (m_entries.isEmpty()) return;
  m_entries.clear();
  m_plot->setEntries(m_entries, m_retentionMinutes);
  updateCountLabel();
}

//------------------------------------------------------- BandMapPlot
BandMapPlot::BandMapPlot(QWidget* parent)
  : QWidget(parent)
{
  setMinimumWidth(180);
  setMouseTracking(false);
}

void BandMapPlot::setEntries(const QList<DecodeLabel>& entries, int retentionMinutes)
{
  m_entries = entries;
  m_retentionMinutes = retentionMinutes;
  if (m_entries.isEmpty()) {
    m_minFreq = 0.0;
    m_maxFreq = 1.0;
  } else {
    double lo = m_entries.first().freq_khz;
    double hi = lo;
    for (const auto& e : m_entries) {
      lo = std::min(lo, e.freq_khz);
      hi = std::max(hi, e.freq_khz);
    }
    // Pad 5% of span (minimum 0.2 kHz) on each side so edge entries
    // don't paint flush against the top/bottom border.
    const double span = std::max(hi - lo, 0.2);
    const double pad = std::max(span * 0.05, 0.2);
    m_minFreq = lo - pad;
    m_maxFreq = hi + pad;
  }
  update();
}

int BandMapPlot::yForFreq(double freq_khz) const
{
  const double span = m_maxFreq - m_minFreq;
  if (span <= 0.0) return height() / 2;
  const double frac = (freq_khz - m_minFreq) / span;
  return static_cast<int>(frac * height());
}

void BandMapPlot::paintEvent(QPaintEvent*)
{
  QPainter p(this);
  p.fillRect(rect(), Qt::black);
  m_hitRects.clear();

  if (m_entries.isEmpty()) {
    p.setPen(Qt::gray);
    p.drawText(rect(), Qt::AlignCenter, "No decodes yet");
    return;
  }

  const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
  const qint64 maxAge = static_cast<qint64>(m_retentionMinutes) * 60 * 1000;

  // Sort by frequency so overlapping labels stack predictably (denser
  // clusters push later entries a few px down rather than overprinting).
  QList<DecodeLabel> sorted = m_entries;
  std::sort(sorted.begin(), sorted.end(), [](const DecodeLabel& a, const DecodeLabel& b) {
    return a.freq_khz < b.freq_khz;
  });

  QFont font = p.font();
  font.setPointSize(9);
  p.setFont(font);
  const int rowH = QFontMetrics(font).height() + 2;

  int lastY = -1000;
  for (const auto& e : sorted) {
    int y = yForFreq(e.freq_khz);
    if (y - lastY < rowH) y = lastY + rowH;   // de-overlap, push down
    lastY = y;
    if (y > height()) break;   // off the bottom; rest would be too

    // Fade older entries toward gray as they approach the retention
    // limit, so the map reads as "recency at a glance" not just a
    // flat list.
    const qint64 age = now_ms - e.last_seen_ms;
    const double freshness = maxAge > 0 ? 1.0 - std::min(1.0, double(age) / double(maxAge)) : 1.0;
    QColor color = e.is_cq ? QColor(80, 220, 80) : QColor(230, 230, 230);
    color = QColor::fromRgbF(
        color.redF()   * freshness + 0.35 * (1.0 - freshness),
        color.greenF() * freshness + 0.35 * (1.0 - freshness),
        color.blueF()  * freshness + 0.35 * (1.0 - freshness));
    p.setPen(color);

    // Small tick at the true frequency position, then the label at
    // the (possibly de-overlapped) row.
    const int trueY = yForFreq(e.freq_khz);
    p.drawLine(0, trueY, 6, trueY);

    QString label = QString("%1  %2").arg(e.freq_khz, 0, 'f', 1).arg(e.callsign);
    QRect labelRect(8, y - rowH / 2, width() - 10, rowH);
    p.drawText(labelRect, Qt::AlignVCenter | Qt::AlignLeft, label);
    m_hitRects.append(qMakePair(labelRect, e));
  }
}

bool BandMapPlot::hitTest(const QPoint& pt, DecodeLabel& out) const
{
  for (const auto& hr : m_hitRects) {
    if (hr.first.contains(pt)) {
      out = hr.second;
      return true;
    }
  }
  return false;
}

void BandMapPlot::mousePressEvent(QMouseEvent* event)
{
  if (event->button() != Qt::LeftButton) return;
  DecodeLabel hit;
  if (hitTest(event->pos(), hit)) {
    // Single click: transfer only (start_qso=false), matching
    // CPlotter's waterfall-overlay click semantics exactly.
    emit callsignClicked(hit.callsign, hit.freq_khz, hit.grid, hit.mode,
                         hit.even_period, hit.fsked_khz, false);
  }
}

void BandMapPlot::mouseDoubleClickEvent(QMouseEvent* event)
{
  if (event->button() != Qt::LeftButton) return;
  DecodeLabel hit;
  if (hitTest(event->pos(), hit)) {
    // Double click: transfer AND start the QSO (start_qso=true).
    emit callsignClicked(hit.callsign, hit.freq_khz, hit.grid, hit.mode,
                         hit.even_period, hit.fsked_khz, true);
  }
}
