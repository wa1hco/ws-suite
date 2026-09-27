! qmap_timer_init.f90 — bind(C) entry point so the QMAP main() can
! enable K1JT's existing decoder timing instrumentation. Without this,
! the `call timer('fftbig  ',0/1)` etc. calls strewn across qmapa.f90,
! q65c.f90, fftbig.f90, decode0.f90 all route to null_timer and the
! data is dropped on the floor.
!
! Once init_timer() is called with a path, every `call timer(...)`
! accumulates per-stage CPU time, and q65c.f90:105's `call timer(
! 'decode0 ',101)` (k=101 triggers dump-and-reset) writes one block
! per decoder cycle to that file. Format is K1JT's own table:
!
!   Name                 Time  Frac     dTime dFrac    Calls
!   ----------------------------------------------------------
!     decode0           0.183  1.00     0.001  0.01        1
!       qmapa           0.182  1.00     0.012  0.07        1
!         get_cand      0.024  0.13     0.024  0.13        1
!         fftbig        0.135  0.74     0.001  0.01        1
!         q65b          0.011  0.06     0.011  0.06        4
!
! That tells us where the 6 s decoder cost goes (fftbig vs candidate
! processing) so we know what to optimise if we bust the TR-60 budget.

subroutine qmap_init_timer_c(path_cstr) bind(C, name="qmap_init_timer_c")
  use, intrinsic :: iso_c_binding, only: c_char
  use timer_impl, only: init_timer
  implicit none
  character(c_char), intent(in) :: path_cstr(*)
  character(len=512) :: path
  integer :: i

  ! Decode the C string up to the NUL terminator (or 512 B max).
  path = ' '
  do i = 1, 512
    if (path_cstr(i) == achar(0)) exit
    path(i:i) = path_cstr(i)
  end do

  if (i > 1) then
    call init_timer(path(1:i-1))
  else
    call init_timer("timer.out")
  end if
end subroutine qmap_init_timer_c
