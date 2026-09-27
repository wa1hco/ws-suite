module datcom_ptrs_mod
  use iso_c_binding
  implicit none

  ! Fortran pointer targets -- the LIVE acquisition buffers, written by
  ! symspec/recvpkt as samples arrive. Only the writer side may use these.
  real(c_float), pointer, contiguous :: dd(:,:) => null()      ! [4, 5760000]
  real(c_float), pointer, contiguous :: ss(:,:,:) => null()     ! [4, 322, NFFT]
  real(c_float), pointer, contiguous :: savg(:,:) => null()     ! [4, NFFT]

  ! Decoder-side SNAPSHOT buffers. MainWindow::decode() memcpys the live
  ! buffers into these on the GUI thread (atomic w.r.t. dataSink, which
  ! also runs there) just before raising decoder_ready -- restoring the
  ! data isolation the two-process design had via its mem_m65 copy. The
  ! decode chain (run_m65/m65a/decode0/map65a and everything they call)
  ! imports THESE via rename-on-use, so a long decode can never race the
  ! live minute wrap again. The decoder may freely write into them
  ! (timf2 zapping etc.) -- they are decoder-private between snapshots.
  real(c_float), pointer, contiguous :: dd_dec(:,:) => null()
  real(c_float), pointer, contiguous :: ss_dec(:,:,:) => null()
  real(c_float), pointer, contiguous :: savg_dec(:,:) => null()

  ! Scalar values
  integer, parameter :: NFFT=32768              !Length of FFTs
  integer(c_int) :: junk1 = 0, junk2 = 0, quitid = 0
  
contains

  ! Helper subroutine to copy fixed-length Fortran string to C buffer
  subroutine move_chars(dest, dest_len, src)
    integer(c_int), intent(in) :: dest_len
    character(kind=c_char), intent(out) :: dest(dest_len)
    character(len=*), intent(in) :: src
    integer :: i, n

    n = min(len(src), dest_len)
    do i = 1, n
      dest(i) = src(i:i)
    end do
  end subroutine move_chars
  
  subroutine set_dd_ptr(ptr, dim1, dim2) bind(C, name="set_dd_ptr")
    use iso_c_binding
    implicit none
    type(c_ptr), value :: ptr
    integer(c_int), value :: dim1, dim2
    call c_f_pointer(ptr, dd, [dim1, dim2])
    print *, "Fortran: dd pointer associated with dimensions", dim1, dim2
  end subroutine

  subroutine set_ss_ptr(ptr, dim1, dim2, dim3) bind(C, name="set_ss_ptr")
    use iso_c_binding
    implicit none
    type(c_ptr), value :: ptr
    integer(c_int), value :: dim1, dim2, dim3
    call c_f_pointer(ptr, ss, [dim1, dim2, dim3])
    print *, "Fortran: ss pointer associated with dimensions", dim1, dim2, dim3
  end subroutine

  subroutine set_savg_ptr(ptr, dim1, dim2) bind(C, name="set_savg_ptr")
    use iso_c_binding
    implicit none
    type(c_ptr), value :: ptr
    integer(c_int), value :: dim1, dim2
    call c_f_pointer(ptr, savg, [dim1, dim2])
    print *, "Fortran: savg pointer associated with dimensions", dim1, dim2
  end subroutine 

  subroutine set_dd_dec_ptr(ptr, dim1, dim2) bind(C, name="set_dd_dec_ptr")
    use iso_c_binding
    implicit none
    type(c_ptr), value :: ptr
    integer(c_int), value :: dim1, dim2
    call c_f_pointer(ptr, dd_dec, [dim1, dim2])
    print *, "Fortran: dd_dec (snapshot) pointer associated", dim1, dim2
  end subroutine

  subroutine set_ss_dec_ptr(ptr, dim1, dim2, dim3) bind(C, name="set_ss_dec_ptr")
    use iso_c_binding
    implicit none
    type(c_ptr), value :: ptr
    integer(c_int), value :: dim1, dim2, dim3
    call c_f_pointer(ptr, ss_dec, [dim1, dim2, dim3])
    print *, "Fortran: ss_dec (snapshot) pointer associated", dim1, dim2, dim3
  end subroutine

  subroutine set_savg_dec_ptr(ptr, dim1, dim2) bind(C, name="set_savg_dec_ptr")
    use iso_c_binding
    implicit none
    type(c_ptr), value :: ptr
    integer(c_int), value :: dim1, dim2
    call c_f_pointer(ptr, savg_dec, [dim1, dim2])
    print *, "Fortran: savg_dec (snapshot) pointer associated", dim1, dim2
  end subroutine
  
  function get_junk1() bind(C, name="get_junk1")
    integer(c_int) :: get_junk1
    get_junk1 = junk1
  end function

  function get_junk2() bind(C, name="get_junk2")
    integer(c_int) :: get_junk2
    get_junk2 = junk2
  end function
  
subroutine set_quitid(val) bind(C, name="set_quitid")
    use iso_c_binding
    integer(c_long_long), value :: val
    quitid = val
    ! print*,'DATCOM_PTRS_MOD QUITID is:',quitid
  end subroutine

  subroutine set_junk1(val) bind(C, name="set_junk1")
    use iso_c_binding
    integer(c_int), value :: val
    junk1 = val
  end subroutine

  subroutine set_junk2(val) bind(C, name="set_junk2")
    use iso_c_binding
    integer(c_int), value :: val
    junk2 = val
  end subroutine

end module datcom_ptrs_mod
