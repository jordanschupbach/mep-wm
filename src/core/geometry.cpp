#include "core/geometry.hpp"

#include <cmath>
#include <limits>

namespace mepwm::core {

std::optional<std::size_t> pick_in_direction(const std::vector<Rect>& frames, std::size_t from,
                                             Direction direction) {
  if (from >= frames.size()) return std::nullopt;
  const Rect& origin = frames[from];
  std::optional<std::size_t> best;
  long best_score = std::numeric_limits<long>::max();
  for (std::size_t index = 0; index < frames.size(); ++index) {
    if (index == from) continue;
    const Rect& candidate = frames[index];
    const long dx = candidate.center_x() - origin.center_x();
    const long dy = candidate.center_y() - origin.center_y();
    bool eligible = false;
    long primary = 0;
    long secondary = 0;
    switch (direction) {
      case Direction::Left:
        eligible = dx < 0;
        primary = -dx;
        secondary = std::labs(dy);
        break;
      case Direction::Right:
        eligible = dx > 0;
        primary = dx;
        secondary = std::labs(dy);
        break;
      case Direction::Up:
        eligible = dy < 0;
        primary = -dy;
        secondary = std::labs(dx);
        break;
      case Direction::Down:
        eligible = dy > 0;
        primary = dy;
        secondary = std::labs(dx);
        break;
    }
    if (!eligible) continue;
    // Weight the on-axis distance, tie-break by off-axis drift so a window
    // straight ahead beats a nearer diagonal one.
    const long score = primary + 2 * secondary;
    if (score < best_score) {
      best_score = score;
      best = index;
    }
  }
  return best;
}

}  // namespace mepwm::core
