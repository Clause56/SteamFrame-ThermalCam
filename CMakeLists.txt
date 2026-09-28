cmake_minimum_required(VERSION 3.16)
project(thermal_viewer VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
add_compile_options(-Wall -Wextra)

option(WITH_SDL2 "Desktop preview window" ON)
option(WITH_OPENVR "SteamVR overlay display" ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBUSB REQUIRED IMPORTED_TARGET libusb-1.0)
find_package(Threads REQUIRED)

# Core library: userland UVC driver + thermal decoding/rendering. Only
# depends on libusb, so it builds anywhere.
add_library(thermal_core STATIC
  src/uvc/uvc_descriptors.cpp
  src/uvc/frame_assembler.cpp
  src/uvc/uvc_camera.cpp
  src/thermal/thermal_frame.cpp
  src/thermal/renderer.cpp
  src/util/png.cpp
  src/util/log.cpp
  src/app/sources.cpp)
target_link_libraries(thermal_core PUBLIC PkgConfig::LIBUSB Threads::Threads)

add_executable(thermal-viewer
  src/app/main.cpp
  src/display/desktop_sdl.cpp
  src/display/openvr_overlay.cpp)
target_link_libraries(thermal-viewer PRIVATE thermal_core)
if(NOT THERMAL_VIEWER_VERSION)
  set(THERMAL_VIEWER_VERSION ${PROJECT_VERSION})
endif()
target_compile_definitions(thermal-viewer PRIVATE THERMAL_VIEWER_VERSION="${THERMAL_VIEWER_VERSION}")

if(WITH_SDL2)
  pkg_check_modules(SDL2 IMPORTED_TARGET sdl2)
  if(SDL2_FOUND)
    target_compile_definitions(thermal-viewer PRIVATE HAVE_SDL2)
    target_link_libraries(thermal-viewer PRIVATE PkgConfig::SDL2)
  else()
    message(STATUS "SDL2 not found: desktop display disabled")
  endif()
endif()

if(WITH_OPENVR)
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    set(OPENVR_PLATFORM linuxarm64)
  else()
    set(OPENVR_PLATFORM linux64)
  endif()
  set(OPENVR_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/openvr)
  set(OPENVR_LIB ${OPENVR_DIR}/lib/${OPENVR_PLATFORM}/libopenvr_api.so)
  if(EXISTS ${OPENVR_LIB})
    target_compile_definitions(thermal-viewer PRIVATE HAVE_OPENVR)
    target_include_directories(thermal-viewer PRIVATE ${OPENVR_DIR}/headers)
    target_link_libraries(thermal-viewer PRIVATE ${OPENVR_LIB})
    # Ship libopenvr_api.so next to the binary.
    set_target_properties(thermal-viewer PROPERTIES BUILD_WITH_INSTALL_RPATH ON INSTALL_RPATH "$ORIGIN")
    add_custom_command(TARGET thermal-viewer POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different ${OPENVR_LIB} $<TARGET_FILE_DIR:thermal-viewer>/)
  else()
    message(STATUS "OpenVR library for ${OPENVR_PLATFORM} not found: overlay display disabled")
  endif()
endif()

enable_testing()
add_executable(thermal-tests tests/tests.cpp)
target_link_libraries(thermal-tests PRIVATE thermal_core)
add_test(NAME thermal-tests COMMAND thermal-tests)
