cmake_minimum_required(VERSION 3.16)

project(mepwm VERSION 0.1.0 LANGUAGES C CXX)

option(MEPWM_ENABLE_WAYLAND "Build the wlroots Wayland backend" ON)

# clang-tidy and IDE tooling (clangd) both read this to know the actual
# include paths/defines each file was compiled with -- without it clang-tidy
# falls back to guessing flags and misses half the X11/wlroots headers.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(X11 REQUIRED IMPORTED_TARGET x11)
pkg_check_modules(XINERAMA REQUIRED IMPORTED_TARGET xinerama)
pkg_check_modules(XFT REQUIRED IMPORTED_TARGET xft)
pkg_check_modules(FONTCONFIG REQUIRED IMPORTED_TARGET fontconfig)
pkg_check_modules(IMLIB2 REQUIRED IMPORTED_TARGET imlib2)
pkg_check_modules(LUA REQUIRED IMPORTED_TARGET lua)
pkg_check_modules(DBUS REQUIRED IMPORTED_TARGET dbus-1)

# Warnings-as-errors for mep-wm's own sources only -- applied per-target
# below via target_compile_options(PRIVATE ...), so they never apply to the
# X11/wlroots/Lua/D-Bus headers we merely include, and per-language via
# COMPILE_LANGUAGE generator expressions, so C++-only flags (-Wshadow and
# friends) don't get handed to the one plain-C source (wayland_backend.c)
# under -Werror, where GCC would reject them outright.
#
# Curated the same way as .clang-tidy's checks: -Wall/-Wextra/-Wpedantic plus
# everything below that a clean run confirmed adds real signal for this
# codebase, rather than also enabling
# -Wconversion/-Wsign-conversion/-Wold-style-cast/-Wuseless-cast -- those
# fire ~130 times, almost all on Window/Atom (Xlib's unsigned long) meeting
# int/long/unsigned int at X11 API boundaries, the same inherent
# protocol-shape friction .clang-tidy's bugprone-narrowing-conversions was
# disabled for. Fighting that would mean sprinkling casts through every
# XEvent field and container-size comparison for no bug caught.
set(MEPWM_WARNING_FLAGS_COMMON
  -Wall -Wextra -Wpedantic
  -Wnull-dereference -Wdouble-promotion -Wformat=2 -Wimplicit-fallthrough -Wuninitialized
)
set(MEPWM_WARNING_FLAGS_CXX_ONLY
  -Wshadow -Wnon-virtual-dtor -Wcast-align -Woverloaded-virtual -Wmissing-declarations
)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_C_COMPILER_ID STREQUAL "GNU")
  list(APPEND MEPWM_WARNING_FLAGS_COMMON -Wmisleading-indentation -Wduplicated-cond -Wduplicated-branches -Wlogical-op)
endif()
set(MEPWM_WARNING_FLAGS
  -Werror
  ${MEPWM_WARNING_FLAGS_COMMON}
  $<$<COMPILE_LANGUAGE:CXX>:${MEPWM_WARNING_FLAGS_CXX_ONLY}>
)

set(MEPWM_SOURCES
  src/window_manager.cpp
  src/x11_backend.cpp
  src/wayland_backend.cpp
)

add_library(mepwm ${MEPWM_SOURCES})
add_library(mepwm::mepwm ALIAS mepwm)
target_include_directories(mepwm PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(mepwm PUBLIC cxx_std_17)
target_compile_options(mepwm PRIVATE ${MEPWM_WARNING_FLAGS})
target_link_libraries(mepwm PRIVATE PkgConfig::X11 PkgConfig::XINERAMA PkgConfig::XFT PkgConfig::FONTCONFIG PkgConfig::LUA PkgConfig::DBUS PkgConfig::IMLIB2)

if(MEPWM_ENABLE_WAYLAND)
  pkg_check_modules(WLROOTS REQUIRED IMPORTED_TARGET wlroots-0.20)
  pkg_check_modules(WAYLAND_SERVER REQUIRED IMPORTED_TARGET wayland-server)
  target_sources(mepwm PRIVATE src/wayland_backend.c)
  target_compile_definitions(mepwm PRIVATE MEPWM_WITH_WAYLAND=1 WLR_USE_UNSTABLE=1)
  target_link_libraries(mepwm PRIVATE PkgConfig::WLROOTS PkgConfig::WAYLAND_SERVER)
endif()

add_executable(mepwm-bin src/main.cpp)
set_target_properties(mepwm-bin PROPERTIES OUTPUT_NAME mepwm)
target_compile_options(mepwm-bin PRIVATE ${MEPWM_WARNING_FLAGS})
target_link_libraries(mepwm-bin PRIVATE mepwm::mepwm)

add_executable(mep-wm-cli src/mep_wm_cli.cpp)
target_compile_options(mep-wm-cli PRIVATE ${MEPWM_WARNING_FLAGS})
target_link_libraries(mep-wm-cli PRIVATE PkgConfig::LUA)
