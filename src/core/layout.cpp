#include "core/layout.hpp"

#include <algorithm>

namespace mepwm::core {

namespace {

// Splits `area` vertically into `count` rows with `gap` between them.
void stack_rows(const Rect& area, std::size_t count, int gap, std::vector<Rect>& out) {
  if (count == 0) return;
  const int total_gap = gap * static_cast<int>(count - 1);
  const int each = std::max(1, (area.height - total_gap) / static_cast<int>(count));
  int y = area.y;
  for (std::size_t row = 0; row < count; ++row) {
    const bool last = row + 1 == count;
    const int height = last ? std::max(1, area.y + area.height - y) : each;
    out.push_back({area.x, y, area.width, height});
    y += each + gap;
  }
}

}  // namespace

std::vector<Rect> master_stack(const Rect& area, std::size_t count, const LayoutParams& params) {
  std::vector<Rect> frames;
  if (count == 0) return frames;
  frames.reserve(count);

  const int gap = static_cast<int>(params.gap);
  const Rect usable{area.x + gap, area.y + gap, std::max(1, area.width - 2 * gap),
                    std::max(1, area.height - 2 * gap)};

  const std::size_t masters = std::min<std::size_t>(std::max(1U, params.nmaster), count);
  const std::size_t stacked = count - masters;
  if (stacked == 0) {
    stack_rows(usable, masters, gap, frames);
    return frames;
  }

  const float mfact = std::clamp(params.mfact, 0.1F, 0.9F);
  const int master_width = std::max(1, static_cast<int>(static_cast<float>(usable.width - gap) * mfact));
  const Rect master_area{usable.x, usable.y, master_width, usable.height};
  const Rect stack_area{usable.x + master_width + gap, usable.y,
                        std::max(1, usable.width - master_width - gap), usable.height};
  stack_rows(master_area, masters, gap, frames);
  stack_rows(stack_area, stacked, gap, frames);
  return frames;
}

}  // namespace mepwm::core
