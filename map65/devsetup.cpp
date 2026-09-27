#include "devsetup.h"
#include "mainwindow.h"
#include <QTextStream>
#include <QDebug>
#include <cstdio>
#include <portaudio.h>
#include <vector>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QStringList>
#include <QSerialPortInfo>
#include <QRegularExpression>

#if !defined(Q_OS_WIN)
extern "C" {
    void ptt_set_override(const char *path);
}
#endif

static QStringList enumeratePorts()
{
    QStringList result;

    QList<QSerialPortInfo> ports = QSerialPortInfo::availablePorts();

    for (auto const& p : ports)
    {
        if (!p.portName().contains("NULL"))
        {
            QString loc = p.systemLocation();
            loc.remove(QRegularExpression{R"(^\\\\\.\\)"});
            result << loc;
        }
    }

    std::sort(result.begin(), result.end(), [](QString const& a, QString const& b) {
        QRegularExpression re{"(\\d+)$"};
        auto ma = re.match(a);
        auto mb = re.match(b);

        if (ma.hasMatch() && mb.hasMatch())
            return ma.captured(1).toInt() < mb.captured(1).toInt();

        return a < b;
    });

    return result;
}

#define MAXDEVICES 1024 // was 200

//----------------------------------------------------------- DevSetup()
DevSetup::DevSetup(MainWindow *parent)
    : QDialog(parent),
      mw(parent)
{
  ui.setupUi(this);	//setup the dialog form
  m_restartSoundIn=false;
  m_restartSoundOut=false;

  // One-time item setup; current selection is restored per-open in initDlg().
  // Signals blocked: QComboBox::addItem() on an initially-empty combo
  // auto-selects the first item added and fires currentIndexChanged(0),
  // which -- since setupUi() already wired up the on_..._currentIndexChanged
  // auto-connect above -- would otherwise call applyColorPreset("Classic")
  // right here in the constructor, silently resetting every persisted
  // color value back to Classic before initDlg() ever runs.
  ui.colorPresetComboBox->blockSignals(true);
  ui.colorPresetComboBox->addItem("Classic");
  ui.colorPresetComboBox->addItem("Black on white");
  ui.colorPresetComboBox->addItem("White on Blue");
  ui.colorPresetComboBox->addItem("Custom");
  ui.colorPresetComboBox->blockSignals(false);
}

DevSetup::~DevSetup()
{
}

void DevSetup::initDlg()
{
  int k,id;
  
  // Use heap-allocated vectors instead of stack arrays
  std::vector<int> minChan(MAXDEVICES);
  std::vector<int> maxChan(MAXDEVICES);
  std::vector<int> minSpeed(MAXDEVICES);
  std::vector<int> maxSpeed(MAXDEVICES);
  
  // Use a unique pointer or vector for the 2D char array
  struct DeviceName {
    char name[50];
  };
  std::vector<DeviceName> hostAPI_DeviceName(MAXDEVICES);

  char s[256];
  int numDevices = Pa_GetDeviceCount();
  
  if (numDevices > MAXDEVICES) {
      numDevices = MAXDEVICES;
  }

  // Pass the pointers to the data inside the vectors
getDev(&numDevices,
       reinterpret_cast<char (*)[50]>(hostAPI_DeviceName.data()),
       minChan.data(), maxChan.data(), minSpeed.data(), maxSpeed.data());
  
k = 0;
for (id = 0; id < numDevices; id++) {

    if (!(g_sampleRate >= minSpeed[id] && g_sampleRate <= maxSpeed[id]))
        continue;


#ifdef _WIN32
    if (!QString(hostAPI_DeviceName[id].name).contains("MME"))
        continue;
#endif

    // Now safe to add to list
    m_inDevList[k] = id;

    snprintf(s, sizeof(s), "%2d   %d  %-49.49s",
             id, maxChan[id], hostAPI_DeviceName[id].name);

    ui.comboBoxSndIn->addItem(QString(s));
    k++;
  }

  const PaDeviceInfo *pdi;
  int nchout;
  char p2[256];

  k=0;
  for (id = 0; id < numDevices; id++) {

    pdi = Pa_GetDeviceInfo(id);
    if (!pdi) continue;

    nchout = pdi->maxOutputChannels;
    if (nchout < 1)
        continue;

    QString devName = QString(pdi->name);

#ifdef __linux__
    QString lower = devName.toLower();

    // Only allow plug/resampling devices for TX
    bool isPlug =
        lower.contains("default")  ||
        lower.contains("dmix")     ||
        lower.contains("pulse")    ||
        lower.contains("pipewire") ||
        !lower.contains("hw:");    // reject raw hw: devices

    if (!isPlug)
        continue;
#endif

    const char* api = Pa_GetHostApiInfo(pdi->hostApi)->name;

    // Skip WASAPI and WDM-KS for TX (Windows only)
    if (strstr(api, "WASAPI") || strstr(api,"WDM-KS"))
        continue;

    // Now safe to add to list
    m_outDevList[k++] = id;

    // Determine label
    const char* p1 = "";
    if (strstr(api, "MME"))     p1 = "MME";
    if (strstr(api, "Direct"))  p1 = "DirectX";
    if (strstr(api, "ASIO"))    p1 = "ASIO";
    if (strstr(api, "ALSA"))    p1 = "ALSA";

    snprintf(p2, sizeof(p2), "%2d   %-8.8s  %-39.39s",
             id, p1, pdi->name);

    ui.comboBoxSndOut->addItem(QString(p2));
  }
  ui.myCallEntry->setText(mw->m_myCall);
  ui.myGridEntry->setText(mw->m_myGrid);
  ui.idIntSpinBox->setValue(mw->m_idInt);

  ui.pttComboBox->clear();
  ui.pttComboBox->addItem("NONE");

  // unified cross-platform enumeration
  QStringList ports = enumeratePorts();
  for (auto const& p : ports)
      ui.pttComboBox->addItem(p);

  // restore saved selection
  QString saved = mw->m_pttPath;
  int idx = ui.pttComboBox->findText(saved);
  if (idx >= 0)
      ui.pttComboBox->setCurrentIndex(idx);

  // backend override (Linux/macOS only)
  #if !defined(Q_OS_WIN)
  if (ui.pttComboBox->currentText() != "NONE")
      ptt_set_override(ui.pttComboBox->currentText().toUtf8().constData());
  else
      ptt_set_override(nullptr);
  #endif
  
  if      (mw->m_fs96000 == 2)
      oldSampleRate = 192000;
  else if (mw->m_fs96000 == 1)
      oldSampleRate = 96000;
  else if (mw->m_fs96000 == 0)
      oldSampleRate = 95238;
  else oldSampleRate = 96000;

  ui.astroFont->setValue(mw->m_astroFont);
  ui.cbXpol->setChecked(mw->m_xpol);
  ui.rbAntennaX->setChecked(mw->m_xpolx);
  ui.saveDirEntry->setText(mw->m_saveDir);
  ui.azelDirEntry->setText(mw->m_azelDir);
  ui.call3PathEntry->setText(mw->m_call3PathUser);
  if (ui.call3PathEntry->text().trimmed().isEmpty()) ui.call3PathEntry->setText(mw->m_dataDir + "/CALL3.TXT");
  ui.editorEntry->setText(mw->m_editorCommand);
  ui.dxccEntry->setText(mw->m_dxccPfx);
  ui.timeoutSpinBox->setValue(mw->m_timeout);
  ui.dPhiSpinBox->setValue(mw->m_dPhi);
  ui.fCalSpinBox->setValue(mw->m_fCal);
  ui.faddEntry->setText(QString::number(mw->m_fAdd,'f',3));
  ui.networkRadioButton->setChecked(mw->m_network);
  ui.soundCardRadioButton->setChecked(!mw->m_network);
  ui.rb192000->setChecked(mw->m_fs96000 == 2);
  ui.rb96000->setChecked(mw->m_fs96000 == 1);
  ui.rb95238->setChecked(mw->m_fs96000 == 0);
  ui.rbIQXT->setChecked(mw->m_bIQxt);
  ui.rbSi570->setChecked(!mw->m_bIQxt);
  ui.mult570TxSpinBox->setEnabled(mw->m_bIQxt);
  ui.comboBoxSndIn->setEnabled(!mw->m_network);
  ui.comboBoxSndIn->setCurrentIndex(mw->m_nDevIn);
  ui.comboBoxSndOut->setCurrentIndex(mw->m_nDevOut);
  ui.sbPort->setValue(mw->m_udpPort);
  ui.cbIQswap->setChecked(mw->m_IQswap);
  ui.cbInitIQplus->setChecked(mw->m_initIQplus);
  ui.sb_dB->setValue(mw->m_dB);
  ui.mult570SpinBox->setValue(mw->m_mult570);
  ui.mult570TxSpinBox->setValue(mw->m_mult570Tx);
  ui.cal570SpinBox->setValue(mw->m_cal570);
  ui.sbTxOffset->setValue(mw->m_TxOffset);
  ::sscanf (mw->m_colors.toLatin1(),"%2x%2x%2x%2x%2x%2x%2x%2x%2x%2x%2x%2x%2x%2x%2x",
            &r,&g,&b,&r0,&g0,&b0,&r1,&g1,&b1,&r2,&g2,&b2,&r3,&g3,&b3);
  updateColorLabels();
  // Restoring from mw->m_colors below, not applying a preset -- suppress
  // the individual valueChanged handlers' own "switch to Custom" reaction
  // while these 15 values are set, then restore the preset dropdown to
  // whatever was actually persisted (blocked so it doesn't re-fire and
  // overwrite the just-restored values with hardcoded preset numbers).
  m_applyingPreset = true;
  ui.sbBackgroundRed->setValue(r);
  ui.sbBackgroundGreen->setValue(g);
  ui.sbBackgroundBlue->setValue(b);
  ui.sbRed0->setValue(r0);
  ui.sbRed1->setValue(r1);
  ui.sbRed2->setValue(r2);
  ui.sbRed3->setValue(r3);
  ui.sbGreen0->setValue(g0);
  ui.sbGreen1->setValue(g1);
  ui.sbGreen2->setValue(g2);
  ui.sbGreen3->setValue(g3);
  ui.sbBlue0->setValue(b0);
  ui.sbBlue1->setValue(b1);
  ui.sbBlue2->setValue(b2);
  ui.sbBlue3->setValue(b3);
  m_applyingPreset = false;

  ui.colorPresetComboBox->blockSignals(true);
  int presetIdx = ui.colorPresetComboBox->findText(mw->m_colorPreset);
  if (presetIdx < 0) presetIdx = ui.colorPresetComboBox->findText("Classic");
  ui.colorPresetComboBox->setCurrentIndex(presetIdx);
  ui.colorPresetComboBox->blockSignals(false);

  mw->m_paInDevice=m_inDevList[mw->m_nDevIn];
  mw->m_paOutDevice=m_outDevList[mw->m_nDevOut];

  ui.otherUrlBox->setText(mw->m_otherUrl);
  ui.LiveCQ_groupBox->setChecked(mw->m_livecqEnabled);
  ui.cbLiveCQOfficial->setChecked(mw->m_livecqOfficial);
  ui.cbLiveCQN6NU->setChecked(mw->m_livecqN6NU);
  ui.cbLiveCQCustom->setChecked(mw->m_livecqCustom);
  ui.cbLiveCQW3SZAppId->setCurrentText(mw->m_livecqW3szAppId);
  ui.cbLiveCQW3SZAppId->setEnabled(mw->m_livecqOfficial);
  // the program name for w3sz.com only matters while that destination is on
  connect(ui.cbLiveCQOfficial, &QCheckBox::toggled, ui.cbLiveCQW3SZAppId, &QComboBox::setEnabled);
  
  ui.pskBox->setChecked(mw->m_spot_to_psk_reporter);
  ui.pskReporterTcpIpBox->setChecked(mw->m_psk_reporter_tcpip);
}

//------------------------------------------------------- accept()
// "..." next to the CALL3.TXT field: pick an existing file, or type a new
// one -- QFileDialog::getSaveFileName allows naming a not-yet-existing
// CALL3.TXT while still constraining the directory to one that exists.
void DevSetup::on_call3BrowseButton_clicked()
{
  QString start = ui.call3PathEntry->text().trimmed();
  if (start.isEmpty()) start = mw->m_dataDir + "/CALL3.TXT";
  const QString f = QFileDialog::getSaveFileName(this, "Select CALL3.TXT",
                                                 start, "CALL3 files (*.TXT *.txt);;All files (*)",
                                                 nullptr, QFileDialog::DontConfirmOverwrite);
  if (!f.isEmpty()) ui.call3PathEntry->setText(f);
}

void DevSetup::accept()
{
  // Called when OK button is clicked.
  // Check to see whether SoundInThread must be restarted,
  // and save user parameters.

  int newSampleRate = ui.rb192000->isChecked() ? 192000 :
      ui.rb96000->isChecked()  ? 96000  :
      95238;

  emit sampleRateChanged(newSampleRate);

  bool restartNeeded = false;

  if (oldSampleRate == 192000 && newSampleRate != 192000)
    restartNeeded = true;

  if (oldSampleRate != 192000 && newSampleRate == 192000)
    restartNeeded = true;

  if(mw->m_network!=ui.networkRadioButton->isChecked() or
     mw->m_nDevIn!=ui.comboBoxSndIn->currentIndex() or
     mw->m_paInDevice!=m_inDevList[mw->m_nDevIn] or
     mw->m_xpol!=ui.cbXpol->isChecked() or
     mw->m_udpPort!=ui.sbPort->value()) m_restartSoundIn=true;

  if(mw->m_nDevOut!=ui.comboBoxSndOut->currentIndex() or
     mw->m_paOutDevice!=m_outDevList[mw->m_nDevOut]) m_restartSoundOut=true;

  mw->m_myCall=ui.myCallEntry->text();
  mw->m_myGrid=ui.myGridEntry->text();
  mw->m_idInt=ui.idIntSpinBox->value();
  
  mw->m_pttPath = ui.pttComboBox->currentText();
      if (mw->m_pttPath.startsWith("COM"))
          mw->m_pttPortNumber = mw->m_pttPath.mid(3).toInt();
      else
        mw->m_pttPortNumber = 1;

#if !defined(Q_OS_WIN)
    if (ui.pttComboBox->currentText() != "NONE")
        ptt_set_override(ui.pttComboBox->currentText().toUtf8().constData());
    else
        ptt_set_override(nullptr);
#endif
  
  mw->m_astroFont=ui.astroFont->value();
  mw->m_xpol=ui.cbXpol->isChecked();
  mw->m_xpolx=ui.rbAntennaX->isChecked();
  mw->m_saveDir=ui.saveDirEntry->text();
  mw->m_azelDir=ui.azelDirEntry->text();
  {
    // CALL3.TXT path: blank means "use the default in the data dir". A
    // non-blank path must sit in an EXISTING directory -- Fortran's
    // open(status='unknown') creates a missing file happily but fails hard
    // on a missing directory, and that failure would surface inside the
    // decoder rather than here. Reject and keep the previous setting.
    const QString c3 = ui.call3PathEntry->text().trimmed();
    if (c3.isEmpty()) {
      mw->m_call3PathUser.clear();
    } else if (QFileInfo {c3}.absoluteDir().exists()) {
      mw->m_call3PathUser = c3;
    } else {
      QMessageBox::warning(this, "EME65",
        QString("CALL3.TXT directory does not exist:") + QChar(0x0A)
        + QFileInfo {c3}.absolutePath() + QChar(0x0A) + QChar(0x0A)
        + "Keeping the previous setting.");
    }
    mw->pushCall3PathToDecoder();
  }
  mw->m_editorCommand=ui.editorEntry->text();
  mw->m_dxccPfx=ui.dxccEntry->text();
  mw->m_timeout=ui.timeoutSpinBox->value();
  mw->m_dPhi=ui.dPhiSpinBox->value();
  mw->m_fCal=ui.fCalSpinBox->value();
  mw->m_fAdd=ui.faddEntry->text().toDouble();
  mw->m_network=ui.networkRadioButton->isChecked();
  if(ui.rb96000->isChecked()) mw->m_fs96000 = 1;
  if(ui.rb95238->isChecked()) mw->m_fs96000 = 0;
  if(ui.rb192000->isChecked()) mw->m_fs96000 = 2;
  mw->m_bIQxt=ui.rbIQXT->isChecked();
  mw->m_nDevIn=ui.comboBoxSndIn->currentIndex();
  mw->m_paInDevice=m_inDevList[mw->m_nDevIn];
  mw->m_nDevOut=ui.comboBoxSndOut->currentIndex();
  mw->m_paOutDevice=m_outDevList[mw->m_nDevOut];
  mw->m_udpPort=ui.sbPort->value();
  mw->m_IQswap=ui.cbIQswap->isChecked();
  mw->m_initIQplus=ui.cbInitIQplus->isChecked();
  mw->m_dB=ui.sb_dB->value();
  mw->m_mult570=ui.mult570SpinBox->value();
  mw->m_mult570Tx=ui.mult570TxSpinBox->value();
  mw->m_cal570=ui.cal570SpinBox->value();
  mw->m_TxOffset=ui.sbTxOffset->value();
  mw->m_otherUrl=ui.otherUrlBox->text().trimmed();
  mw->m_livecqEnabled=ui.LiveCQ_groupBox->isChecked();
  mw->m_livecqOfficial=ui.cbLiveCQOfficial->isChecked();
  mw->m_livecqN6NU=ui.cbLiveCQN6NU->isChecked();
  mw->m_livecqCustom=ui.cbLiveCQCustom->isChecked();
  mw->m_livecqW3szAppId=ui.cbLiveCQW3SZAppId->currentText();
  
  mw->m_spot_to_psk_reporter = ui.pskBox->isChecked();
  mw->m_psk_reporter_tcpip = ui.pskReporterTcpIpBox->isChecked();
  
  if (restartNeeded) {
      QMessageBox::information(this,
          "Restart Required",
          "Changing sample rate to or from 192000 Hz requires EME65 to be restarted.\n"
          "Please exit EME65 and start it again.");
  }

  // Normal path
  QDialog::accept();

}

void DevSetup::on_soundCardRadioButton_toggled(bool checked)
{
  ui.comboBoxSndIn->setEnabled(ui.soundCardRadioButton->isChecked());
  if(checked) {
    if(mw->m_fs96000 == 0) mw->m_fs96000 = 1;
    ui.rb95238->setEnabled(false);
    ui.rb192000->setChecked(mw->m_fs96000 == 2);
    ui.rb96000->setChecked(mw->m_fs96000 == 1);
  }
  else {
    ui.rb192000->setChecked(mw->m_fs96000 == 2);
    ui.rb96000->setChecked(mw->m_fs96000 == 1);
    ui.rb95238->setChecked(mw->m_fs96000 == 0);
    ui.rb95238->setEnabled(true);
  }
  ui.rb95238->setEnabled(!checked);
  ui.label_InputDev->setEnabled(checked);
  ui.label_Port->setEnabled(!checked);
  ui.sbPort->setEnabled(!checked);
  ui.cbIQswap->setEnabled(checked);
  ui.sb_dB->setEnabled(checked);
}

void DevSetup::on_cbXpol_stateChanged(int n)
{
  mw->m_xpol = (n!=0);
  ui.rbAntenna->setEnabled(mw->m_xpol);
  ui.rbAntennaX->setEnabled(mw->m_xpol);
  ui.dPhiSpinBox->setEnabled(mw->m_xpol);
  ui.labelDphi->setEnabled(mw->m_xpol);
}

void DevSetup::on_cal570SpinBox_valueChanged(double ppm)
{
  mw->m_cal570=ppm;
}

void DevSetup::on_mult570SpinBox_valueChanged(int mult)
{
  mw->m_mult570=mult;
}

void DevSetup::on_sb_dB_valueChanged(int n)
{
  mw->m_dB=n;
}

void DevSetup::updateColorLabels()
{
  QString t;
  int r=ui.sbBackgroundRed->value();
  int g=ui.sbBackgroundGreen->value();
  int b=ui.sbBackgroundBlue->value();
  int r0=ui.sbRed0->value();
  int r1=ui.sbRed1->value();
  int r2=ui.sbRed2->value();
  int r3=ui.sbRed3->value();
  int g0=ui.sbGreen0->value();
  int g1=ui.sbGreen1->value();
  int g2=ui.sbGreen2->value();
  int g3=ui.sbGreen3->value();
  int b0=ui.sbBlue0->value();
  int b1=ui.sbBlue1->value();
  int b2=ui.sbBlue2->value();
  int b3=ui.sbBlue3->value();

  ui.lab0->setStyleSheet (
                          QString {"QLabel{background-color: #%1%2%3; color: #%4%5%6}"}
                             .arg (r, 2, 16, QLatin1Char {'0'})
                             .arg (g, 2, 16, QLatin1Char {'0'})
                             .arg (b, 2, 16, QLatin1Char {'0'})
                             .arg (r0, 2, 16, QLatin1Char {'0'})
                             .arg (g0, 2, 16, QLatin1Char {'0'})
                             .arg (b0, 2, 16, QLatin1Char {'0'})
                          );
  ui.lab1->setStyleSheet(
                         QString {"QLabel{background-color: #%1%2%3; color: #%4%5%6}"}
                            .arg (r, 2, 16, QLatin1Char {'0'})
                            .arg (g, 2, 16, QLatin1Char {'0'})
                            .arg (b, 2, 16, QLatin1Char {'0'})
                            .arg (r1, 2, 16, QLatin1Char {'0'})
                            .arg (g1, 2, 16, QLatin1Char {'0'})
                            .arg (b1, 2, 16, QLatin1Char {'0'})
                         );
  ui.lab2->setStyleSheet(
                         QString {"QLabel{background-color: #%1%2%3; color: #%4%5%6}"}
                            .arg (r, 2, 16, QLatin1Char {'0'})
                            .arg (g, 2, 16, QLatin1Char {'0'})
                            .arg (b, 2, 16, QLatin1Char {'0'})
                            .arg (r2, 2, 16, QLatin1Char {'0'})
                            .arg (g2, 2, 16, QLatin1Char {'0'})
                            .arg (b2, 2, 16, QLatin1Char {'0'})
                         );
  ui.lab3->setStyleSheet(
                         QString {"QLabel{background-color: #%1%2%3; color: #%4%5%6}"}
                            .arg (r, 2, 16, QLatin1Char {'0'})
                            .arg (g, 2, 16, QLatin1Char {'0'})
                            .arg (b, 2, 16, QLatin1Char {'0'})
                            .arg (r3, 2, 16, QLatin1Char {'0'})
                            .arg (g3, 2, 16, QLatin1Char {'0'})
                            .arg (b3, 2, 16, QLatin1Char {'0'})
                         );

  mw->m_colors.clear ();
  QTextStream ots {&mw->m_colors, QIODevice::WriteOnly};
  ots.setIntegerBase (16);
  ots.setFieldWidth (2);
  ots.setPadChar ('0');
  ots << r << g << b << r0 << g0 << b0 << r1 << g1 << b1 << r2 << g2 << b2 << r3 << g3 << b3;
}

// Named color presets: bg + 4 aging levels, brightest (newest decode) to
// faintest (oldest). "Classic" matches the app's long-standing default
// (MainWindow::m_colors's default string) so selecting it reproduces
// exactly what users already had before this dropdown existed.
void DevSetup::applyColorPreset(const QString& name)
{
  int bg[3], l0[3], l1[3], l2[3], l3[3];
  if (name == "Classic") {
    bg[0]=0;   bg[1]=0;   bg[2]=102;
    l0[0]=255; l0[1]=0;   l0[2]=0;
    l1[0]=255; l1[1]=255; l1[2]=0;
    l2[0]=150; l2[1]=150; l2[2]=150;
    l3[0]=100; l3[1]=100; l3[2]=100;
  } else if (name == "Black on white") {
    bg[0]=255; bg[1]=255; bg[2]=255;
    l0[0]=0;   l0[1]=0;   l0[2]=0;
    l1[0]=64;  l1[1]=64;  l1[2]=64;
    l2[0]=128; l2[1]=128; l2[2]=128;
    l3[0]=180; l3[1]=180; l3[2]=180;
  } else if (name == "White on Blue") {
    bg[0]=0;   bg[1]=0;   bg[2]=128;
    l0[0]=255; l0[1]=255; l0[2]=255;
    l1[0]=200; l1[1]=200; l1[2]=230;
    l2[0]=140; l2[1]=140; l2[2]=180;
    l3[0]=90;  l3[1]=90;  l3[2]=130;
  } else {
    return;  // "Custom" (or unrecognized) -- no fixed values, no-op
  }

  m_applyingPreset = true;
  ui.sbBackgroundRed->setValue(bg[0]);
  ui.sbBackgroundGreen->setValue(bg[1]);
  ui.sbBackgroundBlue->setValue(bg[2]);
  ui.sbRed0->setValue(l0[0]);
  ui.sbGreen0->setValue(l0[1]);
  ui.sbBlue0->setValue(l0[2]);
  ui.sbRed1->setValue(l1[0]);
  ui.sbGreen1->setValue(l1[1]);
  ui.sbBlue1->setValue(l1[2]);
  ui.sbRed2->setValue(l2[0]);
  ui.sbGreen2->setValue(l2[1]);
  ui.sbBlue2->setValue(l2[2]);
  ui.sbRed3->setValue(l3[0]);
  ui.sbGreen3->setValue(l3[1]);
  ui.sbBlue3->setValue(l3[2]);
  m_applyingPreset = false;

  updateColorLabels();
  mw->m_colorPreset = name;
}

// Called from every color spinbox's valueChanged handler. A manual edit
// means the table no longer matches any named preset.
void DevSetup::markCustomColors()
{
  if (m_applyingPreset) return;
  mw->m_colorPreset = "Custom";
  if (ui.colorPresetComboBox->currentText() != "Custom") {
    ui.colorPresetComboBox->blockSignals(true);
    ui.colorPresetComboBox->setCurrentText("Custom");
    ui.colorPresetComboBox->blockSignals(false);
  }
}

void DevSetup::on_colorPresetComboBox_currentIndexChanged(int /*index*/)
{
  applyColorPreset(ui.colorPresetComboBox->currentText());
}

void DevSetup::on_sbBackgroundRed_valueChanged(int /*r*/)
{
  markCustomColors();
  updateColorLabels();
}

void DevSetup::on_sbBackgroundGreen_valueChanged(int /*g*/)
{
  markCustomColors();
  updateColorLabels();
}

void DevSetup::on_sbBackgroundBlue_valueChanged(int /*b*/)
{
  markCustomColors();
  updateColorLabels();
}


void DevSetup::on_sbRed0_valueChanged(int /*arg1*/)
{
  markCustomColors();
  updateColorLabels();
}

void DevSetup::on_sbGreen0_valueChanged(int /*arg1*/)
{
  markCustomColors();
  updateColorLabels();
}

void DevSetup::on_sbBlue0_valueChanged(int /*arg1*/)
{
  markCustomColors();
  updateColorLabels();
}

void DevSetup::on_sbRed1_valueChanged(int /*arg1*/)
{
  markCustomColors();
   updateColorLabels();
}

void DevSetup::on_sbGreen1_valueChanged(int /*arg1*/)
{
  markCustomColors();
  updateColorLabels();
}

void DevSetup::on_sbBlue1_valueChanged(int /*arg1*/)
{
  markCustomColors();
   updateColorLabels();
}

void DevSetup::on_sbRed2_valueChanged(int /*arg1*/)
{
  markCustomColors();
   updateColorLabels();
}

void DevSetup::on_sbGreen2_valueChanged(int /*arg1*/)
{
  markCustomColors();
   updateColorLabels();
}

void DevSetup::on_sbBlue2_valueChanged(int /*arg1*/)
{
  markCustomColors();
   updateColorLabels();
}

void DevSetup::on_sbRed3_valueChanged(int /*arg1*/)
{
  markCustomColors();
  updateColorLabels();
}

void DevSetup::on_sbGreen3_valueChanged(int /*arg1*/)
{
  markCustomColors();
  updateColorLabels();
}

void DevSetup::on_sbBlue3_valueChanged(int /*arg1*/)
{
  markCustomColors();
  updateColorLabels();
}

void DevSetup::on_pushButton_5_clicked()
{
  QColor color = QColorDialog::getColor(Qt::green, this);
  if (color.isValid()) {
  }
}

void DevSetup::on_mult570TxSpinBox_valueChanged(int n)
{
  mw->m_mult570Tx=n;
}

void DevSetup::on_rbIQXT_toggled(bool checked)
{
  mw->m_bIQxt=checked;
  ui.mult570TxSpinBox->setEnabled(mw->m_bIQxt);
  ui.label_25->setEnabled(mw->m_bIQxt);
  ui.sbTxOffset->setEnabled(mw->m_bIQxt);
  ui.label_26->setEnabled(mw->m_bIQxt);
}

void DevSetup::on_sbTxOffset_valueChanged(double f)
{
  mw->m_TxOffset=f;
}


