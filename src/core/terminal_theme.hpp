#pragma once

#include <array>
#include <functional>
#include <string>

namespace mepwm::core {

// Terminal theming, shared by every backend's theme picker. kitty (the
// default on every platform) gets a dedicated config include plus its
// remote-control protocol. xterm and urxvt get their startup colors from
// the X resource database. Every VT100-descended emulator (xterm, urxvt,
// alacritty, foot, kitty too) honors the OSC 10/11/12 (cursor/fg/bg) and
// OSC 4 (the 16-slot ANSI palette) escape sequences for live recoloring, so
// already-running sessions are updated by writing those sequences directly
// into each terminal's pty. The ANSI palette is what tools like neofetch
// paint their color-swatch printout with, so it needs to move in lockstep
// with fg/bg/accent for the picker to look "applied" everywhere. iTerm2/
// Terminal.app have no equivalent hook available from a shell command, so
// they're left unthemed.
//
// `ansi` holds the 16 standard slots in order: black, red, green, yellow,
// blue, magenta, cyan, white, then the same eight again as "bright"
// variants (see core::theme_palettes()).
//
// `spawn` is invoked once per shell command that needs to run; the caller
// supplies its own platform-appropriate fire-and-forget spawn mechanism.
void sync_terminal_theme(const std::string& fg, const std::string& bg, const std::string& accent,
                          const std::array<std::string, 16>& ansi,
                          const std::function<void(const std::string&)>& spawn);

}  // namespace mepwm::core
