#include "displaytext.h"
#include <QDebug>
#include <QMouseEvent>

DisplayText::DisplayText(QWidget *parent) :
    QTextBrowser(parent)
{
}

void DisplayText::mousePressEvent(QMouseEvent *e)
{
  // Base class first: QTextBrowser::mousePressEvent positions the text
  // cursor at the click point. Emitting selectCallsign before this would
  // read the *previous* cursor position instead of the one the operator
  // just clicked.
  QTextBrowser::mousePressEvent(e);
  if (e->button() == Qt::LeftButton) {
    bool ctrl = (e->modifiers() & 0x4000000);
    emit(selectCallsign(ctrl, false));
  }
}

void DisplayText::mouseDoubleClickEvent(QMouseEvent *e)
{
  QTextBrowser::mouseDoubleClickEvent(e);
  bool ctrl = (e->modifiers() & 0x4000000);
  emit(selectCallsign(ctrl, true));
}
