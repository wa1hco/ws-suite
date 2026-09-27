logical function jpleph_ok()

! True if the JPL ephemeris file registered via jpl_setup (common/jplcom)
! exists, so callers can choose MoonDopJPL over the legacy analytic
! MoonDop. The check runs once, on first call; the result is cached.
! NUL bytes are blanked first because C callers hand jpl_setup a C string
! shorter than the declared character*256.

  character*256 jpleph_file_name
  common/jplcom/jpleph_file_name
  logical first,ok
  data first/.true./,ok/.false./
  save first,ok

  if(first) then
     first=.false.
     do i=1,256
        if(jpleph_file_name(i:i).eq.char(0)) jpleph_file_name(i:i)=' '
     enddo
     if(len_trim(jpleph_file_name).gt.0) then
        inquire(file=trim(jpleph_file_name),exist=ok)
     endif
  endif
  jpleph_ok=ok

  return
end function jpleph_ok
