subroutine qmapa(dd,ss,savg,newdat,nutc,fcenter,ntol,nfa,nfb,         &
     mousedf,mousefqso,nagain,ntx30a,ntx30b,nfshift,max_drift,offset, &
     nfcal,mycall,hiscall,hisgrid,nfsample,nBaseSubmode,ndepth,       &
     datetime,ndop00,fselected,bAlso30,nhsym,nCFOM)

!  Processes timf2 data received from Linrad to find and decode Q65 signals.

  use timer_module, only: timer
  use qmap_params, only: nrate_active, nfft_active, nsmax_active

  type candidate
     real :: snr          !Relative S/N of sync detection
     real :: f            !Freq of sync tone, 0 to 96000 Hz
     real :: xdt          !DT of matching sync pattern, -1.0 to +4.0 s
     integer :: ntrperiod !60 for Q65-60x, 30 for Q65-30x
     integer :: iseq      !0 for first half-minute, 1 for second half
  end type candidate

  type good_decode
     real :: f            !Freq of sync tone, 0 to 96000 Hz
     integer :: ntrperiod !60 for Q65-60x, 30 for Q65-30x
     integer :: iseq      !0 for first half-minute, 1 for second half
  end type good_decode

  parameter (MAX_CANDIDATES=50)
  parameter (MAXMSG=1000)            !Size of decoded message list
! Dummy args: assumed-size — caller dimensions match the active runtime
! sizes (nsmax_active for dd, nfft_active for ss/savg). Was statically
! NFFT=32768 / NSMAX=60*96000, which masked the wide-mode tail of the
! COMMON arrays from this routine.
  real dd(2,*)                       !I/Q data from Linrad
  real ss(400,*)                     !Symbol spectra
  real savg(*)                       !Average spectrum
  real*8 fcenter                     !Center RF frequency, MHz
  real*4 half_band_khz               !0.5 * nrate_active / 1000 (was hardcoded 48)
  logical*1 bAlso30,bClickDecode
  character mycall*12,hiscall*12,hisgrid*6
  type(candidate) :: cand(MAX_CANDIDATES)
  type(good_decode) found(MAX_CANDIDATES)
  character*72 result
  character*8 result2                !liveCQ
  character*20 datetime
  common/decodes/ndecodes,ncand2,nQDecoderDone,nWDecoderBusy,              &
       nWTransmitting,kHzRequested,result(50)
  common/decodes2/result2(50)        !liveCQ
  save

  tsec0=sec_midn()
  if(nagain.ge.1) ndepth=3            !Use full depth for click-to-decode
  nkhz_center=nint(1000.0*(fcenter-int(fcenter)))
  ! Half-bandwidth in kHz of the active sample rate; was hardcoded 48
  ! (= 0.5 * 96 kHz). At wide-mode (256 kHz) this needs to be 128 so
  ! the baseband-audio ↔ analytical-frequency translation below stays
  ! aligned with the symspec FFT layout (savg is fftshifted: bin 0 =
  ! lower edge = -half_band_khz from dial, bin N/2 = DC = dial).
  half_band_khz=0.5*real(nrate_active)/1000.0
  mfa=nfa-nkhz_center+nint(half_band_khz)
  mfb=nfb-nkhz_center+nint(half_band_khz)
  mode_q65=nBaseSubmode
  nts_q65=2**(mode_q65-1)             !Q65 tone separation factor
  f0_selected=fselected - nkhz_center + half_band_khz

  if(nagain.le.1) then
     call timer('get_cand',0)
     ! Get a list of decoding candidates
     call getcand2(ss,savg,nts_q65,nagain,nhsym,ntx30a,ntx30b,ntol,     &
          f0_selected,bAlso30,cand,ncand2)
     call timer('get_cand',1)
  endif

  nwrite_q65=0
  df=real(nrate_active)/nfft_active   !df = active_rate/active_nfft = 2.930 Hz @ 96 kHz baseline
  ftol=0.010                          !Frequency tolerance (kHz)
  foffset=0.001*(1270 + nfcal)        !Offset from sync tone, plus CAL
  fqso=mousefqso + foffset - 0.5*(nfa+nfb) + nfshift !fqso at baseband (khz)
  nqd=0
  bClickDecode=(nagain.ge.1)

  ! CFOM temporarily DISABLED while debugging — call commented out;
  ! candidate-shift in the icand loop also gated off below.
  ! if (nCFOM .ne. 0 .and. ndop00 .ne. 0) then
  !    call cfom(dd, 0, nsmax_active, ndop00)
  ! endif

  call timer('fftbig  ',0)
  call fftbig(dd,nsmax_active) !Do the full-length FFT (was NSMAX=60*96000)
  call timer('fftbig  ',1)

  if(nagain.ge.2) then
     ncand2=1
     fqso=fselected
  endif

  do icand=1,ncand2                        !Attempt to decode each candidate
     tsec=sec_midn() - tsec0
     if(ndiskdat.eq.0) then
        ! No more realtime decode attempts if it's nearly too late, already
        if(nhsym.eq.130 .and. tsec.gt.6.0) exit
        if(nhsym.eq.200 .and. tsec.gt.10.0) exit
        if(nhsym.eq.330 .and. tsec.gt.6.0) exit
        if(nhsym.eq.390 .and. tsec.gt.16.0) exit
     endif
     f0=cand(icand)%f
     ntrperiod=cand(icand)%ntrperiod
     iseq=cand(icand)%iseq

     if(nagain.eq.0) then
        ! Skip this candidate if we already decoded it.
        do j=1,ndecodes
           if(abs(f0-found(j)%f).lt.0.005 .and.                             &
           ntrperiod.eq.found(j)%ntrperiod .and.                            &
           iseq.eq.found(j)%iseq) go to 10
        enddo
     endif

     ! Candidate-shift gated off in step with the cfom() call above
     ! while debugging.
     ! if (nCFOM .ne. 0 .and. ndop00 .ne. 0) then
     !    f0 = f0 + 0.0005 * real(ndop00)
     ! endif

     mode_q65_tmp=mode_q65
     if(ntrperiod.eq.30) mode_q65_tmp=max(1,mode_q65-1)
     freq=f0+nkhz_center-half_band_khz-1.27046
     ikhz=nint(freq)
     idec=-1
     call timer('q65b    ',0)
     call q65b(nutc,nqd,fcenter,nfcal,nfsample,ikhz,mousedf,ntol,           &
          ntrperiod,iseq,mycall,hiscall,hisgrid,mode_q65_tmp,f0,fqso,       &
          nkhz_center,newdat,nagain,bClickDecode,max_drift,offset,         &
          ndepth,datetime,nCFOM,ndop00,nhsym,idec)
     call timer('q65b    ',1)
     if(bClickDecode .and. idec.ge.0) exit
     if(idec.ge.0) then
        ! Save some details on good decodes, to avoid duplicated effort
        found(ndecodes)%f=f0
        found(ndecodes)%ntrperiod=ntrperiod
        found(ndecodes)%iseq=iseq
     end if
10   continue
  enddo  ! icand

  return
end subroutine qmapa
