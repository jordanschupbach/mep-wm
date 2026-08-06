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
  std::unique_ptr<Backend> backend =
      backend_kind == BackendKind::X11 ? make_x11_backend() : make_wayland_backend();
  return backend->run(implementation_->config);
}

BackendKind parse_backend(const std::string& name) {
  if (name == "x11") return BackendKind::X11;
  if (name == "wayland") return BackendKind::Wayland;
  throw std::invalid_argument("unknown backend: " + name);
}

std::string backend_name(BackendKind backend) {
  return backend == BackendKind::X11 ? "x11" : "wayland";
}

}  // namespace mepwm
