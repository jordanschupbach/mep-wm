#include "core/theme_palette.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace mepwm::core {

namespace {

// "#rrggbb" blend of `base` toward `toward` by `amount` (0..1). Falls back
// to `base` unchanged for non-hex input.
std::string mix_hex(const std::string& base, const std::string& toward, double amount) {
  if (base.size() != 7 || toward.size() != 7 || base[0] != '#' || toward[0] != '#') return base;
  int channels[3];
  for (int channel = 0; channel < 3; ++channel) {
    const long from = std::strtol(base.substr(1 + 2 * channel, 2).c_str(), nullptr, 16);
    const long to = std::strtol(toward.substr(1 + 2 * channel, 2).c_str(), nullptr, 16);
    channels[channel] = std::clamp(
        static_cast<int>(static_cast<double>(from) + static_cast<double>(to - from) * amount), 0,
        255);
  }
  char blended[8];
  std::snprintf(blended, sizeof blended, "#%02x%02x%02x", channels[0], channels[1], channels[2]);
  return blended;
}

bool color_is_dark(const std::string& hex) {
  if (hex.size() != 7 || hex[0] != '#') return true;
  const long r = std::strtol(hex.substr(1, 2).c_str(), nullptr, 16);
  const long g = std::strtol(hex.substr(3, 2).c_str(), nullptr, 16);
  const long b = std::strtol(hex.substr(5, 2).c_str(), nullptr, 16);
  return (0.299 * static_cast<double>(r) + 0.587 * static_cast<double>(g) +
          0.114 * static_cast<double>(b)) <= 140.0;
}

}  // namespace

const std::vector<ThemePalette>& theme_palettes() {
  // clang-format off
  static const std::vector<ThemePalette> list = {
      // name                fg           bg           accent       ansi: black,    red,      green,    yellow,   blue,     magenta,  cyan,     white,    br.black, br.red,   br.green, br.yellow,br.blue,  br.magenta,br.cyan, br.white
      {"dark", "#f8f8f2", "#202124", "#5294e2",
        {"#202124", "#e06c75", "#98c379", "#e5c07b", "#5294e2", "#c678dd", "#56b6c2", "#f8f8f2",
         "#5c6370", "#e06c75", "#98c379", "#e5c07b", "#5294e2", "#c678dd", "#56b6c2", "#ffffff"}},
      {"nord", "#d8dee9", "#2e3440", "#88c0d0",
        {"#3b4252", "#bf616a", "#a3be8c", "#ebcb8b", "#81a1c1", "#b48ead", "#88c0d0", "#e5e9f0",
         "#4c566a", "#bf616a", "#a3be8c", "#ebcb8b", "#81a1c1", "#b48ead", "#8fbcbb", "#eceff4"}},
      {"dracula", "#f8f8f2", "#282a36", "#bd93f9",
        {"#21222c", "#ff5555", "#50fa7b", "#f1fa8c", "#bd93f9", "#ff79c6", "#8be9fd", "#f8f8f2",
         "#6272a4", "#ff6e6e", "#69ff94", "#ffffa5", "#d6acff", "#ff92df", "#a4ffff", "#ffffff"}},
      {"gruvbox-dark", "#ebdbb2", "#282828", "#fe8019",
        {"#282828", "#cc241d", "#98971a", "#d79921", "#458588", "#b16286", "#689d6a", "#a89984",
         "#928374", "#fb4934", "#b8bb26", "#fabd2f", "#83a598", "#d3869b", "#8ec07c", "#ebdbb2"}},
      {"tokyo-night", "#c0caf5", "#1a1b26", "#7aa2f7",
        {"#15161e", "#f7768e", "#9ece6a", "#e0af68", "#7aa2f7", "#bb9af7", "#7dcfff", "#a9b1d6",
         "#414868", "#f7768e", "#9ece6a", "#e0af68", "#7aa2f7", "#bb9af7", "#7dcfff", "#c0caf5"}},
      {"catppuccin-mocha", "#cdd6f4", "#1e1e2e", "#cba6f7",
        {"#45475a", "#f38ba8", "#a6e3a1", "#f9e2af", "#89b4fa", "#f5c2e7", "#94e2d5", "#bac2de",
         "#585b70", "#f38ba8", "#a6e3a1", "#f9e2af", "#89b4fa", "#f5c2e7", "#94e2d5", "#a6adc8"}},
      {"one-dark", "#abb2bf", "#282c34", "#61afef",
        {"#282c34", "#e06c75", "#98c379", "#e5c07b", "#61afef", "#c678dd", "#56b6c2", "#abb2bf",
         "#5c6370", "#e06c75", "#98c379", "#e5c07b", "#61afef", "#c678dd", "#56b6c2", "#ffffff"}},
      {"everforest-dark", "#d3c6aa", "#2d353b", "#a7c080",
        {"#4b565c", "#e67e80", "#a7c080", "#dbbc7f", "#7fbbb3", "#d699b6", "#83c092", "#d3c6aa",
         "#475258", "#e67e80", "#a7c080", "#dbbc7f", "#7fbbb3", "#d699b6", "#83c092", "#d3c6aa"}},
      {"light", "#202124", "#f4f4f4", "#3971ed",
        {"#383a42", "#e45649", "#50a14f", "#c18401", "#3971ed", "#a626a4", "#0184bc", "#f4f4f4",
         "#4f525e", "#e45649", "#50a14f", "#c18401", "#3971ed", "#a626a4", "#0184bc", "#ffffff"}},
      {"solarized-light", "#586e75", "#fdf6e3", "#268bd2",
        {"#073642", "#dc322f", "#859900", "#b58900", "#268bd2", "#d33682", "#2aa198", "#eee8d5",
         "#002b36", "#cb4b16", "#586e75", "#657b83", "#839496", "#6c71c4", "#93a1a1", "#fdf6e3"}},
      {"gruvbox-light", "#3c3836", "#fbf1c7", "#d65d0e",
        {"#fbf1c7", "#cc241d", "#98971a", "#d79921", "#458588", "#b16286", "#689d6a", "#7c6f64",
         "#928374", "#9d0006", "#79740e", "#b57614", "#076678", "#8f3f71", "#427b58", "#3c3836"}},
      {"catppuccin-latte", "#4c4f69", "#eff1f5", "#8839ef",
        {"#5c5f77", "#d20f39", "#40a02b", "#df8e1d", "#1e66f5", "#ea76cb", "#179299", "#acb0be",
         "#6c6f85", "#d20f39", "#40a02b", "#df8e1d", "#1e66f5", "#ea76cb", "#179299", "#bcc0cc"}},
      {"rose-pine-dawn", "#575279", "#faf4ed", "#907aa9",
        {"#f2e9e1", "#b4637a", "#286983", "#ea9d34", "#56949f", "#907aa9", "#d7827e", "#575279",
         "#9893a5", "#b4637a", "#286983", "#ea9d34", "#56949f", "#907aa9", "#d7827e", "#797593"}},
      {"everforest-light", "#5c6a72", "#f3ead3", "#8da101",
        {"#939f91", "#f85552", "#8da101", "#dfa000", "#3a94c5", "#df69ba", "#35a77c", "#5c6a72",
         "#a6b0a0", "#f85552", "#8da101", "#dfa000", "#3a94c5", "#df69ba", "#35a77c", "#5c6a72"}},
      {"nord-light", "#2e3440", "#eceff4", "#5e81ac",
        {"#4c566a", "#bf616a", "#a3be8c", "#ebcb8b", "#5e81ac", "#b48ead", "#88c0d0", "#d8dee9",
         "#2e3440", "#bf616a", "#a3be8c", "#ebcb8b", "#81a1c1", "#b48ead", "#8fbcbb", "#e5e9f0"}},
  };
  // clang-format on
  return list;
}

std::array<std::string, 16> synthesize_ansi_palette(const std::string& fg, const std::string& bg,
                                                     const std::string& accent) {
  const bool dark = color_is_dark(bg);
  const std::string& red = dark ? "#e06c75" : "#e45649";
  const std::string& green = dark ? "#98c379" : "#50a14f";
  const std::string& yellow = dark ? "#e5c07b" : "#c18401";
  const std::string& magenta = dark ? "#c678dd" : "#a626a4";
  const std::string& cyan = dark ? "#56b6c2" : "#0184bc";
  // Black/white are pinned to bg/fg (dark theme) or fg/bg (light theme) so
  // neofetch's swatch reads naturally against whichever the terminal is
  // actually showing, matching the convention every hand-authored palette
  // above follows.
  const std::string black = dark ? bg : mix_hex(bg, fg, 0.75);
  const std::string white = dark ? fg : bg;
  const std::string bright_black = mix_hex(black, fg, 0.4);
  const std::string bright_white = dark ? std::string("#ffffff") : mix_hex(bg, fg, 0.08);
  return {black, red, green, yellow, accent, magenta, cyan, white,
          bright_black, red, green, yellow, accent, magenta, cyan, bright_white};
}

}  // namespace mepwm::core
