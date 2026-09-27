#include "soundin.h"
#include <math.h>
#include <atomic>

#ifdef Q_OS_WIN32
#include <windows.h>
#else
#include <sys/socket.h>
#endif

#include "qmap_runtime_config.h"

namespace {
// Updated by inputUDP() on each successful readDatagram(); read by
// WideGraph's 1 Hz no-stream poll. Atomic so the poll thread (GUI)
// doesn't need a mutex against the SoundIn thread.
std::atomic<qint64> g_lastUdpPacketMs{0};
}

qint64 soundinLastUdpPacketMs()
{
  return g_lastUdpPacketMs.load(std::memory_order_relaxed);
}

#define NFFT 32768
#define FRAMES_PER_BUFFER 1024

// COMMON-block dimensions sized at qmap_runtime::MAX_* (256 kHz upper
// bound). Local NFFT (above) still controls loop bounds; only allocation
// changes here. Must match qmap_params.f90 and commons.h.

extern "C"
{
  struct
  {
    double d8[60*qmap_runtime::MAX_IQ_RATE_HZ]; //This is "common/datcom/..." in fortran (sized for wide-mode max)
    float ss[400*qmap_runtime::MAX_NFFT];
    float savg[qmap_runtime::MAX_NFFT];   //Avg spectra at 0,45,90,135 deg pol
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
    int junk5;                        //
    int nCFOM;                        //1 ==> apply self-Doppler IQ pre-shift before fftbig
    int nfsample;                     //Input sample rate
    int junk3;                        //
    int nBaseSubmode;                 //Base submode for Q65-60x (aka m_modeQ65)
    int ndop00;                       //EME Self Doppler
    int nsave;                        //Number of s3(64,63) spectra saved
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
}

namespace
{
  struct COMWrapper
  {
    explicit COMWrapper ()
    {
#ifdef Q_OS_WIN32
      // required because Qt only does this for GUI thread
      CoInitializeEx (nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
#endif
    }
    ~COMWrapper ()
    {
#ifdef Q_OS_WIN32
      CoUninitialize ();
#endif
    }
  };
}

void SoundInThread::run()                           //SoundInThread::run()
{
  quitExecution = false;

  if (m_net) {
    inputUDP();
    return;
  }
}

void SoundInThread::setScale(qint32 n)
{
  m_dB=n;
}
void SoundInThread::setPort(int n)                              //setPort()
{
  if (isRunning()) return;
  this->m_udpPort=n;
}

void SoundInThread::setRate(double rate)                         //setRate()
{
  if (isRunning()) return;
  this->m_rate = rate;
}

void SoundInThread::setBufSize(unsigned n)                      //setBufSize()
{
  if (isRunning()) return;
  this->bufSize = n;
}

void SoundInThread::setFadd(double x)
{
  m_fAdd=x;
}


void SoundInThread::quit()                                       //quit()
{
  quitExecution = true;
}

void SoundInThread::setNetwork(bool b)                          //setNetwork()
{
  m_net = b;
}

void SoundInThread::setMonitoring(bool b)                    //setMonitoring()
{
  m_monitoring = b;
}

void SoundInThread::setNrx(int n)                              //setNrx()
{
  m_nrx = n;
}

int SoundInThread::nrx()
{
  return m_nrx;
}

int SoundInThread::mhsym()
{
  return m_hsym;
}

void SoundInThread::setPeriod(int n)
{
  m_TRperiod=n;
}

//--------------------------------------------------------------- inputUDP()
void SoundInThread::inputUDP()
{
  udpSocket = new QUdpSocket();
  if(!udpSocket->bind(m_udpPort,QUdpSocket::ShareAddress) )
  {
    emit error(tr("UDP Socket bind failed."));
    return;
  }

  // Set this socket's total buffer space for received UDP packets
  udpSocket->setSocketOption (QUdpSocket::ReceiveBufferSizeSocketOption, 141600);

  bool qe = quitExecution;
  // Payload sized at the wide-mode upper bound to match the bridge's
  // LinradRaw16Packet (LINRAD_MAX_PAIRS_PER_PACKET = 1024 → 4096 bytes
  // = 512 doubles). Active prefix per packet = iz IQ pairs, computed
  // from readDatagram()'s actual byte count.
  static const int LINRAD_HEADER_BYTES = 24;
  static const int LINRAD_MAX_PAYLOAD_DOUBLES = 512;   // 1024 IQ pairs
  struct linradBuffer {
    double cfreq;
    int msec;
    float userfreq;
    int iptr;
    quint16 iblk;
    qint8 nrx;
    char iusb;
    double d8[LINRAD_MAX_PAYLOAD_DOUBLES];
  } b;

  int ntr0=99;
  int k=0;
  int nsec;
  int ntr;
  int nhsym0=0;
  int iz=174;

  // Main loop for input of UDP packets over the network:
  while (!qe) {
    qe = quitExecution;
    if (qe) break;
    if (!udpSocket->hasPendingDatagrams()) {
//      msleep(2);                  // Sleep if no packet available
      QObject().thread()->usleep(2000);
    } else {
      // Read whatever the bridge sends; legacy = 1416 bytes (348 i*2
      // pairs at 96 kHz), wide-mode up to 24+1024*4 = 4120.
      int nBytesRead = udpSocket->readDatagram((char *)&b, sizeof(b));
      if (nBytesRead < LINRAD_HEADER_BYTES) {
        qDebug() << "UDP Read Error: short datagram" << nBytesRead;
        continue;
      }
      const int payload_bytes = nBytesRead - LINRAD_HEADER_BYTES;
      g_lastUdpPacketMs.store(QDateTime::currentMSecsSinceEpoch(),
                              std::memory_order_relaxed);

      qint64 ms = QDateTime::currentMSecsSinceEpoch() % 86400000;
      nsec = ms/1000;             // Time according to this computer
      ntr = nsec % m_TRperiod;

// Reset buffer pointer and symbol number at start of minute
      if(ntr < ntr0 or !m_monitoring or m_TRperiod!=m_TRperiod0) {
        k=0;
        nhsym0=0;
        m_TRperiod0=m_TRperiod;
      }

      ntr0=ntr;

      if(m_monitoring) {
        m_nrx=b.nrx;
        // iz = IQ pairs in this datagram, derived from payload byte count
        // and the nrx mode flag. Was hardcoded for the 1416-byte 96 kHz
        // packet; now scales with the bridge's pairs_per_packet.
        if(m_nrx == +1) iz = payload_bytes / 4;       // i*2: 4 bytes per pair
        if(m_nrx == -1 or m_nrx == +2) iz = payload_bytes / 8;  // r*4: 8 bytes per pair
        if(m_nrx == -2) iz = payload_bytes / 16;     // r*4 dual-channel: 16 bytes per pair
        if(iz <= 0) continue;                         // malformed

        // Tell recvpkt how many pairs this packet carries (per-packet,
        // pushed via qmap_params::iz_packet_active).
        if(m_nrx == +1) qmap_set_iz_packet_active(iz);

        // If buffer will not overflow, move data into datcom_
        if ((k+iz) <= 60*qmap_runtime::activeRateHz()) {
          int nsam=-1;
          recvpkt_(&nsam, &b.iblk, &b.nrx, &k, b.d8, b.d8, &m_dB);
          datcom_.fcenter=b.cfreq + m_fAdd;
        }
        // Half-symbol stride = 0.15 s of samples = 0.15 * activeRateHz.
        // 2048 = startup latency offset (post-FFT timf2 setup in symspec).
        m_hsym=(k-2048) / (qmap_runtime::activeRateHz() * 15 / 100);   // was: /14400 = 0.15*96000
        if(m_hsym != nhsym0) {
          if(!m_dataSinkBusy) {
            m_dataSinkBusy=true;
            emit readyForFFT(k);         //Signal to compute new FFTs
          }
          nhsym0=m_hsym;
        }
      }
    }
  }
  delete udpSocket;
}
