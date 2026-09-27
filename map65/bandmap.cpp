#include "bandmap.h"
#include <QSettings>
#include <QRegularExpression>
#include <QCloseEvent>
#include "ui_bandmap.h"
#include "qt_helpers.hpp"
#include "SettingsGroup.hpp"
#include <QDebug>

BandMap::BandMap (QString const& settings_filename, QWidget * parent)
  : QWidget {parent},
    ui {new Ui::BandMap},
    m_settings_filename {settings_filename}
{
  ui->setupUi (this);
  setWindowTitle ("Band Map");
  setWindowFlags (Qt::Dialog | Qt::WindowCloseButtonHint | Qt::WindowMinimizeButtonHint);
  QSettings settings {m_settings_filename, QSettings::IniFormat};
  SettingsGroup g {&settings, "MainWindow"}; // MainWindow group for
                                             // historical reasons
  // Default widened from 142 (too narrow to find a spot on the title bar
  // to drag by) but kept <=220 -- setText()'s own 2-column threshold --
  // so it doesn't default into the 2-column layout.
  setGeometry (settings.value ("BandMapGeom", QRect {280, 400, 200, 400}).toRect ());
  ui->bmTextBrowser->setStyleSheet(
                                   "QTextBrowser { background-color : #000066; color : red; }");
  connect (ui->bmTextBrowser, &DisplayText::selectCallsign, this, &BandMap::selectCallsign2);
}

// Click-to-work: setText() renders each row as tfreq(3 chars) + tspace(a
// SINGLE character at source position 4 -- position 3 is dropped
// entirely, never rendered) + tcall (source position 5+). So the actual
// rendered row is "NNN" + 1 pad char + "[*]CALLSIGN" -- position 4
// onward is the callsign, optionally "*"-prefixed (dupe marker) or
// preceded by the invisible-dot background placeholder character.
void BandMap::selectCallsign2(bool ctrl, bool isDoubleClick)
{
  Q_UNUSED(ctrl);
  QString t = ui->bmTextBrowser->toPlainText();
  // Each row is built as one <br> inside a single HTML paragraph (not a
  // separate block), so Qt represents that line break as U+2028 (LINE
  // SEPARATOR) in toPlainText(), not '\n' -- only true paragraph breaks
  // become '\n'. Normalize both to '\n' (one-for-one, so cursor-position
  // offsets stay aligned) or every row boundary search below silently
  // misses and swallows the rest of the column.
  t.replace(QChar(0x2028), QChar('\n'));
  int i = ui->bmTextBrowser->textCursor().position();
  // Search strictly BEFORE i, not at-or-before: double-click's default
  // word-selection can leave the cursor positioned exactly ON a row
  // separator character (not just adjacent to one). Searching at-or-before
  // AND at-or-after the same index both land on that same separator,
  // producing i0 > i1 -- and QString::mid with a resulting negative
  // length silently means "everything to the end of the string", which
  // is exactly how a double-click ended up grabbing every row below it.
  int i0 = (i > 0) ? (t.lastIndexOf("\n", i - 1) + 1) : 0;
  int i1 = t.indexOf("\n", i);
  if (i1 < 0) i1 = t.length();
  QString line = t.mid(i0, i1 - i0);
  // Table-cell boundaries (start of the first column, the transition into
  // the empty spacer column between the two data columns, and the very
  // end of the document) don't reliably behave like plain '\n'/U+2028
  // breaks in QTextDocument's linear text-position model -- the row
  // boundary search above can occasionally glue multiple rows together
  // at those exact edges. A genuine single row is always short and never
  // contains an embedded break; bail out rather than ever emit a
  // multi-row blob as a "callsign".
  if (line.length() > 20 || line.contains('\n')) return;
  if (line.length() <= 4) return;
  QString hiscall = line.mid(4).trimmed();
  hiscall.remove(QRegularExpression("^[.*\\s]+"));
  if (hiscall.isEmpty()) return;
  emit callsignClicked(hiscall, isDoubleClick);
}

BandMap::~BandMap ()
{
  delete ui;
}

void BandMap::closeEvent(QCloseEvent* event)
{
  QSettings settings {m_settings_filename, QSettings::IniFormat};
  SettingsGroup g {&settings, "MainWindow"};
  settings.setValue ("BandMapGeom", geometry ());
  settings.sync();
  QWidget::closeEvent(event);
}

void BandMap::setText(QString t)
{
  m_bandMapText=t;
  int w=ui->bmTextBrowser->size().width();
  int ncols=1;
  if(w>220) ncols=2;
  QString s="QTextBrowser{background-color: "+m_colorBackground+"}";
  ui->bmTextBrowser->setStyleSheet(s);
  QString t0="<html style=\" font-family:'Courier New';"
      "font-size:9pt; background-color:#000066\">"
      "<table border=0 cellspacing=7><tr><td>\n";
  QString tfreq,tspace,tcall;
  QString s0,s1,s2,s3,bg;
  const QString weight = m_boldText ? ";font-weight:bold" : "";
  bg="<span style=color:"+m_colorBackground+";>.</span>";
  s0="<span style=color:"+m_color0+weight+";>";
  s1="<span style=color:"+m_color1+weight+";>";
  s2="<span style=color:"+m_color2+weight+";>";
  s3="<span style=color:"+m_color3+weight+";>";

  ui->bmTextBrowser->clear();
  QStringList lines = t.split( "\n", SkipEmptyParts );
  int nrows=(lines.length()+ncols-1)/ncols;

  for(int i=0; i<nrows; i++) {
    tfreq=lines[i].mid(0,3);
    tspace=lines[i].mid(4,1);
    if(tspace==" ") tspace=bg;
    tcall=lines[i].mid(5).section(" ", 0, 0);
    int n=lines[i].mid(13,1).toInt();
    if(n==0) t0 += s0;
    if(n==1) t0 += s1;
    if(n==2) t0 += s2;
    if(n>=3) t0 += s3;
    t0 += (tfreq + tspace + tcall + "</span><br>\n");
  }

  if(ncols==2) {                                  //2-column display
    t0 += "<td><br><td>\n";
    for(int i=nrows; i<lines.length(); i++) {
      tfreq=lines[i].mid(0,3);
      tspace=lines[i].mid(4,1);
      if(tspace=="  ") tspace=bg;
      tcall=lines[i].mid(5).section(" ", 0, 0);
      int n=lines[i].mid(13,1).toInt();
      if(n==0) t0 += s0;
      if(n==1) t0 += s1;
      if(n==2) t0 += s2;
      if(n>=3) t0 += s3;
      t0 += (tfreq + tspace + tcall + "</span><br>\n");
    }
    if(2*nrows>lines.length()) t0 += (s0 + "</span><br>\n");
  }
  ui->bmTextBrowser->setHtml(t0);
}

void BandMap::resizeEvent(QResizeEvent* )
{
  setText(m_bandMapText);
}

void BandMap::setColors(QString t, bool bold)
{
  m_colorBackground = "#"+t.mid(0,6);
  m_color0 = "#"+t.mid(6,6);
  m_color1 = "#"+t.mid(12,6);
  m_color2 = "#"+t.mid(18,6);
  m_color3 = "#"+t.mid(24,6);
  m_boldText = bold;
  setText(m_bandMapText);
}
