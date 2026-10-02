  ! Retired. level and onlevel live in timer_impl as threadprivate
  ! module variables. A threadprivate COMMON emits .tls_common, which
  ! the MinGW assembler rejects.
