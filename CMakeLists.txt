cmake_minimum_required(VERSION 3.16)

project(mepwm VERSION 0.1.0 LANGUAGES C CXX)

# Backend availability per host platform (see docs/PORTING.md): on Linux
# mepwm is the window manager (X11, experimental Wayland); on macOS it is an
# overlay manager driving the native window server through the Accessibility
# API; on Windows it is the same kind of overlay over Win32 (src/windows).
set(MEPWM_DEFAULT_WINDOWS OFF)
if(APPLE)
  set(MEPWM_DEFAULT_X11 OFF)
  set(MEPWM_DEFAULT_WAYLAND OFF)
  set(MEPWM_DEFAULT_MACOS ON)
elseif(WIN32)
  set(MEPWM_DEFAULT_X11 OFF)
  set(MEPWM_DEFAULT_WAYLAND OFF)
  set(MEPWM_DEFAULT_MACOS OFF)
  set(MEPWM_DEFAULT_WINDOWS ON)
else()
  set(MEPWM_DEFAULT_X11 ON)
  set(MEPWM_DEFAULT_WAYLAND ON)
  set(MEPWM_DEFAULT_MACOS OFF)
endif()

option(MEPWM_ENABLE_X11 "Build the X11 backend" ${MEPWM_DEFAULT_X11})
option(MEPWM_ENABLE_WAYLAND "Build the wlroots Wayland backend" ${MEPWM_DEFAULT_WAYLAND})
option(MEPWM_ENABLE_MACOS "Build the macOS overlay backend" ${MEPWM_DEFAULT_MACOS})
option(MEPWM_ENABLE_WINDOWS "Build the Windows overlay backend" ${MEPWM_DEFAULT_WINDOWS})

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
if(MSVC)
  # MSVC's analogue of the curated GCC/Clang set above: /W4 as errors.
  # /utf-8 because the sources carry UTF-8 literals (Nerd Font icons, the
  # help text). C4996 is MSVC steering getenv/sscanf/strncpy toward its _s
  # variants, which are not portable; the code already bounds-checks.
  set(MEPWM_WARNING_FLAGS /W4 /WX /utf-8 /wd4996)
else()
  set(MEPWM_WARNING_FLAGS
    -Werror
    ${MEPWM_WARNING_FLAGS_COMMON}
    $<$<COMPILE_LANGUAGE:CXX,OBJCXX>:${MEPWM_WARNING_FLAGS_CXX_ONLY}>
  )
endif()

# Platform-agnostic sources: the public WindowManager facade, the core
# policy layer (layout math, Platform bridge, TilingEngine), and the Wayland
# factory stub (which throws at run() unless the real backend is compiled).
set(MEPWM_SOURCES
  src/window_manager.cpp
  src/core/geometry.cpp
  src/core/layout.cpp
  src/core/tree.cpp
  src/core/engine.cpp
  src/core/mep_theme.cpp
  src/core/terminal_theme.cpp
  src/core/theme_palette.cpp
  src/core/org_todo.cpp
  src/wayland/wayland_backend.cpp
)

find_package(Threads REQUIRED)

add_library(mepwm ${MEPWM_SOURCES})
add_library(mepwm::mepwm ALIAS mepwm)
target_include_directories(mepwm PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(mepwm PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(mepwm PUBLIC cxx_std_17)
target_compile_options(mepwm PRIVATE ${MEPWM_WARNING_FLAGS})
# mep_theme.cpp fans a theme-sync write out to every running mep instance on
# its own detached std::thread, so the picker never blocks on a stale socket.
target_link_libraries(mepwm PRIVATE Threads::Threads)

if(MEPWM_ENABLE_X11)
  pkg_check_modules(X11 REQUIRED IMPORTED_TARGET x11)
  pkg_check_modules(XINERAMA REQUIRED IMPORTED_TARGET xinerama)
  pkg_check_modules(XRANDR REQUIRED IMPORTED_TARGET xrandr)
  pkg_check_modules(XFT REQUIRED IMPORTED_TARGET xft)
  pkg_check_modules(FONTCONFIG REQUIRED IMPORTED_TARGET fontconfig)
  pkg_check_modules(IMLIB2 REQUIRED IMPORTED_TARGET imlib2)
  pkg_check_modules(LUA REQUIRED IMPORTED_TARGET lua)
  pkg_check_modules(DBUS REQUIRED IMPORTED_TARGET dbus-1)
  target_sources(mepwm PRIVATE src/x11/x11_backend.cpp)
  target_compile_definitions(mepwm PRIVATE MEPWM_WITH_X11=1)
  target_link_libraries(mepwm PRIVATE PkgConfig::X11 PkgConfig::XINERAMA PkgConfig::XRANDR PkgConfig::XFT PkgConfig::FONTCONFIG PkgConfig::LUA PkgConfig::DBUS PkgConfig::IMLIB2)
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

if(MEPWM_ENABLE_WINDOWS)
  if(NOT WIN32)
    message(FATAL_ERROR "MEPWM_ENABLE_WINDOWS requires building on Windows")
  endif()
  target_sources(mepwm PRIVATE src/windows/windows_platform.cpp)
  # NOMINMAX/WIN32_LEAN_AND_MEAN keep <windows.h> from clobbering std::min
  # and dragging in winsock 1; UNICODE selects the W APIs throughout.
  target_compile_definitions(mepwm PRIVATE MEPWM_WITH_WINDOWS=1 NOMINMAX WIN32_LEAN_AND_MEAN
                                           UNICODE _UNICODE)
  target_link_libraries(mepwm PRIVATE user32 gdi32 shell32 ole32 dwmapi wlanapi bthprops
                                      advapi32 shlwapi)
endif()

add_executable(mepwm-bin src/main.cpp)
if(MEPWM_ENABLE_WINDOWS)
  # UTF-8 active code page (see the manifest); MSVC embeds .manifest sources.
  target_sources(mepwm-bin PRIVATE src/windows/mepwm.manifest)
endif()
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

if(MEPWM_ENABLE_WINDOWS)
  # Start-menu/startup entry: the same program built for the GUI subsystem,
  # so launching it outside a terminal doesn't open a console window (the
  # Windows analogue of MEP-wm.app). mepwm.exe stays a console program for
  # terminal use, with its log output and Ctrl+C.
  add_executable(mepwm-gui WIN32 src/main.cpp src/windows/mepwm.manifest)
  set_target_properties(mepwm-gui PROPERTIES OUTPUT_NAME "MEP-wm")
  target_link_options(mepwm-gui PRIVATE /ENTRY:mainCRTStartup)
  target_compile_options(mepwm-gui PRIVATE ${MEPWM_WARNING_FLAGS})
  target_compile_definitions(mepwm-gui PRIVATE MEPWM_GUI_SUBSYSTEM=1)
  target_link_libraries(mepwm-gui PRIVATE mepwm::mepwm)
  install(TARGETS mepwm-gui RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

# mep-wm-cli only writes Lua source over the X11 backend's unix-socket IPC;
# it does not embed Lua itself, so it stays dependency-free and builds on
# every POSIX platform. The overlay backends have no IPC yet, so Windows
# skips it.
if(NOT WIN32)
  add_executable(mep-wm-cli src/mep_wm_cli.cpp)
  target_compile_options(mep-wm-cli PRIVATE ${MEPWM_WARNING_FLAGS})
  install(TARGETS mep-wm-cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

install(TARGETS mepwm-bin RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY assets/ DESTINATION ${CMAKE_INSTALL_DATADIR}/mepwm/assets)
