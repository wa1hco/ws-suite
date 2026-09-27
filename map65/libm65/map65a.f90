module map65a_mod
   use txpol_mod
   use trimlist_mod
   use iso_fortran_env, only: real64
   use debug_log
   implicit none
contains

   subroutine map65a(dd, newdat, nutc, fcenter, ntol, idphi, nfa, nfb, &
                     mousedf, mousefqso, nagain, ndecdone, nfshift, ndphi, max_drift, &
                     nfcal, nkeep, mcall3b, nsum, nsave, nxant, mycall, mygrid, &
                     neme, ndepth, nstandalone, hiscall, hisgrid, nhsym, nfsample, &
                     ndiskdat, nxpol, nmode, ndop00)
!  Processes timf2 data from Linrad to find and decode JT65 signals.
      use iso_c_binding
      use wideband_sync
      use timer_module, only: timer
      use debug_log, only: dbg, itoa, rtoa
      use q65b_mod
      use decode1a_mod
      use ccf65_legacy_mod
      use pctile_mod
      use stdout_channel_mod, only: write_stdout
      use decodes_mod, only: nhsym1, nhsym2, ldecoded, ljt65decoded, ndecodes, mcall3a, decodes_init
      use display_mod
      use timf2_mod
      use getdphi_mod
      use datcom_ptrs_mod, only: ss => ss_dec, savg => savg_dec   ! decoder reads the SNAPSHOT
      use npar_ptrs_mod,  only: nsmax_active, nrate_active, nfft_active, t_start, abort_decode, &
                                call3_path
      use sec0_mod, only: sec0

      implicit none
    
      integer, parameter :: MAXMSG = 1000            !Size of decoded message list
      real,    intent(in)    :: dd(4, nsmax_active)
      integer, intent(inout) :: newdat
      integer, intent(inout) :: nutc
      real(real64),  intent(in)    :: fcenter
      integer, intent(in)    :: ntol, idphi, nfa, nfb
      integer, intent(in)    :: mousedf, mousefqso, nagain
      integer, intent(inout) :: ndecdone
      integer, intent(in)    :: nfshift
      integer, intent(inout) :: ndphi
      integer, intent(in)    :: max_drift, nfcal, nkeep
      integer, intent(inout) :: mcall3b
      integer, intent(inout) :: nsum, nsave
      integer, intent(in)    :: nxant
      character(len=12), intent(in) :: mycall, hiscall
      character(len=6),  intent(in) :: mygrid, hisgrid
      integer, intent(in)    :: neme, ndepth, nstandalone
      integer, intent(in)    :: nhsym, nfsample, ndiskdat, nxpol, nmode, ndop00

      real tavg(-50:50)                  !Temp for finding local base level
      real base(4)                       !Local basel level at 4 pol'ns
      real sig(MAXMSG, 30)                !Parameters of detected signals
      real a(5)
      real :: df, dphi, dt, dt2, fa, fb, flip, flipk, foffset, freq, freq0, fshort
      real :: ftol, pol, qual, s2db, smax, snr2, ssmax, sync1, sync10, sync2, syncshort
      real :: tdec, thresh0, thresh1, tsec0, fshort0, fqso, syncshort0
      real(real64) :: f0
      character(len=22) msg(MAXMSG)
      character(len=3) shmsg0(4)
      character(len=1) :: cp, cm
      integer indx(MAXMSG), nsiz(MAXMSG)
      integer ibin(MAXMSG)                !symspec bin each sig()/msg() came from
      integer nbtol, jb                   !JT65 dupe-guard half-width (bins), loop var
      integer :: ipol, mode65
      integer :: i, ia, ib, i0, icand, idf, ifile, ifile0, ifreq, ii, iii, ikhz, ilatest, iloop
      integer :: ip000, ip001, ipol2, j, jp, jpmax, jpz, k, km, m, mfa, mfb, mhz, mode_q65
      integer :: mousefqso0, n, ncand, ndf, ndf0, ndf1, ndf2, nfile, nflip, nhist
      integer :: nhzdiff, nid, nkhz, nkm, nkv, noffset, npol, nqd, nqual, nsync1
      integer :: nsync2, ntry, nts_jt65, nts_q65, ntxpol, nutc0, nwrite, nwrite_q65, nz
      integer :: idec
      logical done(MAXMSG)
      logical xpol, bq65, q65b_called
      logical candec(MAX_CANDIDATES)
      character(len=22) decoded
      character(len=22) blank 
      character(len=2) cmode
      real short(3, nfft_active)                 !SNR dt ipol for potential shorthands
      real qphi(12)
      type(candidate) :: cand(MAX_CANDIDATES)
      real(real64) :: f00
      character(len=256) :: line

      ! line below for debug only
      ! character(len=64) :: sfa, sfb, sspan

      integer :: t_now, t_rate

      logical :: m65_inited = .false.     ! added with modernization
      integer :: icenter
      data blank/'                      '/, cm/'#'/
      data shmsg0/'ATT', 'RO ', 'RRR', '73 '/
      data nfile/0/, nutc0/-999/, nid/0/, ip000/1/, ip001/1/, mousefqso0/-999/
      save

      abort_decode = .false.

      if (.not. m65_inited) then
         call decodes_init()
         call init_wideband_sync()
         m65_inited = .true.
      endif
     
! Vestigial "rewind 12" removed: nothing in the decoder uses unit 12 any
! more, and in a single-process build a low fixed unit can belong to
! someone else entirely -- the JPL ephemeris sat on unit 12 (pre-93 fix)
! and this rewind corrupted its position, so the next Doppler update hit
! STATE's error path and its STOP killed the whole application cleanly
! ("window closes by itself").
      ndecodes = 0

      ipol = 1
  
! Clean start for Q65 at early decode
      if (nhsym .eq. nhsym1 .or. nagain .ne. 0) ldecoded = .false.
      if (ndiskdat .eq. 1) ldecoded = .false.
! Same lifetime for the JT65 guard: cleared at the first pass of a period
! (and for click/disk decodes), retained into the second pass so a signal
! already decoded at t~52 s is not decoded and printed again at t~56 s.
      if (nhsym .eq. nhsym1 .or. nagain .ne. 0) ljt65decoded = .false.
      if (ndiskdat .eq. 1) ljt65decoded = .false.

      nkhz_center = nint(1000.0*(fcenter - int(fcenter)))
      mfa = nfa - nkhz_center + int(nrate_active/2000.0)
      mfb = nfb - nkhz_center + int(nrate_active/2000.0)
    !  mfa = nfa - nkhz_center + 48
    !  mfb = nfb - nkhz_center + 48
      mode65 = mod(nmode, 10)
      if (mode65 .eq. 3) mode65 = 4
      mode_q65 = nmode/10      
      nts_jt65 = mode65                     !JT65 tone separation factor
      nts_q65 = 2**(mode_q65 - 1)             !Q65 tone separation factor
      xpol = (nxpol .ne. 0)

! No second decode for JT65? Can't use this guard with modern code
!      if (nhsym .eq. nhsym2 .and. nagain .eq. 0 .and. ndiskdat .eq. 0) mode65 = 0

      if (nagain .eq. 0) then
         call timer('get_cand', 0)
         call get_candidates(ss, savg, xpol, nhsym, mfa, mfb, nts_jt65, nts_q65, cand, ncand)
         call timer('get_cand', 1)
         candec = .false.
      endif
      nwrite_q65 = 0
      bq65 = mode_q65 .gt. 0
      
      mcall3a = mcall3b
      mousefqso0 = mousefqso
      if (.not. xpol) ndphi = 0
      nsum = 0

! CALL3.TXT: use the GUI-supplied full path when set (Settings ->
! CALL3.TXT file), else fall back to the historical bare relative open,
! which resolves against the process cwd = the data dir. deep65 reads
! this same unit 23, so decoder deep search and the GUI's lookup /
! Add-to-CALL3 always operate on one and the same file.
      if (len_trim(call3_path) .gt. 0) then
         open (23, file=trim(call3_path), status='unknown')
      else
         open (23, file='CALL3.TXT', status='unknown')
      endif

      df = real(nrate_active)/real(nfft_active)    !df = bin width = 2.930 Hz
      
      !  call dbg('map65a: df=' // rtoa(df) // ' nfft_active=' // itoa(nfft_active) // &
      !   ' nrate_active=' // itoa(nrate_active))
      
      if (nfsample .eq. 95238) df = 95238.1/real(nfft_active)
      ftol = 0.010                          !Frequency tolerance (kHz)
! Half-width, in symspec bins, of the JT65 cross-pass dupe guard. The sync
! peak of one signal can land a bin or two apart between passes (different
! amount of data), so marking a single bin would leak duplicates; ftol is
! the same +/-10 Hz "this is the same signal" tolerance the in-pass
! best-candidate logic below already uses.
      nbtol = max(1, nint(1000.0*ftol/df))
      dphi = idphi/57.2957795
      foffset = 0.001*(1270 + nfcal)              !Offset from sync tone, plus CAL
      fqso = mousefqso + foffset - 0.5*(nfa + nfb) + nfshift !fqso at baseband (khz)
      iloop = 0

2     if (ndphi .eq. 1) dphi = 30*iloop/57.2957795

      if (nutc .ne. nutc0) nfile = nfile + 1
      nutc0 = nutc

      km = 0            !decode list spans both passes now -- see the note below

      do nqd = 1, 0, -1

         call system_clock(t_now, t_rate)
         if (real(t_now - t_start)/real(t_rate) > 40.0) then
            call write_stdout('Decode budget (40 s) exhausted before pass ' // &
                              itoa(nqd) // ' -- wideband decodes skipped this period' // &
                              new_line('a'))
            abort_decode = .true.
            go to 700
         endif

   !  call dbg('MAP65A: starting pass with nqd=' // itoa(nqd))

         if (nqd .eq. 1) then                     !Quick decode, at fQSO
            fa = 1000.0*(fqso + 0.001*mousedf) - ntol
            fb = 1000.0*(fqso + 0.001*mousedf) + ntol + 4*(nrate_active/1783.0)
         else                                  !Wideband decode at all freqs
            fa = -1000*0.5*(nfb - nfa) + 1000*nfshift
            fb = 1000*0.5*(nfb - nfa) + 1000*nfshift

         ! Debug: report JT65 wideband search window

!write(sfa, '(F20.6)') fa
!write(sfb, '(F20.6)') fb
!write(sspan, '(F20.6)') (fb-fa)/1000.0

!  call dbg('JT65 wideband: fa=' // trim(sfa) // ' fb=' // trim(sfb) // &
!         ' span_kHz=' // trim(sspan))

         endif
         icenter = nfft_active/2 + 1
         ia = nint(fa/df) + icenter
         ib = nint(fb/df) + icenter
         ia = max(51, ia)
         ib = min(nfft_active - 51, ib)
         if (ndiskdat .eq. 1 .and. mode65 .eq. 0) ib = ia

! Accumulate the decode list across BOTH passes. trimlist() below groups
! entries within ftol and the block after label 700 writes exactly ONE line
! per group, so a signal found by pass 1 and again by pass 2 collapses to a
! single line -- the dupe protection is already there. Resetting km per pass
! instead threw pass 1's decodes away before display() ever saw them: they
! reached the main window (their "!" line is written in-pass at :473) but
! never the Messages window, map65_rx.log or the forward to WSJT-X. That is
! the "dial sitting on the DX" case, i.e. every normal QSO.
!
! Perversely, an aborted period showed MORE than a clean one: "go to 700"
! preserves whatever km holds, so pass-1 decodes survived an abort and were
! discarded only when pass 2 ran to completion.
         if (nqd .eq. 1) km = 0
         nkm = 1
         nz = n/8
         freq0 = -999.
         sync10 = -999.
         fshort0 = -999.
         syncshort0 = -999.
         ntry = 0
         short = 0.                                 !Zero the whole short array
         jpz = 1
         if (xpol) jpz = 4

         do i = ia, ib                               !Search over freq range
     
         call system_clock(t_now, t_rate)
         if (real(t_now - t_start)/real(t_rate) > 40.0) then
            call write_stdout('Decode budget (40 s) exhausted during pass ' // &
                              itoa(nqd) // ' frequency scan -- period truncated' // &
                              new_line('a'))
            abort_decode = .true.
            go to 700
         endif

            freq = 0.001*(i - icenter)*df
!  Find the local base level for each polarization; update every 10 bins.
            if (mod(i - ia, 10) .eq. 0) then
               do jp = 1, jpz
                  do ii = -50, 50
                     iii = i + ii
                     if (iii .ge. 1 .and. iii .le. nfft_active) then
                        tavg(ii) = savg(jp, iii)
                     else
                        write (13, *) 'Error in iii:', iii, ia, ib, fa, fb
                        flush (13)
                        go to 900
                     endif
                  enddo
                  call pctile(tavg, 101, 50, base(jp))
               enddo
            endif

!  Find max signal at this frequency
            smax = 0.
            jpmax = 1
            do jp = 1, jpz
               if (savg(jp, i)/base(jp) .gt. smax) then
                  smax = savg(jp, i)/base(jp)
                  jpmax = jp
               endif
            enddo
            
! Cross-pass dupe guard: this bin already yielded a JT65 decode earlier in
! the same period (pass 1 at t~52 s), so skip it -- otherwise pass 2 finds
! the same signal again and prints a second, identical line. Before
! 40e65b4 the whole JT65 path was disabled in pass 2, which suppressed the
! duplicates but also lost signals only pass 2 can hear; guarding per bin
! keeps pass 2 useful. Skipping before ccf65 also shortens the pass-2 scan.
            if (mode65 .ne. 0 .and. nagain .eq. 0 .and. ljt65decoded(i)) cycle

            if (smax .gt. 1.1 .or. ia .eq. ib) then

!  Look for JT65 sync patterns and shorthand square-wave patterns.
               call timer('ccf65   ', 0)
               ssmax = 1.e30
               call ccf65(ss(:,:,i), nhsym, ssmax, sync1, ipol, jpz, dt, flipk, &
                  syncshort, snr2, ipol2, dt2)
               call timer('ccf65   ', 1)
               if (mode65 .eq. 0) syncshort = -99.0     !If "No JT65", don't waste time

! ########################### Search for Shorthand Messages #################
!  Is there a shorthand tone above threshold?
               thresh0 = 1.0
!  Use lower thresh0 at fQSO
               if (nqd .eq. 1 .and. ntol .le. 100) thresh0 = 0.
               if (syncshort .gt. thresh0) then
! ### Do shorthand AFC here (or maybe after finding a pair?) ###
                  short(1, i) = syncshort
                  short(2, i) = dt2
                  short(3, i) = ipol2

!  Check to see if lower tone of shorthand pair was found.
                  do j = 2, 4
                     i0 = i - nint(j*mode65*10.0*(11025.0/4096.0)/df)
!  Should this be i0 +/- 1, or just i0?
!  Should we also insist that difference in DT be either 1.5 or -1.5 s?
                     if (short(1, i0) .gt. thresh0) then
                        fshort = 0.001*(i0 - 16385)*df
                        noffset = 0
                        if (nqd .eq. 1) noffset = nint(1000.0*(fshort - fqso) - mousedf)
                        if (abs(noffset) .le. ntol) then
!  Keep only the best candidate within ftol.
!### NB: sync2 was not defined here!
!                       sync2=syncshort                   !### try this ???
                           if (fshort - fshort0 .le. ftol .and. &
                            syncshort.gt.syncshort0 .and. nkm.eq.2) km=km-1
                           if (fshort - fshort0 .gt. ftol .or. &
                               syncshort .gt. syncshort0) then
                              if (km .lt. MAXMSG) km = km + 1
                              sig(km, 1) = nfile
                              sig(km, 2) = nutc
                              sig(km, 3) = fshort + 0.5*(nfa + nfb)
                              sig(km, 4) = syncshort
                              sig(km, 5) = dt2
                              sig(km, 6) = 45*(ipol2 - 1)/57.2957795
                              sig(km, 7) = 0
                              sig(km, 8) = snr2
                              sig(km, 9) = 0
                              sig(km, 10) = 0
!                           sig(km,11)=rms0
                              sig(km, 12) = savg(ipol2, i)
                              sig(km, 13) = 0
                              sig(km, 14) = 0
                              sig(km, 15) = 0
                              sig(km, 16) = 0
!                           sig(km,17)=0
                              sig(km, 18) = 0
                              msg(km) = shmsg0(j)
                             
                              fshort0 = fshort
                              syncshort0 = syncshort
                              nkm = 2
                           endif
                        endif
                     endif
                  enddo
               endif

! ########################### Search for Normal Messages ###########
!  Is sync1 above threshold?
               thresh1 = 1.0
!  Use lower thresh1 at fQSO
               if (nqd .eq. 1 .and. ntol .le. 100) thresh1 = 0.
               noffset = 0
               if (nqd .ge. 1) noffset = nint(1000.0*(freq - fqso) - mousedf)
               if (newdat .eq. 1 .and. sync1 .gt. -99.0) then
                  sync1 = thresh1 + 1.0
                  noffset = 0
               endif
               if (sync1 .gt. thresh1 .and. abs(noffset) .le. ntol) then
!  Keep only the best candidate within ftol.
!  (Am I deleting any good decodes by doing this?)
              if(freq-freq0.le.ftol .and. sync1.gt.sync10 .and.       &
                   nkm.eq.1) km=km-1
                  if (freq - freq0 .gt. ftol .or. sync1 .gt. sync10) then
                     nflip = nint(flipk)
                     f00 = (i - 1)*df          !Freq of detected sync tone (0-96000 Hz)
                     ntry = ntry + 1
                     if ((nqd .eq. 1 .and. ntry .ge. 40) .or. &
                         (nqd .eq. 0 .and. ntry .ge. 400)) then
  ! Too many calls to decode1a!
                        call write_stdout('! Signal too strong, or suspect data?  Decoding aborted.'//new_line('a'))
                        write (13, *) 'Signal too strong, or suspect data?  Decoding aborted.'
                        flush (13)
! 700, not 900: bailing out of the scan is no reason to discard the decodes
! already in the list. 900 skipped trimlist/display entirely, so one strong
! signal cost the whole period its Messages/log output.
                        go to 700
                     endif
                     
 !    call dbg('JT65: trying cand ' // itoa(i) // ' snr2=' // rtoa(snr2))

                     call timer('decode1a', 0)
                     ifreq = i
                     ikhz = nint(freq + 0.5*(nfa + nfb) - foffset) - nfshift
                     idf = nint(1000.0*(freq + 0.5*(nfa + nfb) - foffset - (ikHz + nfshift)))
                     call decode1a(dd, newdat, f00, nflip, mode65, nfsample, &
                                   xpol, mycall, hiscall, hisgrid, neme, ndepth, nqd, dphi, &
                                   ndphi, nutc, ikHz, idf, ipol, ntol, sync2, &
                                   a, dt, pol, nkv, nhist, nsum, nsave, qual, decoded)
                     call timer('decode1a', 1)
                     
                     ! !  call dbg('JT65: decode1a returned: decoded="' // decoded // '" dt=' // rtoa(dt) // &
!         ' sync2=' // rtoa(sync2) // ' qual=' // rtoa(qual))


! The case sync1=2.0 is just to make sure decode1a is called and bigfft done.
                     if (mode65 .ne. 0 .and. sync1 .ne. 2.000000) then
                        if (km .lt. MAXMSG) km = km + 1
                        sig(km, 1) = nfile
                        sig(km, 2) = nutc
                        sig(km, 3) = freq + 0.5*(nfa + nfb)
                        sig(km, 4) = sync1
                        sig(km, 5) = dt
                        sig(km, 6) = pol
                        sig(km, 7) = flipk
                        sig(km, 8) = sync2
                        sig(km, 9) = nkv
                        sig(km, 10) = qual
!                    sig(km,11)=idphi
                        sig(km, 12) = savg(ipol, i)
                        sig(km, 13) = a(1)
                        sig(km, 14) = a(2)
                        sig(km, 15) = a(3)
                        sig(km, 16) = a(4)
!                     sig(km,17)=a(5)
                        sig(km, 18) = nhist
                        ibin(km) = i            !bin this candidate came from
                        msg(km) = decoded
                        freq0 = freq
                        sync10 = sync1
                        nkm = 1
                     endif
                  endif
               endif
            endif
         enddo  !i=ia,ib
         
         if (nqd .eq. 1) then
            nwrite = 0
            if (mode65 .eq. 0) km = 0
            
            !  call dbg('map65a: km=' // itoa(km) // ' (number of decoded messages)')
            
            do k = 1, km
               decoded = msg(k)
               if (decoded .ne. '                      ') then
! A genuine decode (blank messages are candidates that produced nothing):
! mark its bin, +/- nbtol, so the next pass in this period does not decode
! and print the same signal a second time.
                  do jb = max(1, ibin(k) - nbtol), min(nfft_active, ibin(k) + nbtol)
                     ljt65decoded(jb) = .true.
                  enddo
                  nutc = sig(k, 2)
                  freq = sig(k, 3)
                  sync1 = sig(k, 4)
                  dt = sig(k, 5)
                  npol = nint(57.2957795*sig(k, 6))
                  flip = sig(k, 7)
                  sync2 = sig(k, 8)
                  nkv = sig(k, 9)
                  nqual = sig(k, 10)
!              idphi=nint(sig(k,11))
                  if (flip .lt. 0.0) then
                     do i = 22, 1, -1
                        if (decoded(i:i) .ne. ' ') go to 8
                     enddo
                     stop 'Error in message format'
8                    if (i .le. 18) decoded(i + 2:i + 4) = 'OOO'
                  endif
                  nkHz = nint(freq - foffset) - nfshift
                  mhz = fcenter                         ! ... +fadd ???
                  f0 = mhz + 0.001*nkHz
                  ndf = nint(1000.0*(freq - foffset - (nkHz + nfshift)))
                  nsync1 = sync1
                  s2db = 10.0*log10(sync2) - 40             !### empirical ###
                  nsync2 = nint(s2db)
                  if (decoded(1:4) .eq. 'RO  ' .or. decoded(1:4) .eq. 'RRR  ' .or. &
                      decoded(1:4) .eq. '73  ') then
                     nsync2 = nint(1.33*s2db + 2.0)
                  endif

                  nwrite = nwrite + 1
                  if (nxant .ne. 0) then
                     npol = npol - 45
                     if (npol .lt. 0) npol = npol + 180
                  endif

                  call txpol(xpol, decoded, mygrid, npol, nxant, ntxpol, cp)
                  if (ndphi .eq. 0) then
                     write (line, '("!",I3,I5,I4,I6.4,F5.1,I5,1X,A1,1X,A22,I2,I5,I5,1X,A1)') &
                        nkHz, ndf, npol, nutc, dt, nsync2, cm, decoded, nkv, nqual, ntxpol, cp
                     call write_stdout(trim(line)//new_line('a'))
                  else
                     if (iloop .ge. 1) qphi(iloop) = sig(k, 10)
                     
                     !  call dbg('map65a: DECODED k=' // itoa(k) // ' decoded="' // decoded // '"')
                     
                     write (line, '("!",I3,I5,I4,I6.4,F5.1,I5,1X,A1,1X,A22,I2,I5,I5,1X,A1)') &
                        nkHz, ndf, npol, nutc, dt, nsync2, cm, decoded, nkv, nqual, 30*iloop
                     call write_stdout(trim(line)//new_line('a'))
                     write (27, 1011) 30*iloop, nkHz, ndf, npol, nutc, &
                        dt, sync2, nkv, nqual, cm, decoded
1011                 format(i3, i4, i5, i4, i6.4, 1x, f5.1, f7.1, i3, i5, a1, 1x, a22)
                  endif
               endif
            enddo  ! k=1,km

            if (bq65) then
               q65b_called = .false.
               do icand = 1, ncand
                  if (cand(icand)%iflip .ne. 0) cycle        !Keep only Q65 candidates
                  freq = cand(icand)%f + nkhz_center - real(nrate_active)/2000.0 - 1.27046
                  nhzdiff = nint(1000.0*(freq - mousefqso) - mousedf) - nfcal
! Now looking for "quick decode" (nqd=1) candidates at cursor freq +/- ntol.
                  if (nqd .eq. 1 .and. abs(nhzdiff) .gt. ntol) cycle
                  ikhz = mousefqso
                  q65b_called = .true.
                  f0 = cand(icand)%f
                  call timer('q65b    ', 0)
                  
                  !  call dbg('Q65: 1 trying cand at ' // itoa(icand) // ' f0=' // rtoa(real(f0)) // &
        ! ' snr2=' // rtoa(snr2))
         
               
                  call q65b(nutc, nqd, nxant, fcenter, nfcal, nfsample, ikhz, mousedf, &
                            ntol, xpol, mycall, mygrid, hiscall, hisgrid, mode_q65, f0, fqso, &
                            newdat, nagain, max_drift, ndop00, idec)
                  call timer('q65b    ', 1)
                  if (idec .ge. 0) candec(icand) = .true.
               enddo
               if (.not. q65b_called) then
                  freq = mousefqso + 0.001*mousedf
                  ikhz = mousefqso
                  f0 = freq - (nkhz_center - real(nrate_active)/2000.0 - 1.27046)
                  call timer('q65b    ', 0)
                  
                  !  call dbg('Q65: 2 trying cand at ' // itoa(icand) // ' f0=' // rtoa(real(f0)) // &
      !   ' snr2=' // rtoa(snr2))
         
                  call q65b(nutc, nqd, nxant, fcenter, nfcal, nfsample, ikhz, mousedf, &
                            ntol, xpol, mycall, mygrid, hiscall, hisgrid, mode_q65, f0, fqso, &
                            newdat, nagain, max_drift, ndop00, idec)
                  call timer('q65b    ', 1)
               endif
            endif

            if (nwrite .eq. 0 .and. nwrite_q65 .eq. 0) then
               write (line, '("!",I3,9X,I6.4,"  ")') mousefqso, nutc
               call write_stdout(trim(line)//new_line('a'))
            endif
         endif  !nqd.eq.1

         if (ndphi .eq. 1 .and. iloop .lt. 12) then
            iloop = iloop + 1
            go to 2
         endif

         if (ndphi .eq. 1 .and. iloop .eq. 12) call getdphi(qphi)
         if (nqd .eq. 1) then

            call sec0(1, tdec)
            write (line, '("<QuickDecodeDone>",3I4,I6,F6.2)') &
               nsum, nsave, nstandalone, nhsym, tdec

            call write_stdout(trim(line)//new_line('a'))

            open (16, file='tquick.dat', status='unknown', position='append')
            write (16, 1016) nutc, tdec
1016        format(i4.4, f7.1)
            close (16)
         endif

         call sec0(1, tsec0)
         if (nhsym .eq. nhsym1 .and. tsec0 .gt. 3.0) go to 700
! Was "go to 900", which jumps PAST trimlist/display: a clicked re-decode that
! succeeded in the quick pass printed its "!" line and then vanished from the
! Messages window and the log. 700 is the same early exit the 40 s guard uses
! and keeps whatever has been decoded.
         if (nqd .eq. 1 .and. nagain .eq. 1) go to 700

         if (nqd .eq. 0 .and. bq65) then
! Do the wideband Q65 decode
            do icand = 1, ncand
               if (cand(icand)%iflip .ne. 0) cycle    !Do only Q65 candidates here
               if (candec(icand)) cycle             !Skip if already decoded
               freq = cand(icand)%f + nkhz_center - real(nrate_active)/2000.0 - 1.27046
!###! If here at nqd=1, do only candidates at mousefqso +/- ntol
!###           if(nqd.eq.1 .and. abs(freq-mousefqso).gt.0.001*ntol) cycle
               ikhz = nint(freq)
               f0 = cand(icand)%f
               call timer('q65b    ', 0)
                  
                  !  call dbg('Q65: 3 trying cand at ' // itoa(icand) // ' f0=' // rtoa(real(f0)) // &
      !   ' snr2=' // rtoa(snr2))
         
               call q65b(nutc, nqd, nxant, fcenter, nfcal, nfsample, ikhz, mousedf, ntol, &
                         xpol, mycall, mygrid, hiscall, hisgrid, mode_q65, f0, fqso, newdat, &
                         nagain, max_drift, ndop00, idec)
               call timer('q65b    ', 1)
               if (idec .ge. 0) candec(icand) = .true.
               if (abort_decode) go to 700
            enddo  ! icand
         endif
         
         call sec0(1, tsec0)

         call system_clock(t_now, t_rate)
         if (real(t_now - t_start)/real(t_rate) > 40.0) then
         !   call dbg('Decode abort: exceeded 40 seconds at end of do nqd = 1, 0, -1 pass')
            abort_decode = .true.
            go to 700
         endif

      enddo  ! nqd

!  Trim the list and produce a sorted index and sizes of groups.
!  (Should trimlist remove all but best SNR for given UTC and message content?)

700   continue
      if (km < 0) km = 0      ! single safety clamp
      call trimlist(sig, km, ftol, indx, nsiz, nz)
      if (km .gt. 0) done(1:km) = .false.
      j = 0
      ilatest = -1
      do n = 1, nz
         ifile0 = 0
         do m = 1, nsiz(n)
            i = indx(j + m)
            ifile = sig(i, 1)
            if (ifile .gt. ifile0 .and. msg(i) .ne. blank) then
               ilatest = i
               ifile0 = ifile
            endif
         enddo
         i = ilatest

         if (i .ge. 1) then
            if (.not. done(i)) then
            
            !  call dbg('Q65: decoded cand ' // itoa(icand))
            
               done(i) = .true.
               nutc = sig(i, 2)
               freq = sig(i, 3)
               sync1 = sig(i, 4)
               dt = sig(i, 5)
               npol = nint(57.2957795*sig(i, 6))
               flip = sig(i, 7)
               sync2 = sig(i, 8)
               nkv = sig(i, 9)
               nqual = min(sig(i, 10), 10.0)
!                  rms0=sig(i,11)
               do k = 1, 5
                  a(k) = sig(i, 12 + k)
               enddo
               nhist = sig(i, 18)
               decoded = msg(i)

               if (flip .lt. 0.0) then
                  do i = 22, 1, -1
                     if (decoded(i:i) .ne. ' ') go to 10
                  enddo
                  stop 'Error in message format'
10                if (i .le. 18) decoded(i + 2:i + 4) = 'OOO'
               endif
               mhz = fcenter                             !... +fadd ???
               nkHz = nint(freq - foffset) - nfshift
               f0 = mhz + 0.001*nkHz
               ndf = nint(1000.0*(freq - foffset - (nkHz + nfshift)))
               ndf0 = nint(a(1))
               ndf1 = nint(a(2))
               ndf2 = nint(a(3))
               nsync1 = sync1

               s2db = 10.0*log10(sync2) - 40             !### empirical ###
               nsync2 = nint(s2db)
               if (decoded(1:4) .eq. 'RO  ' .or. decoded(1:4) .eq. 'RRR  ' .or. &
                   decoded(1:4) .eq. '73  ') then
                  nsync2 = nint(1.33*s2db + 2.0)
               endif

               if (nxant .ne. 0) then
                  npol = npol - 45
                  if (npol .lt. 0) npol = npol + 180
               endif

               call txpol(xpol, decoded, mygrid, npol, nxant, ntxpol, cp)
               cmode = '#A'
               if (mode65 .eq. 2) cmode = '#B'
               if (mode65 .eq. 4) cmode = '#C'
               write (26, 1014) f0, ndf, ndf0, ndf1, ndf2, dt, npol, nsync1, &
                  nsync2, nutc, decoded, '#', cp, cmode ! was decoded,cp,
1014           format(f8.3, i5, 3i3, f5.1, i4, i3, i4, i5.4, 4x, a22, 7x, 2a1, 2x, a2) ! was a22,2x,a1,3x,a2
               ndecodes = ndecodes + 1
               write (21, 1100) f0, ndf, dt, npol, nsync2, nutc, decoded, '#', cp, &
                  cmode(1:1), cmode(2:2)! was decoded,cp,
1100           format(f8.3, i5, f5.1, 2i4, i5.4, 2x, a22, 7x, 2a1, 3x, a1, 1x, a1) ! was a22,2x,a1,1x,a1
            endif

         endif
         j = j + nsiz(n)
      enddo  !i=1,km

      write (26, 1015) nutc
1015  format(37x, i6.4, ' ')
      flush (21)
      flush (26)
      call display(nkeep, ftol)
      ndecdone = 2

900   close (23)
      flush (12)
      ndphi = 0
      mcall3b = mcall3a

      return
   end subroutine map65a

end module map65a_mod
