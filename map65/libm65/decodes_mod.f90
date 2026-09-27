module decodes_mod

  use npar_ptrs_mod, only: nfft_active

  implicit none

  integer :: ndecodes = 0
  integer :: nhsym1 = 0, nhsym2 = 0
  logical, allocatable :: ldecoded(:)
  ! Same idea as ldecoded, but for the JT65 path and deliberately SEPARATE:
  ! sharing one array would let a JT65 decode suppress a Q65 decode at the
  ! same bin (and vice versa) in the second pass of a period.
  logical, allocatable :: ljt65decoded(:)
  integer :: mcall3a = 0

contains

  subroutine decodes_init()
    ! (Re)allocate ldecoded to match the active symspec FFT length.
    if (allocated(ldecoded)) then
       if (size(ldecoded) /= nfft_active) then
          deallocate(ldecoded)
       end if
    end if

    if (.not. allocated(ldecoded)) then
       allocate(ldecoded(nfft_active))
       ldecoded = .false.
    end if

    if (allocated(ljt65decoded)) then
       if (size(ljt65decoded) /= nfft_active) then
          deallocate(ljt65decoded)
       end if
    end if

    if (.not. allocated(ljt65decoded)) then
       allocate(ljt65decoded(nfft_active))
       ljt65decoded = .false.
    end if
  end subroutine decodes_init

end module decodes_mod
