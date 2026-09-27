#include "devsetup.h"
#include "mainwindow.h"
#include <QTextStream>
#include <QDebug>
#include <cstdio>

//----------------------------------------------------------- DevSetup()
DevSetup::DevSetup(QWidget *parent) :	QDialog(parent)
{
  ui.setupUi(this);	//setup the dialog form
  m_restartSoundIn=false;
}

DevSetup::~DevSetup()
{
}

void DevSetup::initDlg()
{
  ui.myCallEntry->setText(m_myCall);
  ui.myGridEntry->setText(m_myGrid);
  ui.astroFont->setValue(m_astroFont);
  ui.myCallColor->addItems({"","red","green","cyan"});
  ui.myCallColor->setCurrentIndex(m_myCallColor);
  ui.saveDirEntry->setText(m_saveDir);
  ui.azelDirEntry->setText(m_azelDir);
  ui.fCalSpinBox->setValue(m_fCal);
  ui.faddEntry->setText(QString::number(m_fAdd,'f',3));
  ui.sbPort->setValue(m_udpPort);
  ui.sb_dB->setValue(m_dB);
  // Sample-rate mode: 0 = Auto (combo index 0); other values map to
  // 96/128/192/256 kHz combo indices 1/2/3/4. Anything outside the
  // known set falls back to Auto.
  switch (m_sampleRateModeOrZero) {
    case  96000: ui.cbSampleRateMode->setCurrentIndex(1); break;
    case 128000: ui.cbSampleRateMode->setCurrentIndex(2); break;
    case 192000: ui.cbSampleRateMode->setCurrentIndex(3); break;
    case 256000: ui.cbSampleRateMode->setCurrentIndex(4); break;
    default:     ui.cbSampleRateMode->setCurrentIndex(0); break;
  }
  ui.leLinradHost->setText(m_linradHost);
  ui.sbLinradTcpPort->setValue(m_linradTcpPort);
  ui.otherUrlBox->setText(m_otherUrl);
  ui.gbLiveCQ->setChecked(m_livecqEnabled);
  ui.cbSpotPSK->setChecked(m_spotPSK);
  ui.cbSpotPSKTcpIp->setChecked(m_spotPSKTcpIp);
  ui.cbLiveCQOfficial->setChecked(m_livecqOfficial);
  ui.cbLiveCQN6NU->setChecked(m_livecqN6NU);
  ui.cbLiveCQCustom->setChecked(m_livecqCustom);
  ui.cbLiveCQW3SZAppId->setCurrentText(m_livecqW3szAppId);
  ui.cbLiveCQW3SZAppId->setEnabled(m_livecqOfficial);
  // the program name for w3sz.com only matters while that destination is on
  connect(ui.cbLiveCQOfficial, &QCheckBox::toggled, ui.cbLiveCQW3SZAppId, &QComboBox::setEnabled);
  ui.sbInstanceId->setValue(m_instanceId);
  ui.leConfigName->setText(m_configName);
}

//------------------------------------------------------- accept()
void DevSetup::accept()
{
  // Called when OK button is clicked.
  // Check to see whether SoundInThread must be restarted,
  // and save user parameters.

  m_myCall=ui.myCallEntry->text();
  m_myGrid=ui.myGridEntry->text();
  m_astroFont=ui.astroFont->value();
  m_myCallColor=ui.myCallColor->currentIndex();
  m_saveDir=ui.saveDirEntry->text();
  m_azelDir=ui.azelDirEntry->text();
  m_fCal=ui.fCalSpinBox->value();
  m_fAdd=ui.faddEntry->text().toDouble();
  m_udpPort=ui.sbPort->value();
  m_dB=ui.sb_dB->value();
  switch (ui.cbSampleRateMode->currentIndex()) {
    case 1: m_sampleRateModeOrZero =  96000; break;
    case 2: m_sampleRateModeOrZero = 128000; break;
    case 3: m_sampleRateModeOrZero = 192000; break;
    case 4: m_sampleRateModeOrZero = 256000; break;
    default: m_sampleRateModeOrZero = 0; break;   // Auto
  }
  m_linradHost=ui.leLinradHost->text().trimmed();
  if(m_linradHost.isEmpty()) m_linradHost = "127.0.0.1";
  m_linradTcpPort=ui.sbLinradTcpPort->value();
  m_otherUrl=ui.otherUrlBox->text().trimmed();
  m_livecqEnabled=ui.gbLiveCQ->isChecked();
  m_spotPSK=ui.cbSpotPSK->isChecked();
  m_spotPSKTcpIp=ui.cbSpotPSKTcpIp->isChecked();
  m_livecqOfficial=ui.cbLiveCQOfficial->isChecked();
  m_livecqN6NU=ui.cbLiveCQN6NU->isChecked();
  m_livecqCustom=ui.cbLiveCQCustom->isChecked();
  m_livecqW3szAppId=ui.cbLiveCQW3SZAppId->currentText();
  m_instanceId=ui.sbInstanceId->value();
  m_configName=ui.leConfigName->text().trimmed();
  QDialog::accept();
}
