# Windows GUI KEY-agent stand-in (native .exe, no Qt/Python runtime).
# Installed as bin/inhibit-test-gui.exe next to wsjtx.exe.
# Console sibling on all platforms: inhibit-test (tools/inhibit-test/).

if (NOT WIN32)
  return ()
endif ()

add_executable (inhibit-test-gui WIN32
  inhibit_spacebar.c
  )

target_link_libraries (inhibit-test-gui PRIVATE
  ws2_32
  comctl32
  user32
  gdi32
  )

if (MINGW)
  target_compile_options (inhibit-test-gui PRIVATE -Wall -O2)
  set_target_properties (inhibit-test-gui PROPERTIES
    WIN32_EXECUTABLE TRUE
    )
endif ()

# Stable install name (was inhibit_spacebar.exe).
set_target_properties (inhibit-test-gui PROPERTIES
  OUTPUT_NAME "inhibit-test-gui"
  )

install (TARGETS inhibit-test-gui
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT runtime
  )
