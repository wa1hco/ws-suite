#ifndef COMMONS_H
#define COMMONS_H

#include <QtGlobal>   // qint64 for click-to-work click_rf_hz field

// NFFT and NSMAX are the *runtime active* dimensions used by C++ loops
// over the spectra/IQ buffers. They stay at the 96 kHz baseline values
// for now; runtime selection comes in a follow-up commit.
#define NFFT 32768
#define NSMAX 5760000

// COMMON-block storage is sized at the wide-mode upper bound declared in
// qmap_runtime_config.h (256 kHz IQ rate). Fortran qmap_params.f90
// declares matching values; both sides MUST agree or the COMMON layout
// drifts. Active loops touch only the first NSMAX/NFFT entries — the
// unused tail is slack until wide-mode runtime selection lands.
#include "qmap_runtime_config.h"

extern "C" {

extern struct {                     //This is "common/datcom/..." in Fortran
  float d4[2*qmap_runtime::MAX_NSMAX];      //Raw I/Q data from Linrad (sized for wide-mode max)
  float ss[400*qmap_runtime::MAX_NFFT];     //Half-symbol spectra at 0,45,90,135 deg pol
  float savg[qmap_runtime::MAX_NFFT];       //Avg spectra at 0,45,90,135 deg pol
  double fcenter;                   //Center freq from Linrad (MHz)
  int nutc;                         //UTC as integer, HHMM
  float fselected;                  //Selected frequency for nagain decodes
  int mousedf;                      //User-selected DF
  int mousefqso;                    //User-selected QSO freq (kHz)
  int nagain;                       //1 ==> decode only at fQSO +/- Tol
  int ndepth;                       //How much hinted decoding to do?
  int ndiskdat;                     //1 ==> data read from *.iq file
  int ntx60;                        //Number of seconds transmitted in Q65-60x
  int newdat;                       //1 ==> new data, must do long FFT
  int nfa;                          //Low decode limit (kHz)
  int nfb;                          //High decode limit (kHz)
  int nfcal;                        //Frequency correction, for calibration (Hz)
  int nfshift;                      //Shift of displayed center freq (kHz)
  int ntx30a;                       //Number of seconds transmitted in first half minute , Q65-30x
  int ntx30b;                       //Number of seconds transmitted in second half minute, Q65-30x
  int ntol;                         //+/- decoding range around fQSO (Hz)
  int n60;                          //nsecs%60
  int nCFOM;                        //1 ==> apply self-Doppler IQ pre-shift before fftbig (cfom.f90)
  int nfsample;                     //Input sample rate
  int ndop58;                       //EME Self Doppler at t=58
  int nBaseSubmode;                 //Base submode for Q65-60x (aka m_modeQ65)
  int ndop00;                       //EME Self Doppler at t=0
  int nsave;                        //0=None, 1=SaveDecoded, 2=SaveAll
  int max_drift;                    //Maximum Q65 drift: units symbol_rate/TxT
  int offset;                       //Offset in Hz
  int nhsym;                        //Number of available JT65 half-symbols
  char mycall[12];
  char mygrid[6];
  char hiscall[12];
  char hisgrid[6];
  char datetime[20];
  int junk1;                        //Used to test extent of copy to shared memory
  int junk2;
  bool bAlso30;                     //Process for 30-second submode as well as 60-second
} datcom_;

extern struct {                     //This is "common/datcom2/..." in Fortran
  float d4[2*qmap_runtime::MAX_NSMAX];      //Raw I/Q data from Linrad (sized for wide-mode max)
  float ss[400*qmap_runtime::MAX_NFFT];     //Half-symbol spectra at 0,45,90,135 deg pol
  float savg[qmap_runtime::MAX_NFFT];       //Avg spectra at 0,45,90,135 deg pol
  double fcenter;                   //Center freq from Linrad (MHz)
  int nutc;                         //UTC as integer, HHMM
  float fselected;                  //Selected frequency for nagain decodes
  int mousedf;                      //User-selected DF
  int mousefqso;                    //User-selected QSO freq (kHz)
  int nagain;                       //1 ==> decode only at fQSO +/- Tol
  int ndepth;                       //How much hinted decoding to do?
  int ndiskdat;                     //1 ==> data read from *.iq file
  int ntx60;                        //Number of seconds transmitted in Q65-60x
  int newdat;                       //1 ==> new data, must do long FFT
  int nfa;                          //Low decode limit (kHz)
  int nfb;                          //High decode limit (kHz)
  int nfcal;                        //Frequency correction, for calibration (Hz)
  int nfshift;                      //Shift of displayed center freq (kHz)
  int ntx30a;                       //Number of seconds transmitted in first half minute , Q65-30x
  int ntx30b;                       //Number of seconds transmitted in second half minute, Q65-30x
  int ntol;                         //+/- decoding range around fQSO (Hz)
  int n60;                          //nsecs%60
  int nCFOM;                        //1 ==> apply self-Doppler IQ pre-shift before fftbig (cfom.f90)
  int nfsample;                     //Input sample rate
  int ndop58;                       //EME Self Doppler at t=58
  int nBaseSubmode;                 //Base submode for Q65-60x (aka m_modeQ65)
  int ndop00;                       //EME Self Doppler at t=0
  int nsave;                        //0=None, 1=SaveDecoded, 2=SaveAll
  int max_drift;                    //Maximum Q65 drift: units symbol_rate/TxT
  int offset;                       //Offset in Hz
  int nhsym;                        //Number of available JT65 half-symbols
  char mycall[12];
  char mygrid[6];
  char hiscall[12];
  char hisgrid[6];
  char datetime[20];
  int junk1;                        //Used to test extent of copy to shared memory
  int junk2;
  bool bAlso30;                     //Process for 30-second submode as well as 60-second
} datcom2_;

extern struct {
  int ndecodes;          //These are flags for inter-process communication
  int ncand;             //between QMAP and WSJT-X
  int nQDecoderDone;     //1 for real-time decodes, 2 for data from disk
  int nWDecoderBusy;     //Set to 1 when WSJT-X decoder is busy
  int nWTransmitting;    //Set to TRperiod when WSJT-X is transmitting
  int kHzRequested;      //Integer kHz dial frequency request to WSJT-X
  char result[50][72];   //Staging area for QMAP decodes
  // Click-to-work (N6NU 2026-06-26): QMAP-plotter mouse-press on a
  // callsign overlay populates these fields and increments click_seq;
  // WSJT-X polls click_seq each tick and invokes overlayCallsignClicked
  // when it changes. RF freq is sent as absolute Hz so WSJT-X can
  // compute its own audio offset from m_freqNominal regardless of which
  // dial it's on (QMAP-wideband vs WSJT-X-slice can be tens of kHz apart).
  int click_seq;
  char click_callsign[16];
  qint64 click_rf_hz;
  int click_even_period;
  char click_grid[8];
  char click_mode[8];    //Q65 sub-mode designator ("60A" etc) for click-to-work
  int click_start_qso;   //1 = double-click: also enable Tx in WSJT-X to start the QSO
} decodes_;

extern struct {
  char result2[50][8];
} decodes2_;

extern struct {
  char revision[22];
  char saveFileName[120];
} savecom_;

}

#endif // COMMONS_H
