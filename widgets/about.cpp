#include "about.h"

#include <QCoreApplication>
#include <QString>

#include "revision_utils.hpp"

#include "ui_about.h"

CAboutDlg::CAboutDlg(QWidget *parent) :
  QDialog(parent),
  ui(new Ui::CAboutDlg)
{
  ui->setupUi(this);

  ui->labelTxt->setText ("<h2>" + QString {"WS v" + QCoreApplication::applicationVersion ()
                                           + " " + revision ()}.simplified () + "</h2>"

    "<b>WS - Weak Signal Digital Mode Suite by DG2YCB et al.,</b><br/>"
    "<b>formerly known as WSJT-X Improved.</b><br/>"
    "<b>&copy; by Uwe Risse, DG2YCB, and Andreas Junge, N6NU.</b><br/>"
    "<br/>"
    "WS was derived from WSJT-X. <br/>"
    "WSJT-X implements a number of digital modes designed for <br/>"
    "weak-signal Amateur Radio communication.  <br/>"
    "&copy; 2001-2026 by Joe Taylor, K1JT, Bill Somerville, G4WJS, <br/>"
    "Steve Franke, K9AN, Nico Palermo, IV3NWV, <br/>"
    "Uwe Risse, DG2YCB, Brian Moran, N9ADG, <br/>"
    "and Roger Rehr, W3SZ.<br /><br/>"
    "We gratefully acknowledge contributions from AC6SL, AE4JY,<br/>"
    "AE5TC, DF2ET, DJ0OT, DJ7NT, DL3WDG, EA4AC, G4KLA, IW3RAB,<br/>"
    "JA7UDE, K3WYC, KA1GT, KA6MAL, KA9Q, KB1ZMX, KC1WIH, KD6EKQ,<br/>"
    "KG4IYS, KI7MT, KK1D, N6NU, ND0B, PY1ZRJ, PY2SDR, VE1SKY,<br/>"
    "VK3ACF, VK4BDJ, VK7MO, VR2UPU, VU3CER, W3DJS, W4TI,<br/>"
    "W4TV, and W9MDB.<br /><br />"
    "<b>WS and WSJT-X are licensed under the terms of Version 3</b><br/>"
    "<b>of the GNU General Public License (GPL) </b><br/>"
    "<br/>"
    "<a href=" TO_STRING__ (PROJECT_HOMEPAGE) ">"
    "<img src=\":/ws_icon_96.png\" /></a>"
    "<a href=\"https://www.gnu.org/licenses/gpl-3.0.txt\">"
    "<img src=\":/gpl-v3-logo.svg\" height=\"80\" /><br />"
    "https://www.gnu.org/licenses/gpl-3.0.txt</a>");
}

CAboutDlg::~CAboutDlg()
{
}
