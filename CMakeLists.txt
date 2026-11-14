cmake_minimum_required(VERSION 3.16)
project(ember VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
include(GNUInstallDirs)

enable_testing()

set(LV2_DIR "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}/lv2"
    CACHE PATH "Directory where LV2 bundles are installed")
set(CLAP_DIR "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}/clap"
    CACHE PATH "Directory where CLAP bundles are installed")

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(LV2 REQUIRED IMPORTED_TARGET lv2)
pkg_check_modules(LILV QUIET IMPORTED_TARGET lilv-0)
pkg_check_modules(CAIRO QUIET IMPORTED_TARGET cairo)
pkg_check_modules(FREETYPE QUIET IMPORTED_TARGET freetype2)
pkg_check_modules(X11 QUIET IMPORTED_TARGET x11)

add_library(ember MODULE
  src/ember.cpp
)
target_link_libraries(ember PRIVATE PkgConfig::LV2)
set_target_properties(ember PROPERTIES
  PREFIX ""
  OUTPUT_NAME "ember"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lv2/ember.lv2")
set(EMBER_BUNDLE_FILES
  "${CMAKE_SOURCE_DIR}/lv2/manifest.ttl"
  "${CMAKE_SOURCE_DIR}/lv2/ember.ttl"
  "${CMAKE_SOURCE_DIR}/lv2/presets.ttl"
  "${CMAKE_SOURCE_DIR}/assets/knob.png"
  "${CMAKE_SOURCE_DIR}/assets/metal.png"
  "${CMAKE_SOURCE_DIR}/assets/fonts/Jost-ExtraBold.ttf"
  "${CMAKE_SOURCE_DIR}/assets/fonts/OFL.txt")

add_custom_target(ember_bundle ALL
  COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/lv2/ember.lv2"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${EMBER_BUNDLE_FILES}
          "${CMAKE_BINARY_DIR}/lv2/ember.lv2/"
  DEPENDS ${EMBER_BUNDLE_FILES})
add_dependencies(ember_bundle ember)
target_compile_features(ember PRIVATE cxx_std_17)
target_include_directories(ember PRIVATE
  "${CMAKE_SOURCE_DIR}/src"
  "${CMAKE_SOURCE_DIR}/src/dsp"
  "${CMAKE_SOURCE_DIR}/src/ember"
  ${PKG_CONFIG_LV2_INCLUDE_DIRS})
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(ember PRIVATE -Wall -Wextra -Wpedantic -fvisibility=hidden)
endif()

set(EMBER_UI_TARGET "")
if(CAIRO_FOUND AND X11_FOUND AND FREETYPE_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/src/ui/ember_ui.cpp")
  add_library(ember_ui MODULE
    src/ui/ember_ui.cpp
  )
  set(EMBER_UI_TARGET ember_ui)
  target_include_directories(ember_ui PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/src/dsp"
    "${CMAKE_SOURCE_DIR}/src/ember")
  target_link_libraries(ember_ui PRIVATE
    PkgConfig::CAIRO PkgConfig::X11 PkgConfig::LV2 PkgConfig::FREETYPE)
  set_target_properties(ember_ui PROPERTIES
    PREFIX ""
    OUTPUT_NAME "emberUI"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lv2/ember.lv2")
  target_compile_features(ember_ui PRIVATE cxx_std_17)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(ember_ui PRIVATE -Wall -Wextra -fvisibility=hidden)
  endif()

  add_executable(ember_ui_preview EXCLUDE_FROM_ALL
    src/ui/ember_ui.cpp
  )
  target_compile_definitions(ember_ui_preview PRIVATE EMBER_UI_PREVIEW)
  target_include_directories(ember_ui_preview PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/src/dsp"
    "${CMAKE_SOURCE_DIR}/src/ember")
  target_link_libraries(ember_ui_preview PRIVATE
    PkgConfig::CAIRO PkgConfig::X11 PkgConfig::LV2 PkgConfig::FREETYPE)

  find_library(XTST_LIBRARY Xtst)
  find_program(XVFB_RUN xvfb-run)
  if(XTST_LIBRARY)
    add_executable(ember_ui_host_test
      test/ui_host_test.cpp
    )
    target_include_directories(ember_ui_host_test PRIVATE
      "${CMAKE_SOURCE_DIR}/src/ember"
      ${PKG_CONFIG_LV2_INCLUDE_DIRS})
    target_link_libraries(ember_ui_host_test PRIVATE
      PkgConfig::CAIRO PkgConfig::X11 ${CMAKE_DL_LIBS} ${XTST_LIBRARY})
    if(XVFB_RUN)
      add_test(NAME ember_ui_host
        COMMAND "${XVFB_RUN}" -a -s "-screen 0 1400x1100x24"
          "$<TARGET_FILE:ember_ui_host_test>"
          "${CMAKE_BINARY_DIR}/lv2/ember.lv2/emberUI.so"
          "${CMAKE_BINARY_DIR}/ember-ui-live.png")
      set_tests_properties(ember_ui_host PROPERTIES
        ENVIRONMENT "LIBGL_ALWAYS_SOFTWARE=1")
    endif()
  endif()
endif()

install(TARGETS ember ${EMBER_UI_TARGET}
  LIBRARY DESTINATION "${LV2_DIR}/ember.lv2")
install(FILES
  lv2/manifest.ttl
  lv2/ember.ttl
  lv2/presets.ttl
  assets/knob.png
  assets/metal.png
  assets/fonts/Jost-ExtraBold.ttf
  assets/fonts/OFL.txt
  DESTINATION "${LV2_DIR}/ember.lv2")

# ---------------------------------------------------------------------------
# CLAP build: same DSP and X11 UI, plus clap.gain-adjustment-metering so
# REAPER and other CLAP hosts can show gain reduction on their track meters.
# ---------------------------------------------------------------------------
add_library(ember_clap MODULE
  src/clap/ember_clap.cpp
)
target_link_libraries(ember_clap PRIVATE PkgConfig::LV2 ${CMAKE_DL_LIBS}
  Threads::Threads)
target_compile_features(ember_clap PRIVATE cxx_std_17)
target_include_directories(ember_clap PRIVATE
  "${CMAKE_SOURCE_DIR}/src"
  "${CMAKE_SOURCE_DIR}/src/dsp"
  "${CMAKE_SOURCE_DIR}/src/ember"
  "${CMAKE_SOURCE_DIR}/third_party/clap/include"
  ${PKG_CONFIG_LV2_INCLUDE_DIRS})
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/clap")
set_target_properties(ember_clap PROPERTIES
  PREFIX ""
  OUTPUT_NAME "Ember"
  SUFFIX ".clap"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/clap")
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(ember_clap PRIVATE -Wall -Wextra -fvisibility=hidden)
endif()

set(EMBER_CLAP_ASSETS
  "${CMAKE_SOURCE_DIR}/assets/knob.png"
  "${CMAKE_SOURCE_DIR}/assets/metal.png"
  "${CMAKE_SOURCE_DIR}/assets/fonts/Jost-ExtraBold.ttf"
  "${CMAKE_SOURCE_DIR}/assets/fonts/OFL.txt")

if(CAIRO_FOUND AND X11_FOUND AND FREETYPE_FOUND)
  add_custom_target(ember_clap_bundle ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory
            "${CMAKE_BINARY_DIR}/clap/Ember.resources"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${EMBER_CLAP_ASSETS}
            "${CMAKE_BINARY_DIR}/clap/Ember.resources/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:ember_ui>"
            "${CMAKE_BINARY_DIR}/clap/Ember.resources/emberUI.so"
    DEPENDS ember_clap ember_ui ${EMBER_CLAP_ASSETS})
endif()

if(TARGET ember_clap_bundle)
  install(TARGETS ember_clap LIBRARY DESTINATION "${CLAP_DIR}")
  install(DIRECTORY "${CMAKE_BINARY_DIR}/clap/Ember.resources"
          DESTINATION "${CLAP_DIR}")
endif()

add_executable(ember_dsp_test
  test/test_dsp.cpp
)
target_include_directories(ember_dsp_test PRIVATE
  "${CMAKE_SOURCE_DIR}/src"
  "${CMAKE_SOURCE_DIR}/src/dsp"
  "${CMAKE_SOURCE_DIR}/src/ember")
target_compile_features(ember_dsp_test PRIVATE cxx_std_17)
add_test(NAME ember_dsp COMMAND ember_dsp_test)

if(LILV_FOUND)
  add_executable(ember_host_test
    test/host_test.cpp
  )
  target_include_directories(ember_host_test PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/src/dsp"
    "${CMAKE_SOURCE_DIR}/src/ember")
  target_link_libraries(ember_host_test PRIVATE PkgConfig::LILV)
  add_test(NAME ember_host COMMAND ember_host_test "${CMAKE_BINARY_DIR}/lv2")
endif()
