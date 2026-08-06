#include "backend.hpp"

#include <memory>
#include <stdexcept>

namespace mepwm {
#ifdef MEPWM_WITH_WAYLAND
extern "C" int mepwm_wayland_run(void);
#endif

namespace {

class WaylandBackend final : public Backend {
 public:
  int run(const Config&) override {
#ifdef MEPWM_WITH_WAYLAND
    return mepwm_wayland_run();
#else
    throw std::runtime_error("Wayland support was not built; configure with -DMEPWM_ENABLE_WAYLAND=ON");
#endif
  }
};

}  // namespace

std::unique_ptr<Backend> make_wayland_backend() { return std::make_unique<WaylandBackend>(); }
}  // namespace mepwm
