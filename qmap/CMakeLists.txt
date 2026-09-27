set (qmap_CXXSRCS
  about.cpp
  astro.cpp
  bandmap.cpp
  devsetup.cpp
  displaytext.cpp
  getfile.cpp
  main.cpp
  mainwindow.cpp
  meterwidget.cpp
  plotter.cpp
  runtime_paths.cpp
  signalmeter.cpp
  soundin.cpp
  widegraph.cpp
  ../Network/PSKReporter.cpp
  ../Network/PSKReporterIPFIX.cpp
  )

if (WIN32)
  set (qmap_CXXSRCS ${qmap_CXXSRCS})
endif (WIN32)

set (qmap_UISRCS
  about.ui
  astro.ui
  devsetup.ui
  mainwindow.ui
  widegraph.ui
)

set (qmap_C_and_CXXSRCS
  ${qmap_CSRCS}
  ${qmap_CXXSRCS}
  )
set_property (SOURCE ${qmap_C_and_CXXSRCS} APPEND_STRING PROPERTY COMPILE_FLAGS " -include wsjtx_config.h")
set_property (SOURCE ${qmap_C_and_CXXSRCS} APPEND PROPERTY OBJECT_DEPENDS ${CMAKE_BINARY_DIR}/wsjtx_config.h)

# build the subdirectories
add_subdirectory (libqmap)

# UI generation
qt5_wrap_ui (qmap_GENUISRCS ${qmap_UISRCS})

# Resource generation (the program icon)
qt5_add_resources (qmap_RESOURCES_RCC wsmap.qrc)

# The resource compiler is not dependency-scanned: without this a changed
# icon never reaches wsmap.exe in an existing build tree.
set_source_files_properties (qmap.rc PROPERTIES OBJECT_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/wsjt.ico)

add_executable (qmap ${qmap_CXXSRCS} ${qmap_CSRCS} ${qmap_GENUISRCS} ${qmap_RESOURCES_RCC} qmap.rc)
# Binary is wsmap.exe; the CMake target keeps its old name so the many
# target_link_libraries/-_include_directories references need no churn.
set_target_properties (qmap PROPERTIES OUTPUT_NAME wsmap)
target_include_directories (qmap PRIVATE ${CMAKE_SOURCE_DIR} ${FFTW3_INCLUDE_DIRS})
target_link_libraries (qmap wsjt_qt qmap_impl ${FFTW3_LIBRARIES} Qt5::Widgets Qt5::Network Usb::Usb)

if (WSJT_CREATE_WINMAIN)
  set_target_properties (qmap PROPERTIES WIN32_EXECUTABLE ON)
endif (WSJT_CREATE_WINMAIN)

install (
  TARGETS qmap
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT runtime
  BUNDLE DESTINATION . COMPONENT runtime
  )

