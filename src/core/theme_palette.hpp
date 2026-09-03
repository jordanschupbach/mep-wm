#pragma once

#include <array>
#include <string>
#include <vector>

namespace mepwm::core {

// A named color scheme shared by every backend's theme picker (X11, macOS).
// `fg`/`bg`/`accent` drive UI chrome -- bars, borders, the picker's own
// highlight -- while `ansi` is the classic 16-slot terminal palette (black,
// red, green, yellow, blue, magenta, cyan, white, then the same eight again
// as "bright" variants) that terminal emulators use for everything else,
// including neofetch's color-swatch printout. Values are taken from each
// scheme's own published terminal palette where one exists (Nord, Dracula,
// Gruvbox, Tokyo Night, Catppuccin, One Dark/Light, Everforest, Solarized,
// Rose Pine); the two generic schemes ("dark"/"light") and "nord-light" (an
// unofficial light complement to Nord) are hand-tuned to match their
// fg/bg/accent.
struct ThemePalette {
  const char* name;
  const char* fg;
  const char* bg;
  const char* accent;
  std::array<const char*, 16> ansi;
};

// Built-ins, half dark half light so the picker always offers a real choice
// on either side of a light/dark wallpaper split.
const std::vector<ThemePalette>& theme_palettes();

// Derives a plausible 16-color ANSI palette from just fg/bg/accent, for
// themes that don't define one explicitly -- namely X11's mwm.theme() Lua
// API, which only ever supplied {fg, bg, selected}.
std::array<std::string, 16> synthesize_ansi_palette(const std::string& fg, const std::string& bg,
                                                     const std::string& accent);

}  // namespace mepwm::core
