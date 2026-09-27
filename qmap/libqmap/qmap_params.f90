! Runtime sample-rate / FFT-size parameters for QMAP wide-mode.
!
! Allocatable arrays don't work in Fortran COMMON blocks, so the
! /datcom/ buffers are sized at compile time against MAX_IQ_RATE_HZ
! (the largest supported input rate). The runtime variables below
! control how much of the buffer is actually used at a given session;
! the C++ side calls qmap_set_runtime_config(rate, nfft) once at
! startup before any Fortran call into libqmap.
!
! Default values reproduce the original 96 kHz / NFFT=32768 behaviour
! exactly so a scaffold-only build is byte-identical to upstream.

module qmap_params
  use, intrinsic :: iso_c_binding
  implicit none

  ! Compile-time upper bounds. Pick to be the worst case the ABI
  ! supports; runtime can only request rates / NFFT sizes <= these.
  integer, parameter :: MAX_IQ_RATE_HZ = 256000
  integer, parameter :: MAX_NSMAX      = 60 * MAX_IQ_RATE_HZ        ! 15,360,000
  integer, parameter :: MAX_NFFT       = 131072                     ! next pow2 above 32768 * 256/96
  integer, parameter :: MAX_NFFT_BIG   = 56 * MAX_IQ_RATE_HZ        ! 14,336,000 — fftbig long-FFT plan size at 256 kHz
  integer, parameter :: BASELINE_RATE  = 96000                      ! upstream default
  integer, parameter :: BASELINE_NFFT  = 32768                      ! upstream default
  integer, parameter :: BASELINE_NFFT_BIG = 56 * BASELINE_RATE      ! 5,376,000 — historical MAXFFT1

  ! Active runtime values. Defaults preserve upstream behaviour;
  ! bumped by qmap_set_runtime_config from C++ at startup if the
  ! user has selected a wider mode.
  integer :: nrate_active     = BASELINE_RATE
  integer :: nfft_active      = BASELINE_NFFT
  integer :: nsmax_active     = 60 * BASELINE_RATE                  ! 5,760,000
  integer :: nfft_big_active  = BASELINE_NFFT_BIG                   ! 5,376,000

  ! Per-packet IQ-pair count for the nrx=+1 (single pol, i*2) Linrad
  ! UDP datagram. Updated on EVERY packet receive by soundin.cpp via
  ! qmap_set_iz_packet_active() so recvpkt's inner loop knows how
  ! many pairs the wire actually carried (legacy 348 at 96 kHz; can
  ! grow up to LINRAD_MAX_PAIRS_PER_PACKET=1024 in wide-mode).
  integer :: iz_packet_active = 348

contains

  ! Setter callable from C++ (see qmap_runtime_config.h).
  ! Idempotent — call once at startup, before any decoder activity.
  subroutine qmap_set_runtime_config(rate_hz, nfft) bind(c, name='qmap_set_runtime_config')
    integer(c_int), value :: rate_hz, nfft
    if (rate_hz <= 0 .or. rate_hz > MAX_IQ_RATE_HZ) return
    if (nfft    <= 0 .or. nfft    > MAX_NFFT)       return
    nrate_active    = rate_hz
    nfft_active     = nfft
    nsmax_active    = 60 * rate_hz
    nfft_big_active = 56 * rate_hz
  end subroutine qmap_set_runtime_config

  ! Per-packet pair-count setter, called from soundin.cpp after each
  ! readDatagram() so recvpkt's loop bound matches the actual wire size.
  subroutine qmap_set_iz_packet_active(iz) bind(c, name='qmap_set_iz_packet_active')
    integer(c_int), value :: iz
    if (iz > 0) iz_packet_active = iz
  end subroutine qmap_set_iz_packet_active

end module qmap_params
