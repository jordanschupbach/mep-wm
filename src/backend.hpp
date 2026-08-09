#pragma once

#include "mepwm/window_manager.hpp"

namespace mepwm {

class Backend {
 public:
  Backend() = default;
  virtual ~Backend() = default;
  // Polymorphic base held exclusively behind unique_ptr<Backend> -- copying
  // or moving through the base would slice the concrete X11Backend/Wayland
  // backend, so those are explicitly disabled rather than left implicit.
  Backend(const Backend&) = delete;
  Backend& operator=(const Backend&) = delete;
  Backend(Backend&&) = delete;
  Backend& operator=(Backend&&) = delete;
  virtual int run(const Config& config) = 0;
};

// Each factory is defined in its backend's translation unit; only the
// backends compiled on the current platform may be referenced (the
// MEPWM_WITH_* guards in window_manager.cpp keep the linker honest).
std::unique_ptr<Backend> make_x11_backend();
std::unique_ptr<Backend> make_wayland_backend();
std::unique_ptr<Backend> make_macos_backend();

}  // namespace mepwm
