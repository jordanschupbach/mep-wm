#include "mepwm/window_manager.hpp"

#include <stdexcept>
#include <utility>

#include "backend.hpp"

namespace mepwm {

class WindowManager::Implementation {
 public:
  explicit Implementation(Config initial_config) : config(std::move(initial_config)) {}
  Config config;
};

WindowManager::WindowManager(Config config)
    : implementation_(std::make_unique<Implementation>(std::move(config))) {}
WindowManager::~WindowManager() = default;
WindowManager::WindowManager(WindowManager&&) noexcept = default;
WindowManager& WindowManager::operator=(WindowManager&&) noexcept = default;

int WindowManager::run(BackendKind backend_kind) {
  std::unique_ptr<Backend> backend;
  switch (backend_kind) {
    case BackendKind::X11:
#ifdef MEPWM_WITH_X11
      backend = make_x11_backend();
#else
      throw std::runtime_error("the x11 backend was not built on this platform");
#endif
      break;
    case BackendKind::Wayland:
      // The Wayland factory always exists; it throws from run() when built
      // without MEPWM_ENABLE_WAYLAND.
      backend = make_wayland_backend();
      break;
    case BackendKind::Macos:
#ifdef MEPWM_WITH_MACOS
      backend = make_macos_backend();
#else
      throw std::runtime_error("the macos backend is only available on macOS");
#endif
      break;
    case BackendKind::Windows:
      throw std::runtime_error(
          "the windows backend is not implemented yet (see docs/PORTING.md, phase 4)");
  }
  return backend->run(implementation_->config);
}

BackendKind parse_backend(const std::string& name) {
  if (name == "x11") return BackendKind::X11;
  if (name == "wayland") return BackendKind::Wayland;
  if (name == "macos") return BackendKind::Macos;
  if (name == "windows") return BackendKind::Windows;
  throw std::invalid_argument("unknown backend: " + name);
}

std::string backend_name(BackendKind backend) {
  switch (backend) {
    case BackendKind::X11: return "x11";
    case BackendKind::Wayland: return "wayland";
    case BackendKind::Macos: return "macos";
    case BackendKind::Windows: return "windows";
  }
  return "x11";
}

}  // namespace mepwm
