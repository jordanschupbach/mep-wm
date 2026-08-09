#pragma once

#include "core/geometry.hpp"

namespace mepwm::core {

struct LayoutParams {
  unsigned int gap = 8;
  float mfact = 0.55F;
  unsigned int nmaster = 1;
};

// Master/stack tiling: the first min(nmaster, count) windows share a master
// column of width mfact, the rest stack in the remaining column. Pure math,
// shared by every backend. Returns one frame per window, in order.
std::vector<Rect> master_stack(const Rect& area, std::size_t count, const LayoutParams& params);

}  // namespace mepwm::core
