cmake_minimum_required(VERSION 3.16)

project(mepwm VERSION 0.1.0 LANGUAGES C CXX)

option(MEPWM_ENABLE_WAYLAND "Build the wlroots Wayland backend" ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(X11 REQUIRED IMPORTED_TARGET x11)
pkg_check_modules(XINERAMA REQUIRED IMPORTED_TARGET xinerama)
pkg_check_modules(XFT REQUIRED IMPORTED_TARGET xft)
pkg_check_modules(FONTCONFIG REQUIRED IMPORTED_TARGET fontconfig)
pkg_check_modules(LUA REQUIRED IMPORTED_TARGET lua)

set(MEPWM_SOURCES
  src/window_manager.cpp
  src/x11_backend.cpp
  src/wayland_backend.cpp
)

add_library(mepwm ${MEPWM_SOURCES})
add_library(mepwm::mepwm ALIAS mepwm)
target_include_directories(mepwm PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(mepwm PUBLIC cxx_std_17)
target_link_libraries(mepwm PRIVATE PkgConfig::X11 PkgConfig::XINERAMA PkgConfig::XFT PkgConfig::FONTCONFIG PkgConfig::LUA)

if(MEPWM_ENABLE_WAYLAND)
  pkg_check_modules(WLROOTS REQUIRED IMPORTED_TARGET wlroots-0.20)
  pkg_check_modules(WAYLAND_SERVER REQUIRED IMPORTED_TARGET wayland-server)
  target_sources(mepwm PRIVATE src/wayland_backend.c)
  target_compile_definitions(mepwm PRIVATE MEPWM_WITH_WAYLAND=1 WLR_USE_UNSTABLE=1)
  target_link_libraries(mepwm PRIVATE PkgConfig::WLROOTS PkgConfig::WAYLAND_SERVER)
endif()

add_executable(mepwm-bin src/main.cpp)
set_target_properties(mepwm-bin PROPERTIES OUTPUT_NAME mepwm)
target_link_libraries(mepwm-bin PRIVATE mepwm::mepwm)

add_executable(mep-wm-cli src/mep_wm_cli.cpp)
target_link_libraries(mep-wm-cli PRIVATE PkgConfig::LUA)
