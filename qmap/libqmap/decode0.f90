subroutine decode0(dd,ss,savg)

  use timer_module, only: timer
  use qmap_params, only: nrate_active, nfft_active, nsmax_active

! Dummy args: assumed-size — caller dimensions match the active runtime
! sizes. Was real*4 dd(2,NSMAX), ss(400,NFFT), savg(NFFT) at NSMAX=60*96000
! and NFFT=32768, which masked the wide-mode tail of the COMMON arrays.
  real*4 dd(2,*),ss(400,*),savg(*)
  real*8 fcenter
  integer offset
  integer hist(0:32768)
  logical*1 bAlso30
  character mycall*12,hiscall*12,mygrid*6,hisgrid*6,datetime*20
  character mycall0*12,hiscall0*12,hisgrid0*6
  character*72 result
  common/decodes/ndecodes,ncand2,nQDecoderDone,nWDecoderBusy,              &
       nWTransmitting,kHzRequested,result(50)
  common/npar/fcenter,nutc,fselected,mousedf,mousefqso,nagain,            &
       ndepth,ndiskdat,ntx60,newdat,nfa,nfb,nfcal,nfshift,                &
       ntx30a,ntx30b,ntol,n60,nCFOM,nfsample,ndop58,nmode,                 &
       ndop00,nsave,max_drift,offset,nhsym,mycall,mygrid,                 &
       hiscall,hisgrid,datetime,junk1,junk2,bAlso30
  save

  nQDecoderDone=0
  if(newdat.ne.0) then
     nz=nrate_active*nhsym*0.15            !samples spanning nhsym half-symbols at active rate; was 96000*...
     hist=0
     do i=1,nz
        j1=min(abs(dd(1,i)),32768.0)
        hist(j1)=hist(j1)+1
        j2=min(abs(dd(2,i)),32768.0)
        hist(j2)=hist(j2)+1
     enddo
     m=0
     do i=0,32768
        m=m+hist(i)
        if(m.ge.2*nz) go to 10
     enddo
10   rmsdd=1.5*i
  endif
      
  mycall0=mycall
  hiscall0=hiscall
  hisgrid0=hisgrid

  if(sum(dd(:,1:nsmax_active)).eq.0.0 .or. sum(ss(:,1:nfft_active)).eq.0.0 .or. sum(savg(1:nfft_active)).eq.0.0) return
  call timer('qmapa   ',0)
  call qmapa(dd,ss,savg,newdat,nutc,fcenter,ntol,nfa,nfb,                  &
       mousedf,mousefqso,nagain,ntx30a,ntx30b,nfshift,max_drift,offset,    &
       nfcal,mycall,hiscall,hisgrid,nfsample,nmode,ndepth,                 &
       datetime,ndop00,fselected,bAlso30,nhsym,NCFOM)
  call timer('qmapa   ',1)

  return
end subroutine decode0
