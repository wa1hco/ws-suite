#include "displaytext.h"

#include <vector>
#include <algorithm>

#include <QAudio>
#include <QAudioOutput>
#include <QSound>
#include <QDir>
#include <QCoreApplication>
#include <QTimer>
#include <QMouseEvent>
#include <QDateTime>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextBlock>
#include <QMenu>
#include <QAction>
#include <QListIterator>
#include <QRegularExpression>
#include <QScrollBar>

#include "Configuration.hpp"
#include "Decoder/decodedtext.h"
#include "Network/LotWUsers.hpp"
#include "models/DecodeHighlightingModel.hpp"
#include "logbook/logbook.h"
#include "logbook/USStates.hpp"
#include "Logger.hpp"

#include "qt_helpers.hpp"
#include "moc_displaytext.cpp"

bool play_CQ = false;
bool play_MyCall = false;
bool play_DXCC = false;
bool play_DXCCOB = false;
bool play_Grid = false;
bool play_GridOB = false;
bool play_Continent = false;
bool play_ContinentOB = false;
bool play_CQZ = false;
bool play_CQZOB = false;
bool play_ITUZ = false;
bool play_ITUZOB = false;
bool play_USState = false;
bool play_USStateOB = false;
bool muted = false;

using SpecOp = Configuration::SpecialOperatingActivity;

DisplayText::DisplayText(QWidget *parent)
  : QTextEdit(parent)
  , m_config {nullptr}
  , erase_action_ {new QAction {tr ("&Erase"), this}}
  , high_volume_ {false}
  , modified_vertical_scrollbar_max_ {-1}
{
  setReadOnly (true);
  setUndoRedoEnabled (false);
  viewport ()->setCursor (Qt::ArrowCursor);
  setWordWrapMode (QTextOption::NoWrap);

  // max lines to limit heap usage
  document ()->setMaximumBlockCount (5000);

  // context menu erase action
  setContextMenuPolicy (Qt::CustomContextMenu);
  connect (this, &DisplayText::customContextMenuRequested, [this] (QPoint const& position) {
      auto * menu = createStandardContextMenu (position);
      menu->addAction (erase_action_);
      menu->exec (mapToGlobal (position));
      delete menu;
    });
  connect (erase_action_, &QAction::triggered, this, &DisplayText::erase);
}

void DisplayText::erase ()
{
  clear ();
  Q_EMIT erased ();
}

void DisplayText::setContentFont(QFont const& font)
{
  char_font_ = font;
  selectAll ();
  auto cursor = textCursor ();
  cursor.beginEditBlock ();
  auto char_format = cursor.charFormat ();
  char_format.setFont (char_font_);
  cursor.mergeCharFormat (char_format);
  cursor.clearSelection ();
  cursor.movePosition (QTextCursor::End);

  // position so viewport scrolled to left
  cursor.movePosition (QTextCursor::Up);
  cursor.movePosition (QTextCursor::StartOfLine);
  cursor.endEditBlock ();

  if (!high_volume_ || !m_config || !m_config->decodes_from_top ())
    {
      setTextCursor (cursor);
      ensureCursorVisible ();
    }
}

void DisplayText::mouseDoubleClickEvent(QMouseEvent *e)
{
  Q_EMIT selectCallsign(e->modifiers ());
}

void DisplayText::insertLineSpacer(QString const& line)
{
  insertText (line, "#d3d3d3");
}

namespace
{
  using Highlight = DecodeHighlightingModel::Highlight;
  using highlight_types = std::vector<Highlight>;
  Highlight set_colours (Configuration const * config, QColor * bg, QColor * fg, highlight_types const& types)
  {
    Highlight result = Highlight::CQ;
    if (config)
      {
        QListIterator<DecodeHighlightingModel::HighlightInfo> it {config->decode_highlighting ().items ()};
        // iterate in reverse to honor priorities
        it.toBack ();
        while (it.hasPrevious ())
          {
            auto const& item = it.previous ();
            auto const& type = std::find (types.begin (), types.end (), item.type_);
            if (type != types.end () && item.enabled_)
              {
                if (item.background_.style () != Qt::NoBrush)
                  {
                    *bg = item.background_.color ();
                  }
                if (item.foreground_.style () != Qt::NoBrush)
                  {
                    *fg = item.foreground_.color ();
                  }
                result = item.type_;
              }
          }
      }
    return result;            // highest priority enabled highlighting
  }
}

void DisplayText::insertText(QString const& text, QColor bg, QColor fg
                             , QString const& call1, QString const& call2, QTextCursor::MoveOperation location
                             , QString const& roger, QColor rogerBg, QColor rogerFg
                             , QString const& appendix, QColor appendixBg, QColor appendixFg)
{
  auto cursor = textCursor ();
  cursor.movePosition (location);
  auto block_format = cursor.blockFormat ();
  auto format = cursor.blockCharFormat ();
  format.setFont (char_font_);
  block_format.clearBackground ();
  if (bg.isValid ())
    {
      block_format.setBackground (bg);
    }
  format.clearForeground ();
  if (fg.isValid ())
    {
      format.setForeground (fg);
    }
  if (cursor.position ())
    {
      cursor.insertBlock (block_format, format);
    }
  else
    {
      cursor.setBlockFormat (block_format);
      cursor.setBlockCharFormat (format);
    }

  int text_index {0};
  auto temp_format = format;
  if (call1.size ())
    {
      auto call_index = text.indexOf (call1);
      if (call_index != -1) // sanity check
        {
          auto pos = highlighted_calls_.find (call1);
          if (pos != highlighted_calls_.end ())
            {
              cursor.insertText(text.left (call_index));
              if (pos.value ().first.isValid ())
                {
                  temp_format.setBackground (pos.value ().first);
                }
              if (pos.value ().second.isValid ())
                {
                  temp_format.setForeground (pos.value ().second);
                }
              cursor.insertText(text.mid (call_index, call1.size ()), temp_format);
              text_index = call_index + call1.size ();
            }
        }
    }
  if (call2.size ())
    {
      auto call_index = text.indexOf (call2, text_index);
      if (call_index != -1) // sanity check
        {
          auto pos = highlighted_calls_.find (call2);
          if (pos != highlighted_calls_.end ())
            {
              temp_format = format;
              cursor.insertText(text.mid (text_index, call_index - text_index), format);
              if (pos.value ().second.isValid ())
                {
                  temp_format.setBackground (pos.value ().first);
                }
              if (pos.value ().second.isValid ())
                {
                  temp_format.setForeground (pos.value ().second);
                }
              cursor.insertText(text.mid (call_index, call2.size ()), temp_format);
              text_index = call_index + call2.size ();
            }
        }
    }

    // highlight the RRR/RR73/73 words in messages for us
    if (m_config && m_config->highlight_rogers() && roger.size () && (rogerBg.isValid () || rogerFg.isValid ()))
    {
      auto const roger_index = text.lastIndexOf (roger);
      if (roger_index >= text_index) // never rewind over a callsign already laid down
        {
          cursor.insertText(text.mid (text_index, roger_index - text_index), format);
          auto roger_format = format;
          if (rogerBg.isValid ()) roger_format.setBackground (rogerBg);
          if (rogerFg.isValid ()) roger_format.setForeground (rogerFg);
          cursor.insertText(text.mid (roger_index, roger.size ()), roger_format);
          text_index = roger_index + roger.size ();
        }
    }

    // highlight only the appended country name ("Highlight all messages")
    if (appendix.size () && (appendixBg.isValid () || appendixFg.isValid ()))
    {
      auto const appendix_index = text.lastIndexOf (appendix);
      if (appendix_index >= text_index)
        {
          cursor.insertText(text.mid (text_index, appendix_index - text_index), format);
          auto appendix_format = format;
          if (appendixBg.isValid ()) appendix_format.setBackground (appendixBg);
          if (appendixFg.isValid ()) appendix_format.setForeground (appendixFg);
          cursor.insertText(text.mid (appendix_index, appendix.size ()), appendix_format);
          text_index = appendix_index + appendix.size ();
        }
    }

  cursor.insertText(text.mid (text_index), format);

  // position so viewport scrolled to left
  cursor.movePosition (QTextCursor::StartOfLine);
  if (!high_volume_ || !m_config || !m_config->decodes_from_top ())
    {
      setTextCursor (cursor);
      ensureCursorVisible ();
    }
  document ()->setMaximumBlockCount (document ()->maximumBlockCount ());
}

void DisplayText::extend_vertical_scrollbar (int min, int max)
{
  if (high_volume_ && m_config && m_config->decodes_from_top ())
    {
      if (max && max != modified_vertical_scrollbar_max_)
        {
          setViewportMargins (0,4,0,0);  // ensure first line is readable
          auto vp_margins = viewportMargins ();
          // add enough to vertical scroll bar range to allow last
          // decode to just scroll of the top of the view port
          max += viewport ()->height () - vp_margins.top () - vp_margins.bottom ();
          modified_vertical_scrollbar_max_ = max;
        }
      verticalScrollBar ()->setRange (min, max);
    }
}

void DisplayText::new_period ()
{
  if (m_config->decodes_from_top ()) {
    document ()->setMaximumBlockCount (4800);
    document ()->setMaximumBlockCount (5000);
  }
  alertsTimer.stop ();
  disconnect (&alertsTimer, &QTimer::timeout, this, &DisplayText::AudioAlerts);
  if (m_config->alert_Enabled() && (m_config->alert_MyCall() || m_config->alert_DXCC() || m_config->alert_DXCCOB() ||
      m_config->alert_Grid() || m_config->alert_GridOB() || m_config->alert_Continent() || m_config->alert_ContinentOB() ||
      m_config->alert_CQZ() || m_config->alert_CQZOB() || m_config->alert_ITUZ() || m_config->alert_ITUZOB() ||
      m_config->alert_CQ() || m_config->alert_USState() || m_config->alert_USStateOB())) {
      connect (&alertsTimer, &QTimer::timeout, this, &DisplayText::AudioAlerts);
      alertsTimer.setSingleShot (true);
      alertsTimer.start (1000);
  }

  extend_vertical_scrollbar (verticalScrollBar ()->minimum (), verticalScrollBar ()->maximum ());
  if (high_volume_ && m_config && m_config->decodes_from_top () && !vertical_scroll_connection_)
    {
      vertical_scroll_connection_ = connect (verticalScrollBar (), &QScrollBar::rangeChanged
                                             , [this] (int min, int max) {
                                               extend_vertical_scrollbar (min, max );
                                             });
    }
  verticalScrollBar ()->setSliderPosition (verticalScrollBar ()->maximum ());
}

QString DisplayText::appendWorkedB4 (QString message, QString call, QString const& grid,
                                     QColor * bg, QColor * fg, LogBook const& logBook,
                                     QString const& currentBand, QString const& currentMode,
                                     QString extra)
{
  QString countryName;
  bool callB4;
  bool callB4onBand;
  bool countryB4;
  bool countryB4onBand;
  bool gridB4;
  bool gridB4onBand;
  bool continentB4;
  bool continentB4onBand;
  bool CQZoneB4;
  bool CQZoneB4onBand;
  bool ITUZoneB4;
  bool ITUZoneB4onBand;
  bool stateB4 = true;
  bool stateB4onBand = true;

  if(call.length()==2) {
    int i0=message.indexOf("CQ "+call);
    call=message.mid(i0+6,-1);
    i0=call.indexOf(" ");
    call=call.mid(0,i0);
  }
  if(call.length()<3) return message;
  if(!call.contains(QRegExp("[0-9]|[A-Z]"))) return message;

  auto const& looked_up = logBook.countries ()->lookup (call);
  logBook.match (call, currentMode, grid, looked_up, callB4, countryB4, gridB4, continentB4, CQZoneB4, ITUZoneB4,
                QString {}, &stateB4, nullptr);
  logBook.match (call, currentMode, grid, looked_up, callB4onBand, countryB4onBand, gridB4onBand,
                continentB4onBand, CQZoneB4onBand, ITUZoneB4onBand, currentBand, nullptr, &stateB4onBand);
  if(grid=="") {
    gridB4=true;
    gridB4onBand=true;
  }

  if(callB4onBand) m_points=0;

  message = message.trimmed ();

  highlight_types types;
  // no shortcuts here as some types may be disabled
  if (!countryB4) {
    types.push_back (Highlight::DXCC);
    if (m_config->alert_DXCC()) {
      if (!muted) play_DXCC = true;
    }
  }
  if(!countryB4onBand) {
    types.push_back (Highlight::DXCCBand);
    if (m_config->alert_DXCCOB()) {
       if (!muted) play_DXCCOB = true;
    }
  }
  if (!stateB4)
  {
    types.push_back (Highlight::USState);
    if (m_config->alert_USState()) {
      if (!muted) play_USState = true;
    }
  }
  if (!stateB4onBand)
  {
    types.push_back (Highlight::USStateBand);
    if (m_config->alert_USStateOB()) {
      if (!muted) play_USStateOB = true;
    }
  }
  if(!gridB4) {
    types.push_back (Highlight::Grid);
    if (m_config->alert_Grid()) {
      if (!muted) play_Grid = true;
    }
  }
  if(!gridB4onBand) {
    types.push_back (Highlight::GridBand);
    if (m_config->alert_GridOB()) {
      if (!muted) play_GridOB = true;
    }
  }
  if (!callB4) {
    types.push_back (Highlight::Call);
  }
  if(!callB4onBand) {
    types.push_back (Highlight::CallBand);
  }
  if (!continentB4) {
    types.push_back (Highlight::Continent);
    if (m_config->alert_Continent()) {
      if (!muted) play_Continent = true;
    }
  }
  if(!continentB4onBand) {
    types.push_back (Highlight::ContinentBand);
    if (m_config->alert_ContinentOB()) {
      if (!muted) play_ContinentOB = true;
    }
  }
  if (!CQZoneB4) {
    types.push_back (Highlight::CQZone);
    if (m_config->alert_CQZ()) {
      if (!muted) play_CQZ = true;
    }
  }
  if(!CQZoneB4onBand) {
    types.push_back (Highlight::CQZoneBand);
    if (m_config->alert_CQZOB()) {
      if (!muted) play_CQZOB = true;
    }
  }
  if (!ITUZoneB4) {
    types.push_back (Highlight::ITUZone);
    if (m_config->alert_ITUZ()) {
      if (!muted) play_ITUZ = true;
    }
  }
  if(!ITUZoneB4onBand) {
    types.push_back (Highlight::ITUZoneBand);
    if (m_config->alert_ITUZOB()) {
      if (!muted) play_ITUZOB = true;
    }
  }
  if (m_config && m_config->lotw_users ().user (call))
    {
      types.push_back (Highlight::LotW);
    }
  types.push_back (Highlight::CQ);
  auto top_highlight = set_colours (m_config, bg, fg, types);

  switch (top_highlight)
    {
    case Highlight::Continent:
    case Highlight::ContinentBand:
      extra += AD1CCty::continent (looked_up.continent);
      break;
    case Highlight::CQZone:
    case Highlight::CQZoneBand:
      extra += QString {"CQ Zone %1"}.arg (looked_up.CQ_zone);
      break;
    case Highlight::ITUZone:
    case Highlight::ITUZoneBand:
      extra += QString {"ITU Zone %1"}.arg (looked_up.ITU_zone);
      break;
    default:
      if (m_bPrincipalPrefix)
        {
          extra += looked_up.primary_prefix;
        }
      else
        {
          auto countryName = looked_up.entity_name;

          // do some obvious abbreviations
          countryName.replace ("Islands", "Is.");
          countryName.replace ("Island", "Is.");
          countryName.replace ("North ", "N. ");
          countryName.replace ("Northern ", "N. ");
          countryName.replace ("South ", "S. ");
          countryName.replace ("East ", "E. ");
          countryName.replace ("Eastern ", "E. ");
          countryName.replace ("West ", "W. ");
          countryName.replace ("Western ", "W. ");
          countryName.replace ("Central ", "C. ");
          countryName.replace (" and ", " & ");
          countryName.replace ("Republic", "Rep.");
          countryName.replace ("United States of America", "U.S.A.");
          countryName.replace ("United States", "U.S.A.");
          countryName.replace ("Fed. Rep. of ", "");
          countryName.replace ("French ", "Fr.");
          countryName.replace ("Asiatic", "AS");
          countryName.replace ("European", "EU");
          countryName.replace ("African", "AF");

          // assign WAE entities to the correct DXCC when "Include extra WAE entities" is not selected
          if (!(m_config->include_WAE_entities())) {
            countryName.replace ("Bear Is.", "Svalbard");
            countryName.replace ("Shetland Is.", "Scotland");
            countryName.replace ("AF Italy", "Italy");
            countryName.replace ("Sicily", "Italy");
            countryName.replace ("Vienna Intl Ctr", "Austria");
            countryName.replace ("AF Turkey", "Turkey");
            countryName.replace ("EU Turkey", "Turkey");
          }

          extra += countryName;
        }
    }
    m_CQPriority=DecodeHighlightingModel::highlight_name(top_highlight);
    m_CQPriorityType=top_highlight;
    m_CQPriorityValid=true;

    if(((m_points == 00) or (m_points == -1)) and m_bDisplayPoints) return message;
    return leftJustifyAppendage (message, extra);
}

QString DisplayText::leftJustifyAppendage (QString message, QString const& appendage0) const
{
  QString appendage=appendage0;
  if(m_bDisplayPoints and (m_points>0)) {
    appendage=" " + QString::number(m_points);
    if(m_points<10) appendage=" " + appendage;
  }
  if (appendage.size ())
    {
      // allow for seconds
      int padding {message.indexOf (" ") > 4 ? 2 : 0};

      // use a nbsp to save the start of appended text so we can find
      // it again later, align appended data at a fixed column if
      // there is space otherwise let it float to the right
      int space_count;
      space_count = (40 + m_config->align_steps() + padding - message.size ());
      if (space_count > 0) {
        message += QString {space_count, QChar {' '}};
      }
      message += QChar::Nbsp + appendage;
    }
  return message;
}

void DisplayText::displayDecodedText(DecodedText const& decodedText, QString const& myCall,
                                     QString const& mode,
                                     bool displayDXCCEntity, LogBook const& logBook,
                                     QString const& currentBand, bool ppfx, bool bCQonly,
                                     bool haveFSpread, float fSpread, bool bDisplayPoints,
                                     int points, QString distance, bool alertsMuted, QString const& QSOgrid)
{
  m_points=points;
  m_bDisplayPoints=bDisplayPoints;
  m_bPrincipalPrefix=ppfx;
  muted=alertsMuted;
  QColor bg;
  QColor fg;
  bool CQcall = false;
  auto is_73 = decodedText.messageWords().filter (QRegularExpression {"^(73|RR73)$"}).size();
  auto const roger_words = decodedText.messageWords().filter (QRegularExpression {"^(73|RR73|RRR)$"});
  if (decodedText.string ().contains (" CQ ")) {
    if (m_config->alert_CQ()) {
      if (!muted) play_CQ = true;
    }
  }
  if (decodedText.string ().contains (" CQ ")
      || decodedText.string ().contains (" CQDX ")
      || decodedText.string ().contains (" QRZ ")
      || (is_73 && (m_config->highlight_73 ())))
    {
      CQcall = true;
    }
  else
    {
      if (bCQonly) return;
    }
  auto message = decodedText.string();
  QString dxCall;
  QString dxGrid;
  decodedText.deCallAndGrid (/*out*/ dxCall, dxGrid);
  // Filter grid first (was done after the emit) so the overlay-click
  // handler in WideGraph/MainWindow gets either a valid 4/6-char grid
  // or an empty string (never a stray "RR73" etc.).
  QRegularExpression grid_regexp {"\\A(?![Rr]{2}73)[A-Ra-r]{2}[0-9]{2}([A-Xa-x]{2}){0,1}\\z"};
  if(!dxGrid.contains(grid_regexp)) dxGrid="";
  // Emit one signal per decode line for the waterfall callsign overlay.
  if (!dxCall.isEmpty() && !dxCall.startsWith('<')) {
    emit decodedCallsign(static_cast<double>(decodedText.frequencyOffset()),
                         dxCall,
                         CQcall,
                         static_cast<int>(decodedText.timeInSeconds()),dxGrid);
  }
  message = message.left (message.indexOf (QChar::Nbsp)).trimmed (); // strip appended info
  QString extra;
  QString state;    // NJ0A

  if (displayDXCCEntity && logBook.countries ()->lookup (dxCall).primary_prefix  == "K") {
    if (m_config->GridMap()) {
      if (CQcall || is_73 || m_config->GridMap()) {
        if (QSOgrid.length() >= 4) {
          state = logBook.countries ()->findState(QSOgrid);  // NJ0A method (grid-to-state mapping)
          if (state.size() == 2 && state != "**") {
            LOG_INFO(QString{"NJ0A method lookup: %1 (%2) -> %3"}.arg(dxCall).arg(QSOgrid).arg(state));
          } else {
            state = USStates::callsignToState(dxCall);      // AE5TC method as fall back (callsign database)
            if (!state.isEmpty()) LOG_INFO(QString{"%1 -> %2"}.arg(dxCall).arg(state));
          }
        } else {
          if (dxGrid.length() > 0) {
            state = logBook.countries ()->findState(dxGrid);  // NJ0A method (grid-to-state mapping)
            if (state.size() == 2 && state != "**") {
              LOG_INFO(QString{"NJ0A method lookup: %1 (%2) -> %3"}.arg(dxCall).arg(dxGrid).arg(state));
            } else {
              state = USStates::callsignToState(dxCall);      // AE5TC method as fall back (callsign database)
              if (!state.isEmpty()) LOG_INFO(QString{"%1 -> %2"}.arg(dxCall).arg(state));
            }
          } else {
            state = USStates::callsignToState(dxCall);        // use AE5TC method for messages without grid
            if (!state.isEmpty()) {
              LOG_INFO(QString{"%1 -> %2"}.arg(dxCall).arg(state));
            }  else {
              state.clear();
            }
          }
        }
        // Alert is handled by the highlighting system in appendWorkedB4
        // which properly respects "highlight by mode" setting
      }
    }
  }
  
  //NJ0A

  if (haveFSpread)
    {
      extra += QString {"%1"}.arg (fSpread, 5, 'f', fSpread < 0.95 ? 3 : 2) + QChar {' '};
    }
  auto ap_pos = message.lastIndexOf (QRegularExpression {R"((?:\?\s)?(?:a[0-9]|q[0-9][0-9*]?)$)"});
  if (ap_pos >= 0)
    {
      extra += message.mid (ap_pos) + QChar {' '};
      message = message.left (ap_pos).trimmed ();
    }
  m_CQPriority="";
  m_CQPriorityValid=false;
  bool const highlightLine = CQcall || (is_73 && m_config->highlight_73()) || (mode == "FT4" && m_config->highlight_73() && m_config->NCCC_Sprint()
      && (SpecOp::NA_VHF == m_config->special_op_id()) && decodedText.string().contains(" R "));
  // "Highlight all messages": other lines keep the normal colours (so dark
  // style works) and only their country name is highlighted
  QString appendix;
  QColor appendixBg;
  QColor appendixFg;
  if (highlightLine || m_config->highlight_all())
    {
      if (displayDXCCEntity)
        {
          // if enabled add the DXCC entity and B4 status to the end of the
          // preformated text line t1
          auto currentMode = mode;
          message = appendWorkedB4 (message, dxCall, dxGrid, &bg, &fg
                                    , logBook, currentBand, currentMode, extra);
          if (!highlightLine)
            {
              // the CQ colour is only the fallback, meaningless for a line without CQ
              if (m_CQPriorityValid && m_CQPriorityType != Highlight::CQ)
                {
                  auto const nbsp = message.lastIndexOf (QChar::Nbsp);
                  if (nbsp >= 0) appendix = message.mid (nbsp + 1).trimmed ();
                  appendixBg = bg;
                  appendixFg = fg;
                }
              bg = QColor {};
              fg = QColor {};
            }
        }
      else
        {
          message = leftJustifyAppendage (message, extra);
          highlight_types types {Highlight::CQ};
          if (m_config && m_config->lotw_users ().user (decodedText.CQersCall()))
            {
              types.push_back (Highlight::LotW);
            }
          set_colours (m_config, &bg, &fg, types);
        }
    }
  else
    {
      if (m_config->show_country_names() && mode != "FST4W")
        {
          auto const& looked_up = logBook.countries ()->lookup (dxCall);
          auto countryName = looked_up.entity_name;

          if (m_bPrincipalPrefix) {
              extra += looked_up.primary_prefix;
          } else {
              // do some obvious abbreviations
              countryName.replace ("Islands", "Is.");
              countryName.replace ("Island", "Is.");
              countryName.replace ("North ", "N. ");
              countryName.replace ("Northern ", "N. ");
              countryName.replace ("South ", "S. ");
              countryName.replace ("East ", "E. ");
              countryName.replace ("Eastern ", "E. ");
              countryName.replace ("West ", "W. ");
              countryName.replace ("Western ", "W. ");
              countryName.replace ("Central ", "C. ");
              countryName.replace (" and ", " & ");
              countryName.replace ("Republic", "Rep.");
              countryName.replace ("United States of America", "U.S.A.");
              countryName.replace ("United States", "U.S.A.");
              countryName.replace ("Fed. Rep. of ", "");
              countryName.replace ("French ", "Fr.");
              countryName.replace ("Asiatic", "AS");
              countryName.replace ("European", "EU");
              countryName.replace ("African", "AF");

              // assign WAE entities to the correct DXCC when "Include extra WAE entities" is not selected
              if (!(m_config->include_WAE_entities())) {
                countryName.replace ("Bear Is.", "Svalbard");
                countryName.replace ("Shetland Is.", "Scotland");
                countryName.replace ("AF Italy", "Italy");
                countryName.replace ("Sicily", "Italy");
                countryName.replace ("Vienna Intl Ctr", "Austria");
                countryName.replace ("AF Turkey", "Turkey");
                countryName.replace ("EU Turkey", "Turkey");
              }

              extra += countryName;
          }
          message = leftJustifyAppendage(message, extra);
        }
      else
        {
          message = leftJustifyAppendage (message, extra);
        }
    }

  if (myCall.size ())
    {
      QString regexp {"[ <]" + myCall + "[ >]"};
      if (Radio::is_compound_callsign (myCall))
        {
          regexp = "(?:" + regexp + "|[ <]" + Radio::base_callsign (myCall) + "[ >])";
        }
      if ((decodedText.clean_string () + " ").contains (QRegularExpression {regexp}))
        {
        QStringList tw;
        if (mode == "FT8" or mode == "FT4" or mode == "MSK144") {
          tw=decodedText.string().mid(24).split(" ",SkipEmptyParts);
        } else {
          tw=decodedText.string().mid(22).split(" ",SkipEmptyParts);
        }
          if ((tw.size () > 0 && tw[0].contains(Radio::base_callsign(myCall))) or decodedText.clean_string().contains("; " + myCall)) {
            highlight_types types {Highlight::MyCall};
            set_colours (m_config, &bg, &fg, types);
            if (m_config->alert_MyCall()) play_MyCall = true;
          } else {
            highlight_types types {Highlight::Tx};
            set_colours (m_config, &bg, &fg, types);
          }
        }
    }

  if (m_config->GridMap() && !m_bDisplayPoints) {
      if (ppfx) {               //NJ0A
          extra = " ";
      } else {
          extra = "      ";
      }
      message = leftJustifyAppendage(message, state);    //NJ0A
  }

  // display distance and azimuth
  if (distance.length() > 0) {
      if (m_config->align()) {
          if (!displayDXCCEntity) {
              message = leftJustifyAppendage (message, "[" + distance + "]");
          } else {
              QString space = " ";
              if (m_bPrincipalPrefix) {
                  if (message.length() < (49 + m_config->align_steps() + m_config->align_steps2())) {
                      message = leftJustifyAppendage ((message + (space.repeated(30))).left(48 + m_config->align_steps() + m_config->align_steps2()), "[" + distance + "]");
                  } else {
                      message = leftJustifyAppendage (message, " [" + distance + "]");
                  }
              } else {
                  if (message.length() < 59 + m_config->align_steps() + m_config->align_steps2()) {
                      message = leftJustifyAppendage ((message + (space.repeated(40))).left(59 + m_config->align_steps() + m_config->align_steps2()), "[" + distance + "]");
                  } else {
                      message = leftJustifyAppendage (message, "[" + distance + "]");
                  }
              }
          }
      } else {
         message = leftJustifyAppendage (message, "[" + distance + "]");
      }
  }

  // initialize audible alerts for MSK144 (still experimental)
  if(mode=="MSK144") {
    if ((m_config->alert_Enabled()) && ((m_config->alert_DXCC()) || (m_config->alert_DXCCOB()) || (m_config->alert_Grid()) ||
        (m_config->alert_GridOB()) || (m_config->alert_Continent()) || (m_config->alert_ContinentOB()) || (m_config->alert_CQZ()) ||
        (m_config->alert_CQZOB()) || (m_config->alert_ITUZ()) || (m_config->alert_ITUZOB()) || (m_config->alert_CQ()) ||
        (m_config->alert_USState()) || (m_config->alert_USStateOB()))) {
      alertsTimer.stop ();
      disconnect (&alertsTimer, &QTimer::timeout, this, &DisplayText::AudioAlerts);
      connect (&alertsTimer, &QTimer::timeout, this, &DisplayText::AudioAlerts);
      alertsTimer.setSingleShot (true);
      alertsTimer.start (1000);
    }
  }

  // highlight the RRR/RR73/73 words in messages for us
  QString roger;
  QColor roger_bg = QColor{0,255,0}; // green
  QColor roger_fg = QColor{0,0,0};   // black
  // a line highlighted as a whole (e.g. for my call) needs no country highlight
  if (bg.isValid () || fg.isValid ()) appendix.clear ();
  if (m_config && m_config->highlight_rogers() && roger_words.size() && !myCall.isEmpty()
      && decodedText.string ().contains (" " + Radio::base_callsign (myCall) + " ")) {
    roger = roger_words.last ();
    insertText (message.trimmed (), bg, fg, decodedText.call (), dxCall, QTextCursor::End, roger, roger_bg, roger_fg
                , appendix, appendixBg, appendixFg);
  } else {
    insertText (message.trimmed (), bg, fg, decodedText.call (), dxCall, QTextCursor::End, QString {}, QColor {}, QColor {}
                , appendix, appendixBg, appendixFg);
  }
}

void DisplayText::displayTransmittedText(QString text, QString modeTx, qint32 txFreq,
                                         bool bFastMode, double TRperiod,bool bSuperfox)
{
    QString t1=" @  ";
    if(modeTx=="FT4") t1=" +  ";
    if(modeTx=="FT2") t1=" *  ";
    if(modeTx.contains("FT8")) t1=" ~  ";
    if(modeTx=="JT4") t1=" $  ";
    if(modeTx=="Q65") t1=" :  ";
    if(modeTx=="JT65") t1=" #  ";
    if(modeTx=="MSK144") t1=" &  ";
    if(modeTx=="FST4") t1=" `  ";
    QString t2;
    t2 = t2.asprintf("%4d",txFreq);
    QString t;
    if(bFastMode or modeTx=="FT8" or modeTx=="FT4" or (TRperiod<60) or
       (modeTx=="Q65" and TRperiod==60)) {
      t = QDateTime::currentDateTimeUtc().toString("hhmmss") + \
        "  Tx      " + t2 + t1 + text;
    } else if(modeTx.mid(0,6)=="FT8fox") {
      t = QDateTime::currentDateTimeUtc().toString("hhmmss") + \
        " Tx" + modeTx.mid(7) + " " + text;
    } else {
      t = QDateTime::currentDateTimeUtc().toString("hhmm") + \
        "  Tx      " + t2 + t1 + text;
    }
    QColor bg;
    QColor fg;
    highlight_types types {Highlight::Tx};
    set_colours (m_config, &bg, &fg, types);
    if(bSuperfox and t.contains(";")) {
      int i0=t.indexOf(";");
      int i1=t.indexOf("<");
      int i2=t.indexOf(">");
      QString foxcall=t.mid(i1+1,i2-i1-1);
      t2=t.left(i0).replace(" RR73", " " + foxcall + " RR73");
      QString t3=t.left(24) + t.mid(i0+2,-1).remove("<").remove(">");
      insertText (t2, bg, fg);
      insertText (t3, bg, fg);
    } else {
      insertText (t, bg, fg);
    }
}

void DisplayText::displayQSY(QString text)
{
  QString t = QDateTime::currentDateTimeUtc().toString("hhmmss") + "            " + text;
  insertText (t, "hotpink");
}

void DisplayText::displayHoundToBeCalled(QString t, bool bAtTop, QColor bg, QColor fg)
{
  if (bAtTop)  t = t + "\n"; // need a newline when insertion at top
  insertText(t, bg, fg, "", "", bAtTop ? QTextCursor::Start : QTextCursor::End);
}

void DisplayText::setHighlightedHoundText(QString t) {
  QColor bg=QColor{255,255,255};
  QColor fg=QColor{0,0,0};
  highlight_types types{Highlight::Call};
  set_colours(m_config, &bg, &fg, types);
  // t is multiple lines of text, each line is a hound calling
  // iterate through each line and highlight the callsign
  auto lines = t.split(QChar('\n'), SkipEmptyParts);
  clear();
  foreach (auto line, lines)
  {
    auto fields = line.split(QChar(' '), SkipEmptyParts);
    insertText(line, bg, fg, fields.first(), QString{});
  }
}

namespace
{
  void update_selection (QTextCursor& cursor, QColor const& bg, QColor const& fg)
  {
    QTextCharFormat format {cursor.charFormat ()};
    if (bg.isValid ())
      {
        format.setBackground (bg);
      }
    else
      {
        format.clearBackground ();
      }
    if (fg.isValid ())
      {
        format.setForeground (fg);
      }
    else
      {
        format.clearForeground ();
      }
    cursor.mergeCharFormat (format);
  }

  void reset_selection (QTextCursor& cursor)
  {
    // restore previous text format, we rely on the text
    // char format at he start of the selection being the
    // old one which should be the case
    auto c2 = cursor;
    c2.setPosition (c2.selectionStart ());
    cursor.setCharFormat (c2.charFormat ());
  }
}

namespace
{
  QString get_timestamp (QTextCursor& cursor)
  {
    QString timestamp;
    if (cursor.movePosition (QTextCursor::PreviousCharacter)
        && cursor.movePosition (QTextCursor::StartOfLine)
        && cursor.movePosition (QTextCursor::EndOfWord, QTextCursor::KeepAnchor)
        && cursor.hasSelection ())
      {
        timestamp = cursor.selectedText ();
        cursor.movePosition (QTextCursor::StartOfLine);
      }
    return timestamp;
  }
}

void DisplayText::highlight_callsign (QString const& callsign, QColor const& bg,
                                      QColor const& fg, bool last_period_only)
{
  auto regexp = callsign;
  if (!callsign.size () || callsign == "" || callsign == " " || callsign == "0")
    {
      return;
    }
  if (callsign == "CLEARALL!")  // programmatic means of clearing all highlighting
  {
    highlighted_calls_.clear();
    return;
  }
  // allow for hashed callsigns and escape any regexp metacharacters
  QRegularExpression target {QString {"<?"}
                             + regexp.replace (QLatin1Char {'+'}, QLatin1String {"\\+"})
                                 .replace (QLatin1Char {'.'}, QLatin1String {"\\."})
                                 .replace (QLatin1Char {'?'}, QLatin1String {"\\?"})
                             + QString {">?"}
                             , QRegularExpression::DontCaptureOption};
  QTextCharFormat old_format {currentCharFormat ()};
  QTextCursor cursor {document ()};
  if (last_period_only)
    {
      // highlight each instance of the given callsign (word) in the
      // current period
      cursor.movePosition (QTextCursor::End);
      QTextCursor period_start {cursor};
      QTextCursor prior {cursor};
      auto period_timestamp = get_timestamp (period_start);
      while (period_timestamp.size () && period_timestamp == get_timestamp (prior))
        {
          period_start = prior;
        }
      cursor = period_start;
      while (!cursor.isNull ())
        {
          cursor = document ()->find (target, cursor, QTextDocument::FindWholeWords);
          if (!cursor.isNull () && cursor.hasSelection ())
            {
              if (bg.isValid () || fg.isValid ())
                {
                  update_selection (cursor, bg, fg);
                }
              else
                {
                  reset_selection (cursor);
                }
            }
        }
    }
  else
    {
      auto pos = highlighted_calls_.find (callsign);
      if (bg.isValid () || fg.isValid ())
        {
          auto colours = qMakePair (bg, fg);
          if (pos == highlighted_calls_.end ())
            {
              pos = highlighted_calls_.insert (callsign.toUpper (), colours);
            }
          else
            {
              pos.value () = colours; // update colours
            }
          while (!cursor.isNull ())
            {
              cursor = document ()->find (target, cursor, QTextDocument::FindWholeWords);
              if (!cursor.isNull () && cursor.hasSelection ())
                {
                  update_selection (cursor, bg, fg);
                }
            }
        }
      else
        {
          if (pos != highlighted_calls_.end ())
            {
              highlighted_calls_.erase (pos);
            }
          QTextCursor cursor {document ()};
          while (!cursor.isNull ())
            {
              cursor = document ()->find (target, cursor, QTextDocument::FindWholeWords);
              if (!cursor.isNull () && cursor.hasSelection ())
                {
                  reset_selection (cursor);
                }
            }
        }
    }
  setCurrentCharFormat (old_format);
}

void DisplayText::AudioAlerts()
{
#ifdef WIN32
  if(m_config->alert_Enabled()) {
        QAudioOutput info(QAudioDeviceInfo::defaultOutputDevice());
        QString audioPath = QCoreApplication::applicationDirPath() + "/sounds" + m_config->voicesPath() + "/";
        QAudioFormat format;
        format.setCodec("audio/pcm");
        format.setSampleRate (48000);
        format.setChannelCount (1);
        format.setSampleSize (16);
        format.setSampleType(QAudioFormat::SignedInt);
        QAudioOutput* audio;
        audio = new QAudioOutput(format, this);
        connect(audio, SIGNAL(stateChanged(QAudio::State)), this, SLOT(handleStateChanged(QAudio::State)));
#else
  if(m_config->alert_Enabled()) {
        QString audioPath = QCoreApplication::applicationDirPath() + "/sounds" + m_config->voicesPath() + "/";
#endif
        QFile *effect2 = new QFile(this);
        QFile *effect3 = new QFile(this);
        QFile *effect4 = new QFile(this);
        QFile *effect5 = new QFile(this);
        QFile *effect6 = new QFile(this);
        QFile *effect7 = new QFile(this);
        QFile *effect8 = new QFile(this);
        QFile *effect9 = new QFile(this);
        QFile *effect10 = new QFile(this);
        QFile *effect11 = new QFile(this);
        QFile *effect12 = new QFile(this);
        QFile *effect13 = new QFile(this);
        QFile *effect14 = new QFile(this);
        QFile *effect15 = new QFile(this);
        effect2->setFileName(QString("%1/%2").arg(audioPath, "MyCall.wav"));
        effect3->setFileName(QString("%1/%2").arg(audioPath, "DXCC.wav"));
        effect4->setFileName(QString("%1/%2").arg(audioPath, "DXCCOnBand.wav"));
        effect5->setFileName(QString("%1/%2").arg(audioPath, "Continent.wav"));
        effect6->setFileName(QString("%1/%2").arg(audioPath, "ContinentOnBand.wav"));
        effect7->setFileName(QString("%1/%2").arg(audioPath, "CQZone.wav"));
        effect8->setFileName(QString("%1/%2").arg(audioPath, "CQZoneOnBand.wav"));
        effect9->setFileName(QString("%1/%2").arg(audioPath, "ITUZone.wav"));
        effect10->setFileName(QString("%1/%2").arg(audioPath, "ITUZoneOnBand.wav"));
        effect11->setFileName(QString("%1/%2").arg(audioPath, "Grid.wav"));
        effect12->setFileName(QString("%1/%2").arg(audioPath, "GridOnBand.wav"));
        effect13->setFileName(QString("%1/%2").arg(audioPath, "CQ.wav"));
        effect14->setFileName(QString("%1/%2").arg(audioPath, "USState.wav"));
        effect15->setFileName(QString("%1/%2").arg(audioPath, "USStateOnBand.wav"));
        static int startIndex = 0;
        int nextStartIndex = startIndex +1;
        switch (startIndex) {
        case 0:
            if (play_MyCall) {
#ifdef WIN32
                effect2->open(QIODevice::ReadOnly);
                audio->start(effect2);
#else
                QSound::play(audioPath + "MyCall.wav");  // for Linux and macOS
#endif
                play_MyCall = false;
                alertsTimer.start (1000);
                startIndex = nextStartIndex;
                return;
            } else {
                nextStartIndex++;
            }
            Q_FALLTHROUGH();
        case 1:
            if (play_DXCC) {
#ifdef WIN32
                effect3->open(QIODevice::ReadOnly);
                audio->start(effect3);
#else
                QSound::play(audioPath + "DXCC.wav");  // for Linux and macOS
#endif
                play_DXCC = false;
                play_DXCCOB = false;
                alertsTimer.start (1200);
                startIndex = nextStartIndex;
                return;
            } else {
                nextStartIndex++;
            }
            Q_FALLTHROUGH();
        case 2:
            if (play_DXCCOB && !play_DXCC) {
#ifdef WIN32
                effect4->open(QIODevice::ReadOnly);
                audio->start(effect4);
#else
                QSound::play(audioPath + "DXCCOnBand.wav");  // for Linux and macOS
#endif
                play_DXCCOB = false;
                alertsTimer.start (1800);
                startIndex = nextStartIndex;
                return;
            } else {
                nextStartIndex++;
            }
            Q_FALLTHROUGH();
        case 3:
            if (play_Continent) {
#ifdef WIN32
                effect5->open(QIODevice::ReadOnly);
                audio->start(effect5);
#else
                QSound::play(audioPath + "Continent.wav");  // for Linux and macOS
#endif
                play_Continent = false;
                play_ContinentOB = false;
                play_GridOB = false;
                play_CQZOB = false;
                play_ITUZOB = false;
                alertsTimer.start (1000);
                startIndex = nextStartIndex;
                return;
            } else {
                nextStartIndex++;
            }
            Q_FALLTHROUGH();
        case 4:
            if (play_ContinentOB && !play_Continent) {
#ifdef WIN32
                effect6->open(QIODevice::ReadOnly);
                audio->start(effect6);
#else
                QSound::play(audioPath + "ContinentOnBand.wav");  // for Linux and macOS
#endif
                play_ContinentOB = false;
                play_GridOB = false;
                play_CQZOB = false;
                play_ITUZOB = false;
                alertsTimer.start (2000);
                startIndex = nextStartIndex;
                return;
            } else {
                nextStartIndex++;
            }
            Q_FALLTHROUGH();
        case 5:
            if (play_CQZ) {
#ifdef WIN32
                effect7->open(QIODevice::ReadOnly);
                audio->start(effect7);
#else
                QSound::play(audioPath + "CQZone.wav");  // for Linux and macOS
#endif
                play_CQZ = false;
                play_CQZOB = false;
                alertsTimer.start (1500);
                startIndex = nextStartIndex;
                return;
            } else {
                nextStartIndex++;
            }
            Q_FALLTHROUGH();
        case 6:
            if (play_CQZOB && !play_CQZ) {
#ifdef WIN32
                effect8->open(QIODevice::ReadOnly);
                audio->start(effect8);
#else
                QSound::play(audioPath + "CQZoneOnBand.wav");  // for Linux and macOS
#endif
                play_CQZOB = false;
                alertsTimer.start (1800);
                startIndex = nextStartIndex;
                return;
            } else {
                nextStartIndex++;
            }
            Q_FALLTHROUGH();
        case 7:
            if (play_ITUZ) {
#ifdef WIN32
                effect9->open(QIODevice::ReadOnly);
                audio->start(effect9);
#else
                QSound::play(audioPath + "ITUZone.wav");  // for Linux and macOS
#endif
                play_ITUZ = false;
                play_ITUZOB = false;
                play_GridOB = false;
                alertsTimer.start (1500);
                startIndex = nextStartIndex;
                return;
            } else {
                nextStartIndex++;
            }
            Q_FALLTHROUGH();
        case 8:
            if (play_ITUZOB && !(play_ITUZ)) {
#ifdef WIN32
                effect10->open(QIODevice::ReadOnly);
                audio->start(effect10);
#else
                QSound::play(audioPath + "ITUZoneOnBand.wav");  // for Linux and macOS
#endif
                play_ITUZOB = false;
                play_GridOB = false;
                alertsTimer.start (1900);
                startIndex = nextStartIndex;
                return;
            } else {
                nextStartIndex++;
            }
            Q_FALLTHROUGH();
        case 9:
            if (play_Grid) {
#ifdef WIN32
                effect11->open(QIODevice::ReadOnly);
                audio->start(effect11);
#else
                QSound::play(audioPath + "Grid.wav");  // for Linux and macOS
#endif
                play_Grid = false;
                play_GridOB = false;
                alertsTimer.start (1000);
                startIndex = nextStartIndex;
                return;
            } else {
                nextStartIndex++;
            }
            Q_FALLTHROUGH();
        case 10:
            if (play_GridOB && !play_Grid) {
#ifdef WIN32
                effect12->open(QIODevice::ReadOnly);
                audio->start(effect12);
#else
                QSound::play(audioPath + "GridOnBand.wav");  // for Linux and macOS
#endif
                play_GridOB = false;
                alertsTimer.start (1500);
                startIndex = nextStartIndex;
                return;
            } else {
                nextStartIndex++;
            }
            Q_FALLTHROUGH();
        case 11:
            if (play_CQ) {
#ifdef WIN32
                effect13->open(QIODevice::ReadOnly);
                audio->start(effect13);
#else
                QSound::play(audioPath + "CQ.wav");  // for Linux and macOS
#endif
                play_CQ = false;
                alertsTimer.start (1000);
                nextStartIndex++;
                return;
            } else {
                nextStartIndex++;
            }
            Q_FALLTHROUGH();
        case 12:
            if (play_USState) {
#ifdef WIN32
                effect14->open(QIODevice::ReadOnly);
                audio->start(effect14);
#else
                QSound::play(audioPath + "USState.wav");  // for Linux and macOS
#endif
                play_USState = false;
                play_USStateOB = false;
                alertsTimer.start (1500);
                startIndex = nextStartIndex;
                return;
            } else {
                nextStartIndex++;
            }
            Q_FALLTHROUGH();
        case 13:
            if (play_USStateOB) {
#ifdef WIN32
                effect15->open(QIODevice::ReadOnly);
                audio->start(effect15);
#else
                QSound::play(audioPath + "USStateOnBand.wav");  // for Linux and macOS
#endif
                play_USStateOB = false;
                alertsTimer.start (1500);
                startIndex = nextStartIndex;
                return;
            } else {
                nextStartIndex++;
            }
            Q_FALLTHROUGH();
        case 14:
            // stop any running alerts timer, clear temp data, and restart alerts timer
            alertsTimer.stop ();
#ifdef WIN32
            effect2->close();
            effect2->deleteLater();
            effect3->close();
            effect3->deleteLater();
            effect4->close();
            effect4->deleteLater();
            effect5->close();
            effect5->deleteLater();
            effect6->close();
            effect6->deleteLater();
            effect7->close();
            effect7->deleteLater();
            effect8->close();
            effect8->deleteLater();
            effect9->close();
            effect9->deleteLater();
            effect10->close();
            effect10->deleteLater();
            effect11->close();
            effect11->deleteLater();
            effect12->close();
            effect12->deleteLater();
            effect13->close();
            effect13->deleteLater();
            effect14->close();
            effect14->deleteLater();
            effect15->close();
            effect15->deleteLater();
            audio->deleteLater();
#endif
            alertsTimer.start (1250);
            startIndex = 0;
            return;
        }
  }
}
