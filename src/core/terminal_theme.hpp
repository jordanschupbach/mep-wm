#pragma once

#include <functional>
#include <string>

namespace mepwm::core {

// Terminal theming, shared by every backend's theme picker. kitty (the
// default on every platform) gets a dedicated config include plus its
// remote-control protocol. xterm and urxvt get their startup colors from
// the X resource database. Every VT100-descended emulator (xterm, urxvt,
// alacritty, foot, kitty too) honors the OSC 10/11/12 escape sequences for
// live recoloring, so already-running sessions are updated by writing those
// sequences directly into each terminal's pty. iTerm2/Terminal.app have no
// equivalent hook available from a shell command, so they're left unthemed.
//
// `spawn` is invoked once per shell command that needs to run; the caller
// supplies its own platform-appropriate fire-and-forget spawn mechanism.
void sync_terminal_theme(const std::string& fg, const std::string& bg, const std::string& accent,
                          const std::function<void(const std::string&)>& spawn);

}  // namespace mepwm::core
