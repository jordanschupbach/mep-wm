#pragma once

#include <cstdint>
#include <optional>
#include <vector>

namespace mepwm::core {

struct Rect {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;

  int center_x() const { return x + width / 2; }
  int center_y() const { return y + height / 2; }
};

enum class Direction { Left, Down, Up, Right };

// Picks the best neighbor of `from` in `direction` among `frames`, the way
// Super+h/j/k/l focus works: the nearest window whose center lies strictly
// in that direction. Returns std::nullopt when nothing lies that way.
std::optional<std::size_t> pick_in_direction(const std::vector<Rect>& frames, std::size_t from,
                                             Direction direction);

}  // namespace mepwm::core
