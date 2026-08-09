#pragma once

#include <memory>
#include <string>

namespace mepwm {

// X11/Wayland act as the native window manager/compositor on Linux; Macos
// and Windows are overlay backends that arrange the host system's windows
// (see docs/PORTING.md).
enum class BackendKind { X11, Wayland, Macos, Windows };

struct Config {
  unsigned int gap = 8;
#if defined(__APPLE__)
  // `open -a Terminal <dir>` opens a NEW window even when Terminal is
  // already running; a bare `open -a Terminal` would only activate it.
  std::string terminal = "open -a kitty ~ || open -a iTerm ~ || open -a Terminal ~";
#elif defined(_WIN32)
  std::string terminal = "start cmd";
#else
  std::string terminal = "command -v kitty >/dev/null 2>&1 && exec kitty || exec xterm";
#endif
  unsigned int border_width = 4;
  std::string border_color_normal = "#444444";
  std::string border_color_focused = "#5294e2";
  // Primary hotkey modifier for the overlay backends (macOS/Windows):
  // "cmd", "alt", "ctrl", "super", "fn" (the macOS Globe/fn key), or
  // "fn+cmd" (Globe/fn held together with Command). The X11 backend keeps
  // Super.
#if defined(__APPLE__)
  std::string modifier = "fn+cmd";
#else
  std::string modifier = "super";
#endif
  unsigned int snap = 32;
  float mfact = 0.55F;
  unsigned int nmaster = 1;
  std::string agent_command = "claude";
  std::string wallpaper_dir_light = "assets/light_comic_wallpapers";
  std::string wallpaper_dir_dark = "assets/dark_comic_wallpapers";
};

class WindowManager {
 public:
  explicit WindowManager(Config config = {});
  ~WindowManager();

  WindowManager(WindowManager&&) noexcept;
  WindowManager& operator=(WindowManager&&) noexcept;
  WindowManager(const WindowManager&) = delete;
  WindowManager& operator=(const WindowManager&) = delete;

  int run(BackendKind backend);

 private:
  class Implementation;
  std::unique_ptr<Implementation> implementation_;
};

BackendKind parse_backend(const std::string& name);
std::string backend_name(BackendKind backend);

}  // namespace mepwm
