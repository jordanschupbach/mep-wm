#pragma once

#include <memory>
#include <string>

namespace mepwm {

enum class BackendKind { X11, Wayland };

struct Config {
  unsigned int gap = 8;
  std::string terminal = "command -v kitty >/dev/null 2>&1 && exec kitty || exec xterm";
  unsigned int border_width = 2;
  std::string border_color_normal = "#444444";
  std::string border_color_focused = "#5294e2";
  unsigned int snap = 32;
  float mfact = 0.55f;
  unsigned int nmaster = 1;
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
