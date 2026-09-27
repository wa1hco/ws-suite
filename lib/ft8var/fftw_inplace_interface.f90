module fftw_inplace_interface
  use iso_c_binding
  implicit none

  interface
     function fftwf_plan_dft_1d_inplace(n, a, sign, flags) bind(C, name="fftwf_plan_dft_1d")
        import :: c_int, c_ptr, c_float_complex
        integer(c_int), value :: n
        complex(c_float_complex) :: a(*)
        integer(c_int), value :: sign
        integer(c_int), value :: flags
        type(c_ptr) :: fftwf_plan_dft_1d_inplace
     end function
  end interface

end module fftw_inplace_interface
