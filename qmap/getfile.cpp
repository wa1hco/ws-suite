#include "getfile.h"
#include "qmap_runtime_config.h"
#include <QDir>
#include <QFileInfo>
#include <QMessageBox>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

extern qint16 id[2*60*qmap_runtime::MAX_IQ_RATE_HZ];

namespace {
// .iq files are byte-perfect signatures: an int16-pair stream of length
// 60 s × rate at the rate the file was written, with an 8-byte fcenter
// header and an optional 8-byte (ntx30a, ntx30b) trailer. So given a
// file size, we can recover the rate unambiguously across the supported
// rate ladder. Returns 0 on no match, else the detected rate in Hz.
int detectIqRateFromFileSize(qint64 file_size_bytes)
{
    static const int kKnownRates[] = { 96000, 128000, 192000, 256000 };
    for (int rate : kKnownRates) {
        const qint64 payload = static_cast<qint64>(60) * rate * 4; // 4 B / pair
        if (file_size_bytes == 8 + payload          // no trailer
         || file_size_bytes == 8 + payload + 8) {   // with trailer
            return rate;
        }
    }
    return 0;
}
}  // namespace

void getfile(QString fname, int dbDgrd)
{
  // npts is the number of int16s for one full 60 s file at the active
  // IQ rate. Disk-load .iq files have no rate field in the header, so
  // we rely on the user telling us via [Linrad]/sample_rate_mode (or
  // --samplerate). At 96 kHz npts=11.52M; at 256 kHz npts=30.72M.
  const int rate = qmap_runtime::activeRateHz();
  int npts = 2 * 60 * rate;

  // ── File-size sanity check ─────────────────────────────────────────
  // Catch the specific foot-gun: user opens a 96 kHz file with QMAP
  // set to 256 kHz (or vice versa). Without this check, fread returns
  // short, dd ends up with valid samples in the prefix and zeros (from
  // the memset below) for the tail, symspec processes the partial
  // buffer, and the decoder finds nothing. Silent failure mode.
  //
  // We ONLY warn when the file size matches a *different* supported IQ
  // rate (96 / 128 / 192 / 256 kHz × 60 s, with optional 8-byte trailer)
  // — i.e. an unambiguous cross-rate mismatch with a clear remediation.
  // For any other size deviation (older 56 s formats, files in mid-
  // recording, custom-format .iq from other tools, anything with an
  // unexpected trailer) we just stay silent: getfile()'s own short-read
  // handling already zero-pads gracefully. False-positive warnings on
  // valid-but-unfamiliar files were noisier than helpful (DG2YCB
  // tester report 2026-05-09).
  {
      const qint64 file_size = QFileInfo(fname).size();
      const qint64 expected_no_trailer  = 8 + static_cast<qint64>(60) * rate * 4;
      const qint64 expected_with_trailer = expected_no_trailer + 8;
      const bool   matches_active =
          (file_size == expected_no_trailer || file_size == expected_with_trailer);
      if (file_size > 0 && !matches_active) {
          const int detected = detectIqRateFromFileSize(file_size);
          if (detected > 0 && detected != rate) {
              QString msg = QString(
                  "File appears to be %1 kHz IQ, but WS-MAP is set to %2 kHz.\n\n"
                  "File: %3 (%4 bytes)\n"
                  "Expected for %2 kHz: %5 bytes\n\n"
                  "To open this file, open Settings (Ctrl+,), set "
                  "\"IQ sample rate\" to %1 kHz, and restart WS-MAP.\n\n"
                  "Otherwise WS-MAP will load only the prefix and pad the rest "
                  "with zeros — the decoder will likely find nothing.")
                  .arg(detected / 1000)
                  .arg(rate / 1000)
                  .arg(fname)
                  .arg(file_size)
                  .arg(expected_no_trailer);
              std::fprintf(stderr, "[getfile] cross-rate mismatch: %s\n",
                           msg.toUtf8().constData());
              QMessageBox::warning(nullptr, "WS-MAP — File / rate mismatch", msg);
          } else {
              // Size doesn't match anything in the [96/128/192/256] ladder.
              // Probably an older 56 s format or a file from a sibling tool.
              // Best-effort load; the existing fread short-read handling +
              // memset zero-fill will sort it out.
              std::fprintf(stderr,
                  "[getfile] note: file size %lld doesn't match active rate %d kHz "
                  "(expected %lld), loading best-effort.\n",
                  static_cast<long long>(file_size),
                  rate / 1000,
                  static_cast<long long>(expected_no_trailer));
          }
      }
  }

// Degrade S/N by dbDgrd dB -- for tests only!!
  float dgrd=0.0;
  if(dbDgrd<0) dgrd = 23.0*sqrt(pow(10.0,-0.1*(double)dbDgrd) - 1.0);
  float fac=23.0/sqrt(dgrd*dgrd + 23.0*23.0);

  memset(id,0,2*npts);
  char name[80];
  strcpy(name,fname.toLocal8Bit());
  FILE* fp=fopen(name,"rb");

  if(fp != NULL) {
    auto n = fread(&datcom_.fcenter,sizeof(datcom_.fcenter),1,fp);
    n=fread(id,2,npts,fp);
    n=fread(&datcom_.ntx30a,4,1,fp);
    n=fread(&datcom_.ntx30b,4,1,fp);
    if(n==0) {
      datcom_.ntx30a=0;
      datcom_.ntx30b=0;
    }
    int j=0;

    if(dbDgrd<0) {
      for(int i=0; i<npts; i+=2) {
        datcom_.d4[j++]=fac*((float)id[i] + dgrd*gran());
        datcom_.d4[j++]=fac*((float)id[i+1] + dgrd*gran());
      }
    } else {
      for(int i=0; i<npts; i+=2) {
        datcom_.d4[j++]=(float)id[i];
        datcom_.d4[j++]=(float)id[i+1];
      }
    }
    fclose(fp);

    datcom_.ndiskdat=1;
  //  int nfreq=(int)datcom_.fcenter;
  //  if(nfreq!=144 and nfreq != 432 and nfreq != 1296) datcom_.fcenter=1296.090;
    int i0=fname.indexOf(".iq");
    datcom_.nutc=0;
    if(i0>0) {
      datcom_.nutc=100*fname.mid(i0-4,2).toInt() + fname.mid(i0-2,2).toInt();
    }
  }
}

void save_iq(QString fname)
{
  int npts=2*60*96000;
  qint16* buf=(qint16*)malloc(2*npts);
  char name[80];
  strcpy(name,fname.toLocal8Bit());
  FILE* fp=fopen(name,"wb");

  if(fp != NULL) {
    fwrite(&datcom_.fcenter,sizeof(datcom_.fcenter),1,fp);
    int j=0;
    for(int i=0; i<npts; i+=2) {
      buf[i]=(qint16)qRound(datcom_.d4[j++]);
      buf[i+1]=(qint16)qRound(datcom_.d4[j++]);
    }
    fwrite(buf,2,npts,fp);
    fwrite(&datcom_.ntx30a,4,2,fp);   //Write ntx30a and ntx30b to disk
    fclose(fp);
  }
  free(buf);
}

/* Generate gaussian random float with mean=0 and std_dev=1 */
float gran()
{
  float fac,rsq,v1,v2;
  static float gset;
  static int iset;

  if(iset){
    /* Already got one */
    iset = 0;
    return gset;
  }
  /* Generate two evenly distributed numbers between -1 and +1
   * that are inside the unit circle
   */
  do {
    v1 = 2.0 * (float)rand() / RAND_MAX - 1;
    v2 = 2.0 * (float)rand() / RAND_MAX - 1;
    rsq = v1*v1 + v2*v2;
  } while(rsq >= 1.0 || rsq == 0.0);
  fac = sqrt(-2.0*log(rsq)/rsq);
  gset = v1*fac;
  iset++;
  return v2*fac;
}
