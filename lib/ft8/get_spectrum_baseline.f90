subroutine get_spectrum_baseline(dd,nfa,nfb,sbase)

  include 'ft8_params.f90'
  parameter(NST=NFFT1/2,NF=93)              !NF=NMAX/NST-1
  real s(NH1,NF)
  real savg(NH1)
  real savsm(NH1)
  real sbase(NH1)
  real x(NFFT1)
  real window(NFFT1)
  complex cx(0:NH1)
  real dd(NMAX)
  equivalence (x,cx)
  logical first
  data first/.true./
  save first,window

  if(first) then
    first=.false.
    pi=4.0*atan(1.)
    window=0.
    call nuttal_window(window,NFFT1)
    window=window/sum(window)*NSPS*2/300.0
  endif

! Compute symbol spectra, stepping by NSTEP steps.  
  savg=0.
  df=12000.0/NFFT1  
  do j=1,NF
     ia=(j-1)*NST + 1
     ib=ia+NFFT1-1
     if(ib.gt.NMAX) exit
     x=dd(ia:ib)*window
     call four2a(x,NFFT1,1,-1,0)              !r2c FFT
     s(1:NH1,j)=abs(cx(1:NH1))**2
     savg=savg + s(1:NH1,j)                   !Average spectrum
  enddo

  nwin=nfb-nfa
  if(nfa.lt.100) then
     nfa=100
     if(nwin.lt.100) then ! nagain
        nfb=nfa+nwin  
     endif
  endif
  if(nfb.gt.4910) then
     nfb=4910
     if(nwin.lt.100) then 
        nfa=nfb-nwin
     endif
  endif

! Detect receiver filter edges from a smoothed spectrum.
! Trim nfa/nfb to exclude the filter rolloff region.
  ia=max(1,nint(nfa/df))
  ib=nint(nfb/df)
  savsm=0.
  do i=8,NH1-7
     savsm(i)=sum(savg(i-7:i+7))/15.
  enddo
  npts_filt=ib-ia+1
  if(npts_filt.gt.40) then
     nq1=ia+npts_filt/4
     nq2=ib-npts_filt/4
     call pctile(savsm(nq1),nq2-nq1+1,30,spassband)
     if(spassband.gt.0.0) then
        sthresh=spassband*0.01                 !20 dB below passband noise
        ia_det=ia
        ib_det=ib
        do i=ia,ib
           if(savsm(i).ge.sthresh) then
              ia_det=i
              exit
           endif
        enddo
        do i=ib,ia,-1
           if(savsm(i).ge.sthresh) then
              ib_det=i
              exit
           endif
        enddo
        if(ia_det.lt.ib_det) then
           nfa=nint(ia_det*df)
           nfb=nint(ib_det*df)
        endif
     endif
  endif

  call baseline(savg,nfa,nfb,sbase)

return
end subroutine get_spectrum_baseline
