cmake_minimum_required(VERSION 3.16)

project(mepwm VERSION 0.1.0 LANGUAGES C CXX)

# Backend availability per host platform (see docs/PORTING.md): on Linux
# mepwm is the window manager (X11, experimental Wayland); on macOS it is an
# overlay manager driving the native window server through the Accessibility
# API. A Windows overlay backend is planned (MEPWM_ENABLE_WINDOWS, phase 4).
if(APPLE)
  set(MEPWM_DEFAULT_X11 OFF)
  set(MEPWM_DEFAULT_WAYLAND OFF)
  set(MEPWM_DEFAULT_MACOS ON)
elseif(WIN32)
  set(MEPWM_DEFAULT_X11 OFF)
  set(MEPWM_DEFAULT_WAYLAND OFF)
  set(MEPWM_DEFAULT_MACOS OFF)
else()
  set(MEPWM_DEFAULT_X11 ON)
  set(MEPWM_DEFAULT_WAYLAND ON)
  set(MEPWM_DEFAULT_MACOS OFF)
endif()

option(MEPWM_ENABLE_X11 "Build the X11 backend" ${MEPWM_DEFAULT_X11})
option(MEPWM_ENABLE_WAYLAND "Build the wlroots Wayland backend" ${MEPWM_DEFAULT_WAYLAND})
option(MEPWM_ENABLE_MACOS "Build the macOS overlay backend" ${MEPWM_DEFAULT_MACOS})

# clang-tidy and IDE tooling (clangd) both read this to know the actual
# include paths/defines each file was compiled with -- without it clang-tidy
# falls back to guessing flags and misses half the X11/wlroots headers.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(GNUInstallDirs)

if(MEPWM_ENABLE_X11 OR MEPWM_ENABLE_WAYLAND)
  find_package(PkgConfig REQUIRED)
endif()

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
# CXX,OBJCXX: the Objective-C++ macOS backend takes the same C++ discipline.
set(MEPWM_WARNING_FLAGS
  -Werror
  ${MEPWM_WARNING_FLAGS_COMMON}
  $<$<COMPILE_LANGUAGE:CXX,OBJCXX>:${MEPWM_WARNING_FLAGS_CXX_ONLY}>
)

# Platform-agnostic sources: the public WindowManager facade, the core
# policy layer (layout math, Platform bridge, TilingEngine), and the Wayland
# factory stub (which throws at run() unless the real backend is compiled).
set(MEPWM_SOURCES
  src/window_manager.cpp
  src/core/geometry.cpp
  src/core/layout.cpp
  src/core/tree.cpp
  src/core/engine.cpp
  src/core/terminal_theme.cpp
  src/core/theme_palette.cpp
  src/wayland/wayland_backend.cpp
)

add_library(mepwm ${MEPWM_SOURCES})
add_library(mepwm::mepwm ALIAS mepwm)
target_include_directories(mepwm PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(mepwm PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(mepwm PUBLIC cxx_std_17)
target_compile_options(mepwm PRIVATE ${MEPWM_WARNING_FLAGS})

if(MEPWM_ENABLE_X11)
  pkg_check_modules(X11 REQUIRED IMPORTED_TARGET x11)
  pkg_check_modules(XINERAMA REQUIRED IMPORTED_TARGET xinerama)
  pkg_check_modules(XFT REQUIRED IMPORTED_TARGET xft)
  pkg_check_modules(FONTCONFIG REQUIRED IMPORTED_TARGET fontconfig)
  pkg_check_modules(IMLIB2 REQUIRED IMPORTED_TARGET imlib2)
  pkg_check_modules(LUA REQUIRED IMPORTED_TARGET lua)
  pkg_check_modules(DBUS REQUIRED IMPORTED_TARGET dbus-1)
  target_sources(mepwm PRIVATE src/x11/x11_backend.cpp)
  target_compile_definitions(mepwm PRIVATE MEPWM_WITH_X11=1)
  target_link_libraries(mepwm PRIVATE PkgConfig::X11 PkgConfig::XINERAMA PkgConfig::XFT PkgConfig::FONTCONFIG PkgConfig::LUA PkgConfig::DBUS PkgConfig::IMLIB2)
endif()

if(MEPWM_ENABLE_WAYLAND)
  pkg_check_modules(WLROOTS REQUIRED IMPORTED_TARGET wlroots-0.20)
  pkg_check_modules(WAYLAND_SERVER REQUIRED IMPORTED_TARGET wayland-server)
  target_sources(mepwm PRIVATE src/wayland/wayland_backend.c)
  target_compile_definitions(mepwm PRIVATE MEPWM_WITH_WAYLAND=1 WLR_USE_UNSTABLE=1)
  target_link_libraries(mepwm PRIVATE PkgConfig::WLROOTS PkgConfig::WAYLAND_SERVER)
endif()

if(MEPWM_ENABLE_MACOS)
  if(NOT APPLE)
    message(FATAL_ERROR "MEPWM_ENABLE_MACOS requires building on macOS")
  endif()
  enable_language(OBJCXX)
  set_target_properties(mepwm PROPERTIES OBJCXX_STANDARD 17 OBJCXX_STANDARD_REQUIRED ON)
  target_sources(mepwm PRIVATE src/macos/macos_platform.mm)
  set_source_files_properties(src/macos/macos_platform.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
  target_compile_definitions(mepwm PRIVATE MEPWM_WITH_MACOS=1)
  target_link_libraries(mepwm PRIVATE
    "-framework AppKit"
    "-framework Carbon"
    "-framework ApplicationServices"
    "-framework CoreFoundation"
    "-framework QuartzCore"
    "-framework CoreWLAN"
    "-framework IOKit"
    "-framework IOBluetooth"
  )
endif()

add_executable(mepwm-bin src/main.cpp)
set_target_properties(mepwm-bin PROPERTIES OUTPUT_NAME mepwm)
target_compile_options(mepwm-bin PRIVATE ${MEPWM_WARNING_FLAGS})
target_link_libraries(mepwm-bin PRIVATE mepwm::mepwm)

if(MEPWM_ENABLE_MACOS)
  # Spotlight/Launchpad entry: the same program wrapped in a MEP-wm.app
  # bundle -- Spotlight only indexes app bundles in Applications folders,
  # not bare binaries. `just install-app` copies it to ~/Applications.
  add_executable(mepwm-app MACOSX_BUNDLE src/main.cpp)
  set_target_properties(mepwm-app PROPERTIES
    OUTPUT_NAME "MEP-wm"
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/src/macos/Info.plist.in
    MACOSX_BUNDLE_BUNDLE_NAME "MEP-wm"
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.mepwm"
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
  )
  target_compile_options(mepwm-app PRIVATE ${MEPWM_WARNING_FLAGS})
  target_link_libraries(mepwm-app PRIVATE mepwm::mepwm)
endif()

# mep-wm-cli only writes Lua source over the IPC socket; it does not embed
# Lua itself, so it stays dependency-free and builds on every POSIX platform.
add_executable(mep-wm-cli src/mep_wm_cli.cpp)
target_compile_options(mep-wm-cli PRIVATE ${MEPWM_WARNING_FLAGS})

install(TARGETS mepwm-bin mep-wm-cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY assets/ DESTINATION ${CMAKE_INSTALL_DATADIR}/mepwm/assets)
