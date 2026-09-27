#include "about.h"
#include "revision_utils.hpp"
#include "ui_about.h"

CAboutDlg::CAboutDlg(QWidget *parent) :
  QDialog(parent),
  ui(new Ui::CAboutDlg)
{
  ui->setupUi(this);
  ui->labelTxt->setText("<html><h2>" + QString {"WS-MAP v"
                + QCoreApplication::applicationVersion ()+ " "
                + revision ()}.simplified () + "</h2>"

    "WS-MAP is a wideband receiver connected to the WS main program.<br/>"
    "At present, it is intended primarily for EME communication,<br/>"
    "and supports the Q65 protocol (Q65-60 and Q65-30).<br/>"
    "Further use for terrestrial applications is planned, which will also<br/>"
    "include additional modes.<br/>"
    "&copy; by Uwe Risse, DG2YCB, and Andreas Junge, N6NU.<br/>"
    "<br />"
    "WS-MAP was derived from QMAP. <br/>"
    "Copyright 2001-2026 by Joe Taylor, K1JT.<br/>");
}

CAboutDlg::~CAboutDlg()
{
  delete ui;
}
