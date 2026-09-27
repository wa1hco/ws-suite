#include "about.h"
#include "revision_utils.hpp"
#include "ui_about.h"

CAboutDlg::CAboutDlg(QWidget *parent) :
  QDialog(parent),
  ui(new Ui::CAboutDlg)
{
  ui->setupUi(this);
  ui->labelTxt->setText("<html><h2>" + QString {"EME65 v"
                + QCoreApplication::applicationVersion ()
                + " " + revision ()}.simplified () + "</h2>"

    "EME65 is primarily intended for EME communication.<br/>"
    "It implements a wideband polarization-matching receiver<br/>"
    "for the JT65 protocol, and also supports Q65-60.<br/>"
    "&copy; by Uwe Risse, DG2YCB, and Andreas Junge, N6NU<br/>"
    "<br />"
    "EME65 was derived from MAP65.<br/>"
    "Copyright 2001-2026 by Joe Taylor, K1JT.<br/>");
}

CAboutDlg::~CAboutDlg()
{
  delete ui;
}
