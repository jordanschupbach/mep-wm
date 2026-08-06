#pragma once

#include "mepwm/window_manager.hpp"

namespace mepwm {

class Backend {
 public:
  virtual ~Backend() = default;
  virtual int run(const Config& config) = 0;
};

std::unique_ptr<Backend> make_x11_backend();
std::unique_ptr<Backend> make_wayland_backend();

}  // namespace mepwm
