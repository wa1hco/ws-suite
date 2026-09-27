#include "commons.h"

extern "C" {
    float*  dd = nullptr;
    float*  dd_snap = nullptr;    // decoder-side snapshot (see decode())
    float*  ss_snap = nullptr;
    float*  savg_snap = nullptr;
    float*  ss = nullptr;
    float*  savg = nullptr;
}

