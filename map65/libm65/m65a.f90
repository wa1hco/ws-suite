module m65a_mod
  implicit none
contains

subroutine m65a()

  use timer_module, only: timer
  use, intrinsic :: iso_c_binding, only: C_NULL_CHAR
  use npar_ptrs_mod
  use datcom_ptrs_mod, only: dd => dd_dec, ss => ss_dec, savg => savg_dec   ! decoder reads the SNAPSHOT
  use m65_mod
  use FFTW3
  use ftninit_mod
  
  implicit none
  
  character(len=256) cwd

  ! Data dir handed over by the GUI via set_wsjtx_dir; the env-var
  ! fallbacks only apply when it was never set (e.g. standalone use).
  cwd = trim(wsjtx_dir)

  ! Try Unix-style PWD first
  if (len_trim(cwd) == 0) then
     call get_environment_variable("PWD", cwd)
  end if

  ! If empty, try Windows-style CD
  if (len_trim(cwd) == 0) then
     call get_environment_variable("CD", cwd)
  end if

  ! If still empty, fall back to current directory
  if (len_trim(cwd) == 0) then
     cwd = "."
  end if
  
  call ftninit(trim(cwd))
  
  if (.not. associated(savg)) then
  !  print *, 'ERROR:M65A savg is not associated!'
  end if
  
  if (.not. associated(dd)) then
  !  print *, 'ERROR: M65A dd is not associated!'
  end if
  
  if (.not. associated(ss)) then
  !  print *, 'ERROR: M65A ss is not associated!'
  end if
      
  call m65c()
  return

end subroutine m65a

end module m65a_mod
