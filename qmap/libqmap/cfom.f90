subroutine cfom(dd,k0,k,ndop0)
  ! Constant Frequency Offset Measurement: applies a complex-phasor
  ! frequency shift of (-ndop0/2) Hz to the IQ samples in dd between
  ! sample k0+1 and k inclusive. Used to remove self-EME Doppler
  ! before the long FFT in qmapa, so a self-echo lands at the dial
  ! instead of being shifted by the round-trip Doppler.
  !
  ! Sample-rate aware: the per-sample phase increment is scaled by
  ! the active runtime rate (was hardcoded 96000.0 — broke wide-mode
  ! 256 kHz, where the same dphi gave 2.67× the intended shift).
  use qmap_params, only: nrate_active

  ! Assumed-size dummy — caller dimensions match the active runtime
  ! NSMAX. Was real dd(2,NMAX) with NMAX=60*96000 hardcoded.
  real dd(2,*)
  complex*16 w,wstep,c
  real*8 twopi,dphi

  ! Reset phase accumulator on every call. Was `data first/save w` so
  ! subsequent calls picked up where the previous one left off — broke
  ! the relationship between sample index n and exp(j·dphi·n) for any
  ! call after the first, randomising the per-buffer phase reference.
  twopi=8.d0*atan(1.d0)
  w=1.d0

  dop0=0.5*ndop0
  dphi=dop0*twopi/real(nrate_active,8)
  ! cmplx() returns DEFAULT (single) precision unless kind= is given.
  ! Without kind=8 the wstep phase is truncated to ~1e-7, and the
  ! per-sample phase error accumulates linearly over 15M samples to
  ! ~1.5 rad = 86° cumulative drift across a 60 s buffer — looks like
  ! fake Doppler drift to Q65 LDPC and breaks the decode.
  wstep=cmplx(cos(dphi),sin(dphi),kind=8)

  do j=k0+1,k
     ! Force double-precision throughout the multiply chain (cmplx
     ! defaults to single → would truncate the per-sample product).
     c=w*cmplx(dd(1,j),dd(2,j),kind=8)
     dd(1,j)=real(c,kind=4)
     dd(2,j)=real(aimag(c),kind=4)
     w=w*wstep
  enddo

  return
end subroutine cfom
