#include "core/engine.hpp"

#include "core/icons.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace mepwm::core {

namespace {

struct Binding {
  Hotkey hotkey;
  Action action;
  int argument = 0;
};

// Keybinding policy lives here in core so every overlay platform behaves the
// same; platforms only translate Key/modifiers to native codes.
const std::vector<Binding>& default_bindings() {
  static const std::vector<Binding> bindings = [] {
    std::vector<Binding> list = {
        {{kModPrimary, Key::H}, Action::FocusLeft},
        {{kModPrimary, Key::J}, Action::FocusDown},
        {{kModPrimary, Key::K}, Action::FocusUp},
        {{kModPrimary, Key::L}, Action::FocusRight},
        {{kModPrimary | kModShift, Key::H}, Action::ResizeLeft},
        {{kModPrimary | kModShift, Key::J}, Action::ResizeDown},
        {{kModPrimary | kModShift, Key::K}, Action::ResizeUp},
        {{kModPrimary | kModShift, Key::L}, Action::ResizeRight},
        {{kModPrimary | kModCtrl, Key::H}, Action::SwapLeft},
        {{kModPrimary | kModCtrl, Key::J}, Action::SwapDown},
        {{kModPrimary | kModCtrl, Key::K}, Action::SwapUp},
        {{kModPrimary | kModCtrl, Key::L}, Action::SwapRight},
        {{kModPrimary, Key::V}, Action::SplitVertical},
        {{kModPrimary, Key::S}, Action::SplitHorizontal},
        {{kModPrimary, Key::N}, Action::NextTab},
        // Mod+p opens the application picker (X11 Super+p parity); previous
        // tab stays reachable via Mod+Shift+Tab.
        {{kModPrimary, Key::P}, Action::OpenAppPicker},
        {{kModPrimary, Key::I}, Action::OpenProjectPicker},
        {{kModPrimary, Key::O}, Action::OpenActiveProjectPicker},
        {{kModPrimary, Key::Tab}, Action::NextTab},
        {{kModPrimary | kModShift, Key::Tab}, Action::PrevTab},
        {{kModPrimary, Key::M}, Action::MergePane},
        {{kModPrimary, Key::D}, Action::MergePane},  // matches X11's Super+d in manual mode
        {{kModPrimary, Key::Minus}, Action::ShrinkMaster},
        {{kModPrimary, Key::Equal}, Action::GrowMaster},
        {{kModPrimary, Key::R}, Action::Retile},
        {{kModPrimary, Key::Return}, Action::SpawnTerminal},
        {{kModPrimary | kModShift, Key::Slash}, Action::ToggleHelpPanel},
        {{kModPrimary, Key::T}, Action::ToggleTodoPanel},
        {{kModPrimary, Key::F}, Action::ToggleHints},  // matches X11's Super+f
        {{kModPrimary, Key::B}, Action::ToggleBars},   // matches X11's Super+b
        {{kModPrimary | kModShift, Key::T}, Action::OpenThemePicker},
        {{kModPrimary | kModShift, Key::W}, Action::OpenWallpaperPicker},  // matches X11's Super+Shift+w
        {{kModPrimary | kModShift, Key::Q}, Action::Quit},
    };
    for (int number = 1; number <= 9; ++number) {
      const Key key = static_cast<Key>(static_cast<int>(Key::N1) + number - 1);
      list.push_back({{kModPrimary, key}, Action::SwitchWorkspace, number});
      list.push_back({{kModPrimary | kModShift, key}, Action::SendToWorkspace, number});
    }
    return list;
  }();
  return bindings;
}

const char* battery_icon(int percent) {
  if (percent >= 90) return icons::kBatteryFull;
  if (percent >= 65) return icons::kBattery75;
  if (percent >= 40) return icons::kBattery50;
  if (percent >= 15) return icons::kBattery25;
  return icons::kBatteryEmpty;
}

// Agent status files, same protocol as the X11 backend: one JSON per agent
// in $XDG_RUNTIME_DIR/mwm-agents (or /tmp/mwm-agents-$USER) with "agent",
// "status", "cwd", and optional "label" string fields.
std::string json_field(const std::string& json, const char* key) {
  const std::string needle = std::string("\"") + key + "\"";
  const std::size_t key_at = json.find(needle);
  if (key_at == std::string::npos) return {};
  const std::size_t colon = json.find(':', key_at);
  if (colon == std::string::npos) return {};
  const std::size_t quote = json.find('"', colon + 1);
  if (quote == std::string::npos) return {};
  const std::size_t end = json.find('"', quote + 1);
  return end == std::string::npos ? std::string{} : json.substr(quote + 1, end - quote - 1);
}

std::vector<AgentInfo> read_agent_status_files() {
  std::vector<AgentInfo> agents;
  const char* runtime = std::getenv("XDG_RUNTIME_DIR");
  const char* user = std::getenv("USER");
  const std::string directory = runtime != nullptr && *runtime != '\0'
                                    ? std::string(runtime) + "/mwm-agents"
                                    : std::string("/tmp/mwm-agents-") + (user != nullptr ? user : "mwm");
  std::error_code listing_error;
  for (const auto& entry : std::filesystem::directory_iterator(directory, listing_error)) {
    if (agents.size() >= 64 || entry.path().extension() != ".json") continue;
    std::ifstream file(entry.path());
    std::ostringstream stream;
    stream << file.rdbuf();
    const std::string json = stream.str();
    AgentInfo agent;
    agent.kind = json_field(json, "agent");
    agent.status = json_field(json, "status");
    agent.cwd = json_field(json, "cwd");
    agent.label = json_field(json, "label");
    if (agent.kind.empty() || agent.status.empty() || agent.cwd.empty()) continue;
    agent.needs_input = agent.status == "needs_input";
    agents.push_back(std::move(agent));
  }
  return agents;
}

// Project persistence, shared with the X11 backend: one directory path per
// line in $XDG_DATA_HOME/mepwm/projects, so both backends see the same list.
std::string project_state_path() {
  if (const char* data_home = std::getenv("XDG_DATA_HOME"); data_home != nullptr && *data_home != '\0') {
    return std::string(data_home) + "/mepwm/projects";
  }
  if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
    return std::string(home) + "/.local/share/mepwm/projects";
  }
  return {};
}

// Built-in theme collection, matching the X11 backend's palettes: half
// dark, half light. `accent` drives the highlight color and the
// focused-window border.
struct ThemeDef {
  const char* name;
  const char* fg;
  const char* bg;
  const char* accent;
};

const std::vector<ThemeDef>& themes() {
  static const std::vector<ThemeDef> list = {
      {"dark", "#f8f8f2", "#202124", "#5294e2"},
      {"nord", "#d8dee9", "#2e3440", "#88c0d0"},
      {"dracula", "#f8f8f2", "#282a36", "#bd93f9"},
      {"gruvbox-dark", "#ebdbb2", "#282828", "#fe8019"},
      {"tokyo-night", "#c0caf5", "#1a1b26", "#7aa2f7"},
      {"catppuccin-mocha", "#cdd6f4", "#1e1e2e", "#cba6f7"},
      {"one-dark", "#abb2bf", "#282c34", "#61afef"},
      {"everforest-dark", "#d3c6aa", "#2d353b", "#a7c080"},
      {"light", "#202124", "#f4f4f4", "#3971ed"},
      {"solarized-light", "#586e75", "#fdf6e3", "#268bd2"},
      {"gruvbox-light", "#3c3836", "#fbf1c7", "#d65d0e"},
      {"catppuccin-latte", "#4c4f69", "#eff1f5", "#8839ef"},
      {"rose-pine-dawn", "#575279", "#faf4ed", "#907aa9"},
      {"everforest-light", "#5c6a72", "#f3ead3", "#8da101"},
      {"nord-light", "#2e3440", "#eceff4", "#5e81ac"},
  };
  return list;
}

// "#rrggbb" blend of `base` toward `toward` by `amount` (0..1); used for the
// unfocused border so it stays visible without competing with content.
// Falls back to `base` unchanged for non-hex input.
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

// The chosen theme persists by name next to the projects file, so it
// survives restarts.
std::string theme_state_path() {
  if (const char* data_home = std::getenv("XDG_DATA_HOME"); data_home != nullptr && *data_home != '\0') {
    return std::string(data_home) + "/mepwm/theme";
  }
  if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
    return std::string(home) + "/.local/share/mepwm/theme";
  }
  return {};
}

// The chosen wallpaper persists by path next to the theme file.
std::string wallpaper_state_path() {
  if (const char* data_home = std::getenv("XDG_DATA_HOME"); data_home != nullptr && *data_home != '\0') {
    return std::string(data_home) + "/mepwm/wallpaper";
  }
  if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
    return std::string(home) + "/.local/share/mepwm/wallpaper";
  }
  return {};
}

// The default wallpaper dirs ("assets/light_comic_wallpapers" etc.) are
// relative, which only resolves against the current working directory --
// fine for `just run` from the repo root, but not for an installed mepwm
// launched from an arbitrary cwd (the LaunchAgent). Fall back to the
// installed data layout, where `cmake --install` puts assets/ (the engine's
// analogue of the X11 backend's exe-relative resolve_wallpaper_dir).
std::filesystem::path resolve_wallpaper_dir(const std::filesystem::path& dir) {
  std::error_code error;
  if (dir.is_absolute() || std::filesystem::exists(dir, error)) return dir;
  std::vector<std::filesystem::path> bases;
  if (const char* data_home = std::getenv("XDG_DATA_HOME"); data_home != nullptr && *data_home != '\0') {
    bases.emplace_back(std::filesystem::path(data_home) / "mepwm");
  }
  if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
    bases.emplace_back(std::filesystem::path(home) / ".local/share/mepwm");
  }
  bases.emplace_back("/usr/local/share/mepwm");
  for (const std::filesystem::path& base : bases) {
    std::filesystem::path candidate = base / dir;
    if (std::filesystem::exists(candidate, error)) return candidate;
  }
  return dir;
}

// Image files in one wallpaper directory, sorted -- the engine's analogue of
// the X11 backend's scan_wallpaper_dir. "~" expands to $HOME; relative
// directories resolve via resolve_wallpaper_dir above.
std::vector<std::string> scan_wallpaper_dir(const std::string& raw_dir) {
  std::vector<std::string> result;
  if (raw_dir.empty()) return result;
  std::filesystem::path dir(raw_dir);
  if (raw_dir == "~" || raw_dir.rfind("~/", 0) == 0) {
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
      dir = std::filesystem::path(home) / raw_dir.substr(raw_dir == "~" ? 1 : 2);
    }
  } else {
    dir = resolve_wallpaper_dir(dir);
  }
  std::error_code error;
  for (const auto& entry : std::filesystem::directory_iterator(dir, error)) {
    if (!entry.is_regular_file(error)) continue;
    std::string extension = entry.path().extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (extension != ".png" && extension != ".jpg" && extension != ".jpeg" &&
        extension != ".bmp" && extension != ".gif" && extension != ".webp" &&
        extension != ".heic" && extension != ".tiff") {
      continue;
    }
    result.push_back(entry.path().string());
  }
  std::sort(result.begin(), result.end());
  return result;
}

std::string project_label(const std::string& path) {
  const char* home = std::getenv("HOME");
  if (home != nullptr && path == home) return "default";
  const std::string label = std::filesystem::path(path).filename().string();
  return label.empty() ? path : label;
}

bool normalize_project_path(const std::string& raw_path, std::string* normalized) {
  if (raw_path.empty()) return false;
  std::filesystem::path path(raw_path);
  if (raw_path == "~" || raw_path.rfind("~/", 0) == 0) {
    const char* home = std::getenv("HOME");
    if (home == nullptr || *home == '\0') return false;
    path = std::filesystem::path(home) / raw_path.substr(raw_path == "~" ? 1 : 2);
  }
  std::error_code error;
  path = std::filesystem::weakly_canonical(path, error);
  if (error || !std::filesystem::is_directory(path, error)) return false;
  *normalized = path.string();
  return true;
}

std::string shell_quote(const std::string& value) {
  std::string quoted = "'";
  for (const char character : value) {
    if (character == '\'') {
      quoted += "'\\''";
    } else {
      quoted += character;
    }
  }
  quoted += "'";
  return quoted;
}

// TODO.org resolution for the todo sidebar, project-centric like X11:
// explicit override first, then the active project's directory, then the
// launch directory, then $HOME.
std::string todo_file_path(const std::string& project_path) {
  if (const char* override_path = std::getenv("MEPWM_TODO_FILE")) return override_path;
  if (!project_path.empty()) return project_path + "/TODO.org";
  if (std::ifstream("TODO.org").good()) return "TODO.org";
  if (const char* home = std::getenv("HOME")) return std::string(home) + "/TODO.org";
  return "TODO.org";
}

// Org-mode plumbing for the todo sidebar, ported from the X11 backend so
// both backends read and write the same TODO.org format (headlines with
// TODO/DONE keywords, :LOGBOOK: drawers with org-clock CLOCK lines).

std::vector<std::string> read_file_lines(const std::string& path) {
  std::vector<std::string> lines;
  std::ifstream file(path);
  for (std::string line; std::getline(file, line);) lines.push_back(std::move(line));
  return lines;
}

void write_file_lines(const std::string& path, const std::vector<std::string>& lines) {
  std::ofstream file(path);
  for (const std::string& line : lines) file << line << '\n';
}

std::string trim(const std::string& value) {
  const std::size_t first = value.find_first_not_of(" \t");
  if (first == std::string::npos) return {};
  const std::size_t last = value.find_last_not_of(" \t");
  return value.substr(first, last - first + 1);
}

// True if `line` is an org headline ("* TODO text") with the given keyword;
// on success, *keyword_start/*text_start bound the keyword and the headline
// text that follows it.
bool parse_org_headline(const std::string& line, const std::string& keyword,
                        std::size_t* keyword_start, std::size_t* text_start) {
  const std::size_t stars = line.find_first_not_of('*');
  if (stars == 0 || stars == std::string::npos || line[stars] != ' ') return false;
  *keyword_start = line.find_first_not_of(' ', stars + 1);
  if (*keyword_start == std::string::npos) return false;
  const std::size_t space = line.find(' ', *keyword_start);
  if (space == std::string::npos ||
      line.compare(*keyword_start, space - *keyword_start, keyword) != 0) {
    return false;
  }
  *text_start = line.find_first_not_of(' ', space + 1);
  return *text_start != std::string::npos;
}

// True if `line` is any org headline, regardless of keyword -- used to find
// the end of a headline's body (its :LOGBOOK: drawer) when scanning.
bool is_org_headline(const std::string& line) {
  const std::size_t stars = line.find_first_not_of('*');
  return stars != 0 && stars != std::string::npos && line[stars] == ' ';
}

// Formats a time as an org-mode clock timestamp: "[2026-08-07 Fri 10:23]".
std::string org_timestamp(std::time_t time) {
  char buffer[32];
  std::tm local{};
  if (localtime_r(&time, &local) == nullptr) return {};
  std::strftime(buffer, sizeof buffer, "[%Y-%m-%d %a %H:%M]", &local);
  return buffer;
}

// Inverse of org_timestamp, given the text between the brackets
// ("2026-08-07 Fri 10:23").
std::time_t parse_org_timestamp(const std::string& text) {
  std::tm parsed{};
  std::istringstream stream(text);
  std::string date;
  std::string day;
  std::string time;
  stream >> date >> day >> time;
  char dash = 0;
  char colon = 0;
  std::istringstream(date) >> parsed.tm_year >> dash >> parsed.tm_mon >> dash >> parsed.tm_mday;
  std::istringstream(time) >> parsed.tm_hour >> colon >> parsed.tm_min;
  parsed.tm_year -= 1900;
  parsed.tm_mon -= 1;
  parsed.tm_isdst = -1;
  return std::mktime(&parsed);
}

// Finds an open (unterminated, i.e. no "--" end range) CLOCK line inside
// `headline`'s body, stopping at the next headline or EOF.
std::optional<std::size_t> find_open_clock_line(const std::vector<std::string>& lines,
                                                std::size_t headline) {
  for (std::size_t scan = headline + 1; scan < lines.size() && !is_org_headline(lines[scan]);
       ++scan) {
    const std::size_t clock_at = lines[scan].find("CLOCK:");
    if (clock_at != std::string::npos && lines[scan].find("--", clock_at) == std::string::npos) {
      return scan;
    }
  }
  return std::nullopt;
}

// Extracts the start time from an open CLOCK line ("CLOCK: [2026-08-08 Sat
// 07:19]"), i.e. the timestamp inside the first bracket pair.
std::optional<std::time_t> parse_clock_start(const std::string& clock_line) {
  const std::size_t open = clock_line.find('[');
  if (open == std::string::npos) return std::nullopt;
  const std::size_t close = clock_line.find(']', open);
  if (close == std::string::npos) return std::nullopt;
  return parse_org_timestamp(clock_line.substr(open + 1, close - open - 1));
}

// Renders elapsed time the way a running timer reads at a glance: just
// seconds under a minute, just minutes under an hour, hours+minutes above.
std::string format_elapsed(std::time_t seconds) {
  seconds = std::max<std::time_t>(seconds, 0);
  if (seconds < 60) return std::to_string(seconds) + "s";
  const long total_minutes = seconds / 60;
  if (total_minutes < 60) return std::to_string(total_minutes) + "m";
  char buffer[32];
  std::snprintf(buffer, sizeof buffer, "%ldh%02ldm", total_minutes / 60, total_minutes % 60);
  return buffer;
}

// Screen-edge chrome geometry, mirroring the X11 backend's bar and docks.
constexpr int kTopBarHeight = 30;
constexpr int kBottomBarHeight = 30;
constexpr int kDockWidth = 40;
constexpr int kTabBarHeight = 24;

// Left-dock application launcher, mirroring the X11 backend's hardcoded
// column (draw_docks). An empty command means "the configured terminal"
// (Config::terminal). Commands are per-platform: `open -a` bundle launches
// on macOS, PATH lookups elsewhere.
struct LauncherApp {
  const char* icon;
  const char* label;  // text fallback when the chrome font lacks icons
  const char* command;
};
#if defined(__APPLE__)
constexpr LauncherApp kLauncherApps[] = {
    {icons::kFirefox, "ff", "open -a Firefox"},
    {icons::kTerminal, "term", ""},
    {icons::kInkscape, "ink", "open -a Inkscape"},
    {icons::kGimp, "gimp", "open -a GIMP"},
    {icons::kLibreOffice, "odf", "open -a LibreOffice"},
    {icons::kVsCode, "code", "open -a 'Visual Studio Code'"},
    {icons::kEmacs, "emacs", "open -a Emacs"},
    {icons::kNeovim, "nvim", "open -a Neovide || open -a MacVim"},
};
#else
constexpr LauncherApp kLauncherApps[] = {
    {icons::kFirefox, "ff", "firefox"},
    {icons::kTerminal, "term", ""},
    {icons::kInkscape, "ink", "inkscape"},
    {icons::kGimp, "gimp", "gimp"},
    {icons::kLibreOffice, "odf", "libreoffice"},
    {icons::kVsCode, "code", "code"},
    {icons::kEmacs, "emacs", "emacs"},
    {icons::kNeovim, "nvim", "${TERMINAL:-xterm} -e nvim"},
};
#endif
constexpr std::size_t kLauncherAppCount = sizeof(kLauncherApps) / sizeof(kLauncherApps[0]);

// Side-panel text metrics mirrored from the platform renderer (18px line
// labels below a ~40px header, plus the footer strip): the engine needs them
// to window the todo list around the keyboard selection.
constexpr int kPanelLineHeight = 18;
constexpr int kPanelChromeHeight = 70;  // header + footer + margins

std::string clock_text() {
  char buffer[32];
  const std::time_t now = std::time(nullptr);
  std::tm local{};
  if (localtime_r(&now, &local) == nullptr) return {};
  if (std::strftime(buffer, sizeof buffer, "%a %H:%M", &local) == 0) return {};
  return buffer;
}

Modifier parse_modifier(const std::string& name) {
  if (name == "cmd" || name == "command") return Modifier::Cmd;
  if (name == "alt" || name == "option") return Modifier::Alt;
  if (name == "ctrl" || name == "control") return Modifier::Ctrl;
  if (name == "super" || name == "win") return Modifier::Super;
  if (name == "fn" || name == "globe") return Modifier::Fn;
  if (name == "fn+cmd" || name == "cmd+fn" || name == "globe+cmd" || name == "cmd+globe") {
    return Modifier::FnCmd;
  }
  throw std::invalid_argument("unknown modifier: " + name + " (use cmd|alt|ctrl|super|fn|fn+cmd)");
}

Direction action_direction(Action action) {
  switch (action) {
    case Action::FocusLeft:
    case Action::SwapLeft:
      return Direction::Left;
    case Action::FocusDown:
    case Action::SwapDown:
      return Direction::Down;
    case Action::FocusUp:
    case Action::SwapUp:
      return Direction::Up;
    case Action::ResizeLeft:
      return Direction::Left;
    case Action::ResizeDown:
      return Direction::Down;
    case Action::ResizeUp:
      return Direction::Up;
    default:
      return Direction::Right;
  }
}

}  // namespace

TilingEngine::TilingEngine(std::unique_ptr<Platform> platform) : platform_(std::move(platform)) {}

int TilingEngine::run(const Config& config) {
  config_ = config;
  layout_.gap = config.gap;
  layout_.mfact = config.mfact;
  layout_.nmaster = config.nmaster;

  std::string error;
  if (!platform_->initialize(parse_modifier(config.modifier), &error)) {
    throw std::runtime_error(error.empty() ? "platform failed to initialize" : error);
  }
  icons_ = platform_->has_icon_font();
  for (const Binding& binding : default_bindings()) {
    if (!platform_->register_hotkey(binding.hotkey, binding.action, binding.argument)) {
      std::cerr << "mepwm: warning: failed to register a hotkey; another app may hold it\n";
    }
  }

  initialize_projects();
  load_theme();
  load_wallpaper();
  reconcile_windows();
  focused_ = platform_->focused_window();
  retile();
  platform_->run([this](const Event& event) { handle_event(event); });
  return 0;
}

void TilingEngine::handle_event(const Event& event) {
  switch (event.type) {
    case Event::Type::WindowsChanged:
      reconcile_windows();
      retile();
      break;
    case Event::Type::FocusChanged: {
      focused_ = event.window;
      const int workspace = workspace_of(focused_);
      if (workspace >= 0 && static_cast<std::size_t>(workspace) != current_workspace_) {
        // Focus landed on a window of another workspace (Cmd-Tab, Dock
        // click): follow it there.
        switch_workspace(static_cast<std::size_t>(workspace));
      } else if (workspace >= 0) {
        // Selection follows focus; an occluded tab reached via Cmd-Tab
        // becomes its pane's active tab.
        PaneNode* leaf = leaf_of(ws().root.get(), focused_);
        ws().selected = leaf;
        if (leaf->active_window() != focused_) {
          const auto found = std::find(leaf->tabs.begin(), leaf->tabs.end(), focused_);
          leaf->active_tab = static_cast<std::size_t>(found - leaf->tabs.begin());
          retile();
        } else {
          refresh_borders();
          refresh_tab_bars();
          refresh_chrome();
        }
      } else {
        refresh_borders();
        refresh_chrome();
      }
      break;
    }
    case Event::Type::ScreenChanged:
      retile();
      break;
    case Event::Type::Tick:
      // Widget probes may launch subprocesses; sample them on a coarse
      // cadence (~5s at the 0.5s tick rate), not every tick.
      if (tick_count_ % 10 == 0) {
        status_ = platform_->system_status();
        refresh_agents();
        if (side_panel_ != PanelKind::None && side_panel_ != PanelKind::Help) {
          refresh_side_panel();
        }
      }
      ++tick_count_;
      advance_pomodoro();
      refresh_chrome();
      break;
    case Event::Type::ChromeClicked:
      handle_chrome_click(event);
      break;
    case Event::Type::PanelKeyPressed:
      handle_panel_key(event);
      break;
    case Event::Type::PickerSelected:
      handle_picker_selected(event);
      break;
    case Event::Type::PickerHighlighted:
      if (pending_picker_ == PendingPicker::Themes) {
        preview_theme(event.cell.empty()
                          ? -1
                          : static_cast<int>(std::strtoull(event.cell.c_str(), nullptr, 10)));
      }
      break;
    case Event::Type::PickerCancelled:
      if (pending_picker_ == PendingPicker::Themes) preview_theme(-1);
      pending_picker_ = PendingPicker::None;
      break;
    case Event::Type::ActionTriggered:
      switch (event.action) {
        case Action::FocusLeft:
        case Action::FocusDown:
        case Action::FocusUp:
        case Action::FocusRight:
          focus_direction(action_direction(event.action));
          break;
        case Action::SwapLeft:
        case Action::SwapDown:
        case Action::SwapUp:
        case Action::SwapRight:
          move_direction(action_direction(event.action));
          break;
        case Action::ResizeLeft:
        case Action::ResizeDown:
        case Action::ResizeUp:
        case Action::ResizeRight:
          resize_direction(action_direction(event.action));
          break;
        case Action::SplitVertical:
          split_pane(true);
          break;
        case Action::SplitHorizontal:
          split_pane(false);
          break;
        case Action::NextTab:
          cycle_tab(1);
          break;
        case Action::PrevTab:
          cycle_tab(-1);
          break;
        case Action::MergePane:
          merge_pane();
          break;
        case Action::ShrinkMaster:
          adjust_ratio(-0.05F);
          break;
        case Action::GrowMaster:
          adjust_ratio(0.05F);
          break;
        case Action::Retile:
          reconcile_windows();
          retile();
          break;
        case Action::SpawnTerminal:
          platform_->spawn(config_.terminal);
          break;
        case Action::OpenAppPicker:
          // The app picker replaces a showing theme picker without a cancel
          // event; drop any uncommitted preview first.
          if (pending_picker_ == PendingPicker::Themes) preview_theme(-1);
          platform_->toggle_app_picker();
          break;
        case Action::OpenProjectPicker:
          open_project_picker(false);
          break;
        case Action::OpenActiveProjectPicker:
          open_project_picker(true);
          break;
        case Action::OpenThemePicker:
          open_theme_picker();
          break;
        case Action::OpenWallpaperPicker:
          open_wallpaper_picker();
          break;
        case Action::ToggleHelpPanel:
          toggle_side_panel(PanelKind::Help);
          break;
        case Action::ToggleHints:
          platform_->toggle_hints();
          break;
        case Action::ToggleBars:
          // Hide/show all border chrome together and reclaim its screen
          // space, X11 toggle_bar() style: tiling_area() drops the margins
          // while hidden, so the retile stretches windows edge-to-edge.
          bars_visible_ = !bars_visible_;
          retile();
          break;
        case Action::ToggleTodoPanel:
          toggle_side_panel(PanelKind::Todo);
          break;
        case Action::SwitchWorkspace:
          switch_workspace(static_cast<std::size_t>(event.argument - 1));
          break;
        case Action::SendToWorkspace:
          send_to_workspace(static_cast<std::size_t>(event.argument - 1));
          break;
        case Action::Quit:
          platform_->stop();
          break;
      }
      break;
  }
}

std::vector<WindowId> TilingEngine::windows_in(std::size_t workspace) const {
  std::vector<WindowId> ids;
  collect_windows(workspaces_[workspace].root.get(), ids);
  return ids;
}

int TilingEngine::workspace_of(WindowId window) const {
  if (window == kNoWindow) return -1;
  for (std::size_t index = 0; index < kWorkspaceCount; ++index) {
    if (leaf_of(workspaces_[index].root.get(), window) != nullptr) {
      return static_cast<int>(index);
    }
  }
  return -1;
}

void TilingEngine::ensure_root() {
  WorkspaceState& state = ws();
  if (state.root == nullptr) {
    state.root = std::make_unique<PaneNode>();
    state.selected = state.root.get();
  }
  if (state.selected == nullptr) state.selected = first_leaf(state.root.get());
}

PaneNode* TilingEngine::current_leaf() {
  if (PaneNode* leaf = leaf_of(ws().root.get(), focused_)) return leaf;
  ensure_root();
  return ws().selected;
}

void TilingEngine::activate_window(WindowId window) {
  const int workspace = workspace_of(window);
  if (workspace < 0) return;
  if (static_cast<std::size_t>(workspace) != current_workspace_) {
    switch_workspace(static_cast<std::size_t>(workspace));
  }
  PaneNode* leaf = leaf_of(ws().root.get(), window);
  if (leaf == nullptr) return;
  const auto found = std::find(leaf->tabs.begin(), leaf->tabs.end(), window);
  leaf->active_tab = static_cast<std::size_t>(found - leaf->tabs.begin());
  ws().selected = leaf;
  focused_ = window;
  platform_->focus_window(window);
  retile();
}

void TilingEngine::notify(const std::string& text) {
  char stamp[8] = {0};
  const std::time_t now = std::time(nullptr);
  std::tm local{};
  if (localtime_r(&now, &local) != nullptr) std::strftime(stamp, sizeof stamp, "%H:%M", &local);
  notifications_.insert(notifications_.begin(), std::string(stamp) + "  " + text);
  if (notifications_.size() > 50) notifications_.resize(50);
}

void TilingEngine::refresh_agents() {
  agents_ = read_agent_status_files();
  // Merge the platform's process scan, deduped by kind+cwd like X11 does
  // (a status file is richer than a bare process hit).
  for (const AgentInfo& scanned : status_.agents) {
    const bool known = std::any_of(agents_.begin(), agents_.end(), [&](const AgentInfo& agent) {
      return agent.kind == scanned.kind && (scanned.cwd.empty() || agent.cwd == scanned.cwd);
    });
    if (!known && agents_.size() < 64) agents_.push_back(scanned);
  }
}

void TilingEngine::reconcile_windows() {
  const std::vector<WindowInfo> current = platform_->list_windows();
  const std::unordered_map<WindowId, WindowInfo> previous = std::move(windows_);
  windows_.clear();
  for (const WindowInfo& info : current) windows_[info.id] = info;
  if (!first_reconcile_) {
    for (const auto& [id, info] : windows_) {
      if (previous.count(id) == 0) notify("opened  " + info.application);
    }
    for (const auto& [id, info] : previous) {
      if (windows_.count(id) == 0) notify("closed  " + info.application);
    }
  }
  first_reconcile_ = false;

  // Drop vanished windows from every workspace tree, including the stashed
  // trees of inactive projects (empty panes prune themselves unless
  // selected); newcomers open as tabs in the selected pane of the active
  // workspace, X11-manual-layout style.
  const auto prune = [this](WorkspaceState& state) {
    if (state.root == nullptr) return;
    std::vector<WindowId> ids;
    collect_windows(state.root.get(), ids);
    for (WindowId id : ids) {
      if (windows_.count(id) == 0) remove_window(state.root, id, &state.selected);
    }
  };
  for (WorkspaceState& state : workspaces_) prune(state);
  for (ProjectState& project : projects_) {
    for (WorkspaceState& state : project.workspaces) prune(state);
  }
  for (const WindowInfo& info : current) {
    if (workspace_of(info.id) >= 0) continue;
    // Windows parked by an inactive project still belong to it; adopting
    // them here would pull every backgrounded project into this workspace.
    if (in_stashed_project(info.id)) continue;
    ensure_root();
    PaneNode* leaf = ws().selected;
    leaf->tabs.push_back(info.id);
    leaf->active_tab = leaf->tabs.size() - 1;
  }
  if (focused_ != kNoWindow && windows_.count(focused_) == 0) focused_ = kNoWindow;
}

void TilingEngine::park_window(WindowId window) {
  auto entry = windows_.find(window);
  if (entry == windows_.end()) return;
  const Rect full = platform_->work_area();
  Rect frame = entry->second.frame;
  // Bottom-right corner, almost entirely off the work area; macOS keeps a
  // sliver visible, which doubles as a hint that the window still exists.
  frame.x = full.x + full.width - 12;
  frame.y = full.y + full.height - 12;
  platform_->set_window_frame(window, frame);
  entry->second.frame = frame;
}

void TilingEngine::switch_workspace(std::size_t index) {
  if (index >= kWorkspaceCount || index == current_workspace_) return;
  for (WindowId id : windows_in(current_workspace_)) park_window(id);
  current_workspace_ = index;
  if (workspace_of(focused_) != static_cast<int>(index)) {
    WorkspaceState& state = ws();
    PaneNode* landing = state.selected != nullptr ? state.selected : first_leaf(state.root.get());
    focused_ = landing != nullptr ? landing->active_window() : kNoWindow;
    if (focused_ != kNoWindow) platform_->focus_window(focused_);
  }
  retile();
}

void TilingEngine::send_to_workspace(std::size_t index) {
  if (index >= kWorkspaceCount || index == current_workspace_) return;
  if (focused_ == kNoWindow || workspace_of(focused_) != static_cast<int>(current_workspace_)) {
    return;
  }
  const WindowId moved = focused_;
  WorkspaceState& source = ws();
  remove_window(source.root, moved, &source.selected);
  WorkspaceState& target = workspaces_[index];
  if (target.root == nullptr) {
    target.root = std::make_unique<PaneNode>();
    target.selected = target.root.get();
  }
  PaneNode* leaf = target.selected != nullptr ? target.selected : first_leaf(target.root.get());
  leaf->tabs.push_back(moved);
  leaf->active_tab = leaf->tabs.size() - 1;
  park_window(moved);
  PaneNode* landing = source.selected != nullptr ? source.selected : first_leaf(source.root.get());
  focused_ = landing != nullptr ? landing->active_window() : kNoWindow;
  if (focused_ != kNoWindow) platform_->focus_window(focused_);
  retile();
}

void TilingEngine::initialize_projects() {
  std::ifstream file(project_state_path());
  for (std::string path; std::getline(file, path);) add_project(path, false);
  if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
    add_project(home, false);
  }
  if (projects_.empty()) {
    std::error_code error;
    add_project(std::filesystem::current_path(error).string(), false);
  }
  if (!projects_.empty()) save_projects();
}

void TilingEngine::save_projects() const {
  const std::string state_path = project_state_path();
  if (state_path.empty()) return;
  std::error_code error;
  std::filesystem::create_directories(std::filesystem::path(state_path).parent_path(), error);
  std::ofstream file(state_path);
  if (!file) return;
  for (const ProjectState& project : projects_) file << project.path << '\n';
}

bool TilingEngine::add_project(const std::string& raw_path, bool save) {
  std::string normalized;
  if (!normalize_project_path(raw_path, &normalized)) return false;
  if (std::any_of(projects_.begin(), projects_.end(),
                  [&](const ProjectState& project) { return project.path == normalized; })) {
    return true;
  }
  projects_.push_back({normalized, {}});
  if (save) save_projects();
  return true;
}

std::string TilingEngine::active_project_path() const {
  return active_project_ < projects_.size() ? projects_[active_project_].path : std::string();
}

bool TilingEngine::project_has_windows(std::size_t index) const {
  if (index >= projects_.size()) return false;
  const auto& workspaces = index == active_project_ ? workspaces_ : projects_[index].workspaces;
  return std::any_of(workspaces.begin(), workspaces.end(), [](const WorkspaceState& state) {
    std::vector<WindowId> ids;
    collect_windows(state.root.get(), ids);
    return !ids.empty();
  });
}

bool TilingEngine::in_stashed_project(WindowId window) const {
  // The active project's stash is empty (its set lives in workspaces_), so
  // this only ever matches windows owned by inactive projects.
  for (const ProjectState& project : projects_) {
    for (const WorkspaceState& state : project.workspaces) {
      if (leaf_of(state.root.get(), window) != nullptr) return true;
    }
  }
  return false;
}

void TilingEngine::switch_project(std::size_t index) {
  if (index >= projects_.size()) return;
  if (index == active_project_) {
    switch_workspace(0);
    return;
  }
  // Overlay analogue of X11's hide_workspace: the outgoing project's visible
  // windows get parked; the incoming project's are already parked and the
  // retile below restores its first workspace.
  for (WindowId id : windows_in(current_workspace_)) park_window(id);
  projects_[active_project_].workspaces = std::move(workspaces_);
  active_project_ = index;
  workspaces_ = std::move(projects_[active_project_].workspaces);
  current_workspace_ = 0;
  WorkspaceState& state = ws();
  PaneNode* landing = state.selected != nullptr ? state.selected : first_leaf(state.root.get());
  focused_ = landing != nullptr ? landing->active_window() : kNoWindow;
  if (focused_ != kNoWindow) platform_->focus_window(focused_);
  retile();
  save_projects();
}

void TilingEngine::open_project_picker(bool active_only) {
  // Replacing the theme picker mid-preview skips its cancel event; drop the
  // preview here so the uncommitted theme doesn't stick around.
  if (pending_picker_ == PendingPicker::Themes) preview_theme(-1);
  std::vector<PickerItem> items;
  for (std::size_t index = 0; index < projects_.size(); ++index) {
    if (active_only && !project_has_windows(index)) continue;
    std::string label = projects_[index].path;
    if (index == active_project_) label += "  (current)";
    items.push_back({std::to_string(index), std::move(label)});
  }
  pending_picker_ = active_only ? PendingPicker::ActiveProjects : PendingPicker::Projects;
  platform_->show_list_picker(active_only ? "Active projects" : "Projects", items);
}

void TilingEngine::open_theme_picker() {
  std::vector<PickerItem> items;
  const auto& list = themes();
  for (std::size_t index = 0; index < list.size(); ++index) {
    std::string label = list[index].name;
    if (index == theme_index_) label += "  (current)";
    items.push_back({std::to_string(index), std::move(label)});
  }
  pending_picker_ = PendingPicker::Themes;
  // Start highlighted on the committed theme so opening the picker doesn't
  // itself change anything until the user navigates (X11 parity).
  platform_->show_list_picker("Theme", items, std::to_string(theme_index_));
}

std::size_t TilingEngine::effective_theme() const {
  return preview_theme_ >= 0 ? static_cast<std::size_t>(preview_theme_) : theme_index_;
}

void TilingEngine::sync_theme_colors() {
  const ThemeDef& theme = themes()[effective_theme()];
  // Focused border tracks the theme accent (matching the bar's highlight
  // color); the unfocused border is a subtle bg/fg blend (X11 parity).
  config_.border_color_focused = theme.accent;
  config_.border_color_normal = mix_hex(theme.bg, theme.fg, 0.35);
}

void TilingEngine::apply_theme(std::size_t index) {
  if (index >= themes().size()) return;
  preview_theme_ = -1;
  theme_index_ = index;
  sync_theme_colors();
  save_theme();
  // Chrome first: the platform reuses the chrome palette for tab bars,
  // side panels, and the picker.
  refresh_chrome();
  refresh_borders();
  refresh_tab_bars();
  refresh_side_panel();
}

void TilingEngine::preview_theme(int index) {
  if (index >= 0 && static_cast<std::size_t>(index) >= themes().size()) return;
  if (preview_theme_ == index) return;
  preview_theme_ = index;
  sync_theme_colors();
  refresh_chrome();
  refresh_borders();
  refresh_tab_bars();
  refresh_side_panel();
}

void TilingEngine::save_theme() const {
  const std::string state_path = theme_state_path();
  if (state_path.empty()) return;
  std::error_code error;
  std::filesystem::create_directories(std::filesystem::path(state_path).parent_path(), error);
  std::ofstream file(state_path);
  if (file) file << themes()[theme_index_].name << '\n';
}

void TilingEngine::load_theme() {
  std::ifstream file(theme_state_path());
  std::string name;
  if (file) std::getline(file, name);
  const auto& list = themes();
  for (std::size_t index = 0; index < list.size(); ++index) {
    if (name == list[index].name) {
      theme_index_ = index;
      break;
    }
  }
  sync_theme_colors();
}

void TilingEngine::open_wallpaper_picker() {
  // Replacing the theme picker mid-preview skips its cancel event; drop the
  // preview here so the uncommitted theme doesn't stick around.
  if (pending_picker_ == PendingPicker::Themes) preview_theme(-1);
  // Both theme buckets combined into one deduplicated, sorted list -- pick
  // any wallpaper regardless of which bucket the active theme draws from
  // (X11 parity).
  std::vector<std::string> paths = scan_wallpaper_dir(config_.wallpaper_dir_light);
  const std::vector<std::string> dark = scan_wallpaper_dir(config_.wallpaper_dir_dark);
  paths.insert(paths.end(), dark.begin(), dark.end());
  std::sort(paths.begin(), paths.end());
  paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
  if (paths.empty()) {
    notify("No wallpapers found (MEPWM_WALLPAPER_LIGHT_DIR / _DARK_DIR)");
    return;
  }
  std::vector<PickerItem> items;
  for (const std::string& path : paths) {
    std::string label = std::filesystem::path(path).filename().string();
    if (path == wallpaper_path_) label += "  (current)";
    items.push_back({path, std::move(label)});
  }
  pending_picker_ = PendingPicker::Wallpapers;
  platform_->show_list_picker("Wallpaper", items, wallpaper_path_);
}

void TilingEngine::apply_wallpaper(const std::string& path) {
  wallpaper_path_ = path;
  platform_->perform(SystemAction::SetWallpaper, 0, path);
  const std::string state_path = wallpaper_state_path();
  if (state_path.empty()) return;
  std::error_code error;
  std::filesystem::create_directories(std::filesystem::path(state_path).parent_path(), error);
  std::ofstream file(state_path);
  if (file) file << wallpaper_path_ << '\n';
}

void TilingEngine::load_wallpaper() {
  std::ifstream file(wallpaper_state_path());
  std::string path;
  if (file) std::getline(file, path);
  if (path.empty() || !std::filesystem::exists(path)) return;
  wallpaper_path_ = path;
  platform_->perform(SystemAction::SetWallpaper, 0, path);
}

void TilingEngine::handle_picker_selected(const Event& event) {
  const PendingPicker pending = pending_picker_;
  pending_picker_ = PendingPicker::None;
  if (pending == PendingPicker::None) return;
  if (pending == PendingPicker::Themes) {
    if (event.cell.empty()) {
      preview_theme(-1);  // Enter with no match confirms nothing: revert
      return;
    }
    apply_theme(static_cast<std::size_t>(std::strtoull(event.cell.c_str(), nullptr, 10)));
    return;
  }
  if (pending == PendingPicker::Wallpapers) {
    if (!event.cell.empty()) apply_wallpaper(event.cell);
    return;
  }
  // A typed, valid directory takes precedence over fuzzy matches (X11
  // behavior): it adds and opens a project that isn't in the list yet.
  if (pending == PendingPicker::Projects && !event.text.empty()) {
    std::string entered_path;
    if (normalize_project_path(event.text, &entered_path)) {
      add_project(entered_path);
      const auto found =
          std::find_if(projects_.begin(), projects_.end(),
                       [&](const ProjectState& project) { return project.path == entered_path; });
      if (found == projects_.end()) return;
      switch_project(static_cast<std::size_t>(found - projects_.begin()));
      spawn_terminal_in(entered_path);
      return;
    }
  }
  if (event.cell.empty()) return;
  const std::size_t index = static_cast<std::size_t>(std::strtoull(event.cell.c_str(), nullptr, 10));
  if (index >= projects_.size()) return;
  const std::string path = projects_[index].path;
  switch_project(index);
  // The recent-projects picker doubles as "start working here": it opens a
  // terminal in the project, matching X11. The active-projects picker only
  // switches.
  if (pending == PendingPicker::Projects) spawn_terminal_in(path);
}

void TilingEngine::spawn_terminal_in(const std::string& directory) {
  // The configured terminal command typically hardcodes "~" as the place to
  // open (e.g. "open -a kitty ~ || open -a Terminal ~"). Rewrite standalone
  // "~" arguments to the project directory, and cd there for commands that
  // just inherit their working directory.
  const std::string quoted = shell_quote(directory);
  std::istringstream stream(config_.terminal);
  std::string rewritten;
  for (std::string token; stream >> token;) {
    if (!rewritten.empty()) rewritten += ' ';
    rewritten += token == "~" ? quoted : token;
  }
  if (rewritten.empty()) rewritten = config_.terminal;
  platform_->spawn("cd " + quoted + " && (" + rewritten + ")");
}

Rect TilingEngine::tiling_area() {
  const Rect full = platform_->work_area();
  // Only reserve space for the border chrome while it's shown (mod+b).
  const int dock_margin = bars_visible_ ? kDockWidth : 0;
  const int bar_margin = bars_visible_ ? kTopBarHeight : 0;
  const int bottom_margin = bars_visible_ ? kBottomBarHeight : 0;
  return {full.x + dock_margin, full.y + bar_margin,
          std::max(1, full.width - 2 * dock_margin),
          std::max(1, full.height - bar_margin - bottom_margin)};
}

std::vector<TilingEngine::VisiblePane> TilingEngine::compute_visible_panes() {
  std::vector<VisiblePane> panes;
  PaneNode* root = ws().root.get();
  if (root == nullptr) return panes;
  const int gap = static_cast<int>(layout_.gap);
  // Inset the outer edge by just the border width so the focus ring sits
  // flush against the chrome; the inter-pane gap holds both panes' rings.
  const int edge = static_cast<int>(config_.border_width);
  const Rect outer = tiling_area();
  const Rect area{outer.x + edge, outer.y + edge, std::max(1, outer.width - 2 * edge),
                  std::max(1, outer.height - 2 * edge)};
  std::vector<PaneFrame> frames;
  layout_panes(root, area, gap, frames);
  for (const PaneFrame& pane : frames) {
    VisiblePane visible;
    visible.leaf = pane.leaf;
    visible.content = pane.frame;
    if (pane.leaf->tabs.size() > 1) {
      visible.has_tab_bar = true;
      visible.tab_bar = {pane.frame.x, pane.frame.y, pane.frame.width, kTabBarHeight};
      visible.content.y += kTabBarHeight + 4;
      visible.content.height = std::max(1, visible.content.height - kTabBarHeight - 4);
    }
    panes.push_back(visible);
  }
  return panes;
}

void TilingEngine::retile() {
  for (const VisiblePane& pane : compute_visible_panes()) {
    // Tabs stack: every tab gets the pane's frame, the active one on top.
    for (WindowId id : pane.leaf->tabs) {
      platform_->set_window_frame(id, pane.content);
      auto entry = windows_.find(id);
      if (entry != windows_.end()) entry->second.frame = pane.content;
    }
    if (const WindowId active = pane.leaf->active_window(); active != kNoWindow) {
      platform_->raise_window(active);
    }
  }
  refresh_borders();
  refresh_tab_bars();
  refresh_chrome();
}

void TilingEngine::refresh_tab_bars() {
  std::vector<TabBar> bars;
  for (const VisiblePane& pane : compute_visible_panes()) {
    if (!pane.has_tab_bar) continue;
    TabBar bar;
    bar.frame = pane.tab_bar;
    for (std::size_t index = 0; index < pane.leaf->tabs.size(); ++index) {
      const WindowId id = pane.leaf->tabs[index];
      const auto entry = windows_.find(id);
      std::string title = entry != windows_.end()
                              ? (entry->second.title.empty() ? entry->second.application
                                                             : entry->second.title)
                              : std::string("?");
      bar.tabs.push_back({std::move(title), index == pane.leaf->active_tab, std::to_string(id)});
    }
    bars.push_back(std::move(bar));
  }
  platform_->update_tab_bars(bars);
}

void TilingEngine::refresh_chrome() {
  const Rect full = platform_->work_area();
  Chrome chrome;
  chrome.visible = bars_visible_;
  if (!bars_visible_) {
    // Keep the theme colors current (tab bars reuse the chrome palette) but
    // skip building bar content nobody will see.
    const ThemeDef& hidden_theme = themes()[effective_theme()];
    chrome.background_color = hidden_theme.bg;
    chrome.text_color = hidden_theme.fg;
    chrome.accent_color = hidden_theme.accent;
    platform_->update_chrome(chrome);
    return;
  }
  chrome.top = {full.x, full.y, full.width, kTopBarHeight};
  chrome.bottom = {full.x, full.y + full.height - kBottomBarHeight, full.width, kBottomBarHeight};
  chrome.left = {full.x, full.y + kTopBarHeight, kDockWidth,
                 full.height - kTopBarHeight - kBottomBarHeight};
  chrome.right = {full.x + full.width - kDockWidth, full.y + kTopBarHeight, kDockWidth,
                  full.height - kTopBarHeight - kBottomBarHeight};
  const ThemeDef& theme = themes()[effective_theme()];
  chrome.background_color = theme.bg;
  chrome.text_color = theme.fg;
  chrome.accent_color = theme.accent;

  chrome.top_left = "mepwm";
  // Workspace cells: occupied workspaces plus the current one, dwm-style.
  for (std::size_t index = 0; index < kWorkspaceCount; ++index) {
    if (windows_in(index).empty() && index != current_workspace_) continue;
    chrome.top_cells.push_back({std::to_string(index + 1), index == current_workspace_,
                                "ws:" + std::to_string(index + 1)});
  }
  // Task list, X11-style: one cell per window on the current workspace in
  // tree order, right after the workspace numbers; the focused one is
  // highlighted and clicking a cell focuses that window.
  const std::vector<WindowId> current_windows = windows_in(current_workspace_);
  for (WindowId id : current_windows) {
    const auto entry = windows_.find(id);
    std::string label = entry == windows_.end() ? std::string("untitled")
                        : entry->second.title.empty() ? entry->second.application
                                                      : entry->second.title;
    if (label.empty()) label = "untitled";
    if (label.size() > 24) { label.resize(23); label += "…"; }
    chrome.top_cells.push_back({std::move(label), id == focused_, "win:" + std::to_string(id)});
  }
  // Active project (X11 shows it right after the launcher cell); the focused
  // window no longer needs a separate title here -- it's the highlighted
  // task cell.
  if (!projects_.empty()) {
    chrome.top_center = "[" + project_label(active_project_path()) + "]";
  }
  chrome.top_right = icons_ ? std::string(icons::kClock) + " " + clock_text() : clock_text();

  // Keybindings moved to the help sidebar; the info widget (bottom right)
  // opens it.
  chrome.bottom_text.clear();

  // OS widget anchoring the bottom bar's left corner (X11 keeps its distro
  // icon there). Decorative like X11's: empty id, so not clickable. A
  // colorless bottom-left cell renders as plain text, not a pill.
#if defined(__APPLE__)
  chrome.bottom_left_widgets.push_back({icons_ ? icons::kApple : "macOS", false, "", ""});
#elif defined(_WIN32)
  chrome.bottom_left_widgets.push_back({icons_ ? icons::kWindows : "windows", false, "", ""});
#else
  chrome.bottom_left_widgets.push_back({icons_ ? icons::kLinux : "linux", false, "", ""});
#endif

  // Active-TODO pill (X11 parity): after the OS icon, red when
  // nothing is clocked in, green with the task text and a running timer
  // while one is. Clicking it opens the todo sidebar.
  load_todos();
  const auto active_todo = std::find_if(todos_.begin(), todos_.end(),
                                        [](const TodoItem& todo) { return todo.active; });
  const bool clocked_in = active_todo != todos_.end();
  std::string pill_text = icons_ ? std::string(icons::kTodo) + " " : "";
  if (clocked_in) {
    pill_text += active_todo->text.size() > 36 ? active_todo->text.substr(0, 35) + "…"
                                               : active_todo->text;
    pill_text += "  (" + format_elapsed(std::time(nullptr) - active_todo->clock_start) + ")";
  } else {
    pill_text += "No active TODO";
  }
  chrome.bottom_left_widgets.push_back(
      {pill_text, false, "todo-active", clocked_in ? "#2ecc71" : "#e74c3c"});

  // Left dock: the application launcher, X11-style -- one cell per app in
  // kLauncherApps; clicking spawns it.
  for (std::size_t index = 0; index < kLauncherAppCount; ++index) {
    chrome.left_cells.push_back({icons_ ? kLauncherApps[index].icon : kLauncherApps[index].label,
                                 false, "launch:" + std::to_string(index)});
  }
  // Right dock: sidebar toggles, X11-style -- notifications, todos, agents.
  const bool agent_attention = std::any_of(agents_.begin(), agents_.end(),
                                           [](const AgentInfo& agent) { return agent.needs_input; });
  chrome.right_cells.push_back({icons_ ? icons::kBell : "ntf",
                                side_panel_ == PanelKind::Notifications, "notifications"});
  chrome.right_cells.push_back(
      {icons_ ? icons::kTodo : "todo", side_panel_ == PanelKind::Todo, "todo"});
  chrome.right_cells.push_back({icons_ ? icons::kAgents : "ai",
                                side_panel_ == PanelKind::Agents || agent_attention, "agents"});

  // Bottom-bar system widgets, matching the X11 backend's set where macOS
  // has an equivalent probe. Widgets with an id react to clicks
  // (handle_chrome_click).
  chrome.bottom_widgets.push_back(
      {pomodoro_text(), pomodoro_ == PomodoroPhase::Break, "pomodoro"});
  // With an icon-capable font, widgets lead with the same Nerd Font glyphs
  // the X11 bar uses; otherwise short text prefixes.
  const auto prefixed = [this](const char* icon, const char* text_prefix,
                               const std::string& value) {
    return (icons_ ? std::string(icon) : std::string(text_prefix)) +
           (value.empty() ? "" : " " + value);
  };
  if (!status_.media_title.empty()) {
    std::string media = status_.media_title;
    if (media.size() > 28) media = media.substr(0, 27) + "…";
    chrome.bottom_widgets.push_back(
        {icons_ ? std::string(icons::kMedia) + " " + media : media, false, "media"});
  }
  if (!status_.git_branch.empty()) {
    std::string git = status_.git_branch;
    if (status_.git_dirty > 0) git += " *" + std::to_string(status_.git_dirty);
    chrome.bottom_widgets.push_back({prefixed(icons::kGit, "git", git), false, "git"});
  }
  if (!status_.keyboard_layout.empty()) {
    chrome.bottom_widgets.push_back(
        {icons_ ? std::string(icons::kKeyboard) + " " + status_.keyboard_layout
                : status_.keyboard_layout,
         false, "keyboard"});
  }
  if (!status_.appearance.empty()) {
    chrome.bottom_widgets.push_back(
        {icons_ ? std::string(status_.appearance == "dark" ? icons::kThemeDark
                                                           : icons::kThemeLight)
                : status_.appearance,
         false, "theme"});
  }
  if (status_.battery_percent >= 0) {
    std::string battery;
    if (icons_) {
      battery = battery_icon(status_.battery_percent);
      if (status_.battery_charging) battery += std::string(" ") + icons::kBatteryCharging;
      battery += " " + std::to_string(status_.battery_percent) + "%";
    } else {
      battery = "bat " + std::to_string(status_.battery_percent) + "%" +
                (status_.battery_charging ? "+" : "");
    }
    chrome.bottom_widgets.push_back(
        {battery, status_.battery_percent <= 15 && !status_.battery_charging, "battery"});
  }
  if (status_.volume_percent >= 0) {
    const char* volume_icon = status_.volume_muted || status_.volume_percent == 0
                                  ? icons::kVolumeMuted
                                  : status_.volume_percent < 50 ? icons::kVolumeLow
                                                                : icons::kVolumeHigh;
    const std::string level =
        status_.volume_muted ? "mut" : std::to_string(status_.volume_percent) + "%";
    chrome.bottom_widgets.push_back({prefixed(volume_icon, "vol", level), false, "volume"});
  }
  if (status_.input_volume_percent >= 0) {
    const char* mic_icon =
        status_.input_volume_percent == 0 ? icons::kMicrophoneMuted : icons::kMicrophone;
    chrome.bottom_widgets.push_back(
        {prefixed(mic_icon, "mic", std::to_string(status_.input_volume_percent) + "%"), false,
         "mic"});
  }
  chrome.bottom_widgets.push_back(
      {prefixed(icons::kWifi, "wifi",
                status_.wifi_on ? (status_.wifi_ssid.empty() ? std::string("on") : status_.wifi_ssid)
                                : std::string("off")),
       false, "wifi"});
  chrome.bottom_widgets.push_back(
      {prefixed(icons::kBluetooth, "bt",
                status_.bluetooth_on ? (status_.bluetooth_device.empty() ? std::string("on")
                                                                         : status_.bluetooth_device)
                                     : std::string("off")),
       false, "bluetooth"});
  // CPU widget shows usage percent like the X11 bar ("..." until the probe
  // has the two samples a delta needs); load average stays in the System
  // panel.
  chrome.bottom_widgets.push_back(
      {prefixed(icons::kCpu, "cpu",
                status_.cpu_used_percent >= 0 ? std::to_string(status_.cpu_used_percent) + "%"
                                              : std::string("...")),
       false, "system"});
  if (status_.memory_used_percent >= 0) {
    chrome.bottom_widgets.push_back(
        {prefixed(icons::kMemory, "mem", std::to_string(status_.memory_used_percent) + "%"),
         false, "system"});
  }
  if (status_.disk_used_percent >= 0) {
    chrome.bottom_widgets.push_back(
        {prefixed(icons::kDisk, "disk", std::to_string(status_.disk_used_percent) + "%"), false,
         "system"});
  }
  // Rightmost: the info widget opening the keybinding help (X11 keeps its
  // info icon in the bottom bar's right-hand corner too).
  chrome.bottom_widgets.push_back(
      {icons_ ? icons::kInfo : "?", side_panel_ == PanelKind::Help, "help"});

  platform_->update_chrome(chrome);
}

void TilingEngine::handle_chrome_click(const Event& event) {
  if (event.area == ChromeArea::TopBar) {
    if (event.cell.rfind("ws:", 0) == 0) {
      switch_workspace(static_cast<std::size_t>(std::atoi(event.cell.c_str() + 3) - 1));
    } else if (event.cell.rfind("win:", 0) == 0) {
      activate_window(static_cast<WindowId>(std::strtoull(event.cell.c_str() + 4, nullptr, 10)));
    }
    return;
  }
  if (event.area == ChromeArea::LeftDock) {
    // Cell ids are "launch:<index>" into kLauncherApps; the terminal entry
    // (empty command) follows the configured terminal.
    if (event.cell.rfind("launch:", 0) == 0) {
      const std::size_t index = std::strtoull(event.cell.c_str() + 7, nullptr, 10);
      if (index < kLauncherAppCount) {
        const std::string command = kLauncherApps[index].command;
        platform_->spawn(command.empty() ? config_.terminal : command);
      }
    }
    return;
  }
  if (event.area == ChromeArea::TabBar) {
    activate_window(static_cast<WindowId>(std::strtoull(event.cell.c_str(), nullptr, 10)));
    return;
  }
  if (event.area == ChromeArea::SidePanel) {
    handle_panel_click(event.cell);
    return;
  }
  if (event.area == ChromeArea::RightDock) {
    if (event.cell == "notifications") toggle_side_panel(PanelKind::Notifications);
    if (event.cell == "todo") toggle_side_panel(PanelKind::Todo);
    if (event.cell == "agents") toggle_side_panel(PanelKind::Agents);
    return;
  }
  if (event.area != ChromeArea::BottomBar) return;
  if (event.cell == "pomodoro") {
    toggle_pomodoro();
    refresh_chrome();
    return;
  }
  if (event.cell == "todo-active") {
    toggle_side_panel(PanelKind::Todo);
    return;
  }
  static const std::vector<std::pair<std::string, PanelKind>> kWidgetPanels = {
      {"wifi", PanelKind::Wifi},       {"bluetooth", PanelKind::Bluetooth},
      {"media", PanelKind::Media},     {"battery", PanelKind::Battery},
      {"volume", PanelKind::Volume},   {"mic", PanelKind::Mic},
      {"git", PanelKind::Git},         {"keyboard", PanelKind::Keyboard},
      {"theme", PanelKind::Theme},     {"system", PanelKind::System},
      {"help", PanelKind::Help},
  };
  for (const auto& [id, panel] : kWidgetPanels) {
    if (event.cell == id) {
      toggle_side_panel(panel);
      return;
    }
  }
}

void TilingEngine::run_panel_action(SystemAction action, int value, const std::string& argument) {
  platform_->perform(action, value, argument);
  // perform() is synchronous, so resampling reflects the change at once.
  status_ = platform_->system_status();
  refresh_side_panel();
  refresh_chrome();
}

void TilingEngine::handle_panel_click(const std::string& cell) {
  if (cell.empty()) return;
  switch (side_panel_) {
    case PanelKind::Volume:
      if (cell == "mute") {
        run_panel_action(SystemAction::SetOutputMuted, status_.volume_muted ? 0 : 1);
      } else if (cell.rfind("vol:", 0) == 0) {
        run_panel_action(SystemAction::SetOutputVolume, std::atoi(cell.c_str() + 4));
      }
      break;
    case PanelKind::Mic:
      if (cell.rfind("mic:", 0) == 0) {
        run_panel_action(SystemAction::SetInputVolume, std::atoi(cell.c_str() + 4));
      }
      break;
    case PanelKind::Theme:
      if (cell == "toggle") run_panel_action(SystemAction::ToggleAppearance);
      if (cell.rfind("theme:", 0) == 0) {
        apply_theme(static_cast<std::size_t>(std::atoi(cell.c_str() + 6)));
      }
      break;
    case PanelKind::Keyboard:
      if (cell.rfind("kb:", 0) == 0) {
        run_panel_action(SystemAction::SelectKeyboardLayout, 0, cell.substr(3));
      }
      break;
    case PanelKind::Media:
      if (cell == "media:prev") run_panel_action(SystemAction::MediaPrevious);
      if (cell == "media:playpause") run_panel_action(SystemAction::MediaPlayPause);
      if (cell == "media:next") run_panel_action(SystemAction::MediaNext);
      break;
    case PanelKind::Todo:
      if (cell.rfind("todo:", 0) == 0) {
        todo_selected_ = std::atoi(cell.c_str() + 5);
        refresh_side_panel();
      }
      break;
    default:
      break;
  }
}

// Keyboard policy for the open side panel, mirroring the X11 backend's
// handle_todo_key/handle_side_panel_key: Esc closes, ? toggles the help
// overlay, and the todo panel adds navigation and org-file actions.
void TilingEngine::handle_panel_key(const Event& event) {
  if (side_panel_ == PanelKind::None || side_panel_ == PanelKind::Help) return;
  const bool in_todo = side_panel_ == PanelKind::Todo;
  if (in_todo && todo_input_active_) {
    // Inline add-mode: the panel behaves like a one-line text field.
    if (event.panel_key == PanelKey::Escape) {
      todo_input_active_ = false;
      todo_input_text_.clear();
      refresh_side_panel();
    } else if (event.panel_key == PanelKey::Return) {
      add_todo(todo_input_text_);
      todo_input_active_ = false;
      todo_input_text_.clear();
      load_todos();
      todo_selected_ = todos_.empty() ? -1 : static_cast<int>(todos_.size()) - 1;
      refresh_side_panel();
    } else if (event.panel_key == PanelKey::Backspace) {
      if (!todo_input_text_.empty()) todo_input_text_.pop_back();
      refresh_side_panel();
    } else if (event.panel_key == PanelKey::Character && !event.ctrl) {
      for (const char typed : event.text) {
        if (std::isprint(static_cast<unsigned char>(typed)) && todo_input_text_.size() < 255) {
          todo_input_text_ += typed;
        }
      }
      refresh_side_panel();
    }
    return;
  }
  if (event.panel_key == PanelKey::Escape) {
    toggle_side_panel(side_panel_);  // toggling the open panel closes it
    return;
  }
  if (event.panel_key == PanelKey::Character && event.text == "?") {
    panel_help_visible_ = !panel_help_visible_;
    refresh_side_panel();
    return;
  }
  if (panel_help_visible_ || !in_todo) return;
  const bool ctrl_char = event.panel_key == PanelKey::Character && event.ctrl;
  if (event.panel_key == PanelKey::Up || (ctrl_char && event.text == "p")) {
    move_todo_selection(-1);
    refresh_side_panel();
    return;
  }
  if (event.panel_key == PanelKey::Down || (ctrl_char && event.text == "n")) {
    move_todo_selection(1);
    refresh_side_panel();
    return;
  }
  if (event.panel_key != PanelKey::Character || event.ctrl) return;
  if (event.text == "a") {
    todo_input_active_ = true;
    todo_input_text_.clear();
    refresh_side_panel();
  } else if (event.text == "d" && todo_selected_ >= 0 &&
             static_cast<std::size_t>(todo_selected_) < todos_.size()) {
    mark_todo_done(todos_[static_cast<std::size_t>(todo_selected_)]);
    load_todos();
    refresh_side_panel();
  } else if (event.text == "s") {
    toggle_todo_active();
    refresh_side_panel();
  } else if (event.text == "e") {
    edit_todo_file();
  }
}

std::string TilingEngine::todo_path() const { return todo_file_path(active_project_path()); }

// Parses org-mode headlines ("* TODO Buy milk"); only the plain TODO keyword
// is a pending item, so DONE never shows up. A headline is "active" when its
// body has an open CLOCK line, mirroring org-clock-in/out.
void TilingEngine::load_todos() {
  todos_.clear();
  const std::vector<std::string> lines = read_file_lines(todo_path());
  for (std::size_t number = 0; number < lines.size(); ++number) {
    std::size_t keyword_start = 0;
    std::size_t text_start = 0;
    if (!parse_org_headline(lines[number], "TODO", &keyword_start, &text_start)) continue;
    const auto clock_line = find_open_clock_line(lines, number);
    const std::time_t clock_start =
        clock_line ? parse_clock_start(lines[*clock_line]).value_or(0) : 0;
    todos_.push_back({lines[number].substr(text_start), number, clock_line.has_value(), clock_start});
  }
  if (todo_selected_ >= static_cast<int>(todos_.size())) {
    todo_selected_ = todos_.empty() ? -1 : static_cast<int>(todos_.size()) - 1;
  }
}

// Rewrites a single line in place, flipping its TODO keyword to DONE.
// Re-checks the line still looks like the expected TODO headline first, in
// case the file changed underneath us since it was last parsed.
void TilingEngine::mark_todo_done(const TodoItem& item) {
  const std::string path = todo_path();
  if (path.empty()) return;
  std::vector<std::string> lines = read_file_lines(path);
  if (item.line >= lines.size()) return;
  std::size_t keyword_start = 0;
  std::size_t text_start = 0;
  if (!parse_org_headline(lines[item.line], "TODO", &keyword_start, &text_start)) return;
  lines[item.line].replace(keyword_start, std::string("TODO").size(), "DONE");
  write_file_lines(path, lines);
}

// Appends a new TODO headline, creating the file if it doesn't exist yet.
void TilingEngine::add_todo(const std::string& text) {
  const std::string path = todo_path();
  if (path.empty() || text.empty()) return;
  std::ofstream file(path, std::ios::app);
  if (file) file << "* TODO " << text << '\n';
}

// Opens a fresh clock for `item`, reusing an existing :LOGBOOK: drawer right
// under the headline if there is one, otherwise creating it -- matching
// org-clock-in's on-disk format so the file stays readable in Emacs.
void TilingEngine::start_todo_clock(const TodoItem& item) {
  const std::string path = todo_path();
  if (path.empty()) return;
  std::vector<std::string> lines = read_file_lines(path);
  if (item.line >= lines.size()) return;
  std::size_t keyword_start = 0;
  std::size_t text_start = 0;
  if (!parse_org_headline(lines[item.line], "TODO", &keyword_start, &text_start)) return;
  const std::string clock_line = "CLOCK: " + org_timestamp(std::time(nullptr));
  if (item.line + 1 < lines.size() && trim(lines[item.line + 1]) == ":LOGBOOK:") {
    lines.insert(lines.begin() + static_cast<long>(item.line) + 2, clock_line);
  } else {
    lines.insert(lines.begin() + static_cast<long>(item.line) + 1,
                 {":LOGBOOK:", clock_line, ":END:"});
  }
  write_file_lines(path, lines);
}

// Closes item's open CLOCK line with an end timestamp and duration, matching
// org-clock-out's format. No-op if the item has no open clock.
void TilingEngine::stop_todo_clock(const TodoItem& item) {
  const std::string path = todo_path();
  if (path.empty()) return;
  std::vector<std::string> lines = read_file_lines(path);
  if (item.line >= lines.size()) return;
  const auto clock_line = find_open_clock_line(lines, item.line);
  if (!clock_line) return;
  const std::size_t open_bracket = lines[*clock_line].find('[');
  const std::size_t close_bracket =
      open_bracket == std::string::npos ? std::string::npos
                                        : lines[*clock_line].find(']', open_bracket);
  if (open_bracket == std::string::npos || close_bracket == std::string::npos) return;
  const std::string start_text =
      lines[*clock_line].substr(open_bracket + 1, close_bracket - open_bracket - 1);
  const std::time_t start = parse_org_timestamp(start_text);
  const std::time_t end = std::time(nullptr);
  const long minutes = std::max<long>(0, (end - start) / 60);
  char duration[32];
  std::snprintf(duration, sizeof duration, "%ld:%02ld", minutes / 60, minutes % 60);
  lines[*clock_line] =
      "CLOCK: [" + start_text + "]--" + org_timestamp(end) + " =>  " + duration;
  write_file_lines(path, lines);
}

// Toggles the clock on the selected todo. Only one todo can be active at a
// time, so starting a new one first stops whichever was running -- mirroring
// how org-clock-in auto-clocks-out any other running clock.
void TilingEngine::toggle_todo_active() {
  if (todo_selected_ < 0 || static_cast<std::size_t>(todo_selected_) >= todos_.size()) return;
  const TodoItem selected = todos_[static_cast<std::size_t>(todo_selected_)];
  if (selected.active) {
    stop_todo_clock(selected);
  } else {
    const auto active = std::find_if(todos_.begin(), todos_.end(),
                                     [](const TodoItem& todo) { return todo.active; });
    if (active != todos_.end()) stop_todo_clock(*active);
    start_todo_clock(selected);
  }
  load_todos();
}

void TilingEngine::move_todo_selection(int delta) {
  if (todos_.empty()) {
    todo_selected_ = -1;
    return;
  }
  const int count = static_cast<int>(todos_.size());
  todo_selected_ = todo_selected_ < 0 ? 0 : (todo_selected_ + delta + count) % count;
}

void TilingEngine::edit_todo_file() {
  const std::string path = todo_path();
  if (path.empty()) return;
  const std::string quoted = shell_quote(path);
  // `open -t` hands the file to the system's default plain-text editor;
  // touch first so a missing TODO.org opens as a new empty file.
  platform_->spawn("touch " + quoted + " && open -t " + quoted);
}

void TilingEngine::toggle_pomodoro() {
  if (pomodoro_ == PomodoroPhase::Idle) {
    pomodoro_ = PomodoroPhase::Work;
    pomodoro_end_ = std::time(nullptr) + 25 * 60;
  } else {
    pomodoro_ = PomodoroPhase::Idle;
    pomodoro_end_ = 0;
  }
}

void TilingEngine::advance_pomodoro() {
  if (pomodoro_ == PomodoroPhase::Idle) return;
  const std::time_t now = std::time(nullptr);
  if (now < pomodoro_end_) return;
  // Work and break phases alternate until toggled off.
  if (pomodoro_ == PomodoroPhase::Work) {
    pomodoro_ = PomodoroPhase::Break;
    pomodoro_end_ = now + 5 * 60;
    notify("pomodoro: break time");
  } else {
    pomodoro_ = PomodoroPhase::Work;
    pomodoro_end_ = now + 25 * 60;
    notify("pomodoro: back to work");
  }
}

std::string TilingEngine::pomodoro_text() const {
  const std::string prefix =
      icons_ ? icons::kPomodoro : (pomodoro_ == PomodoroPhase::Break ? "break" : "pom");
  if (pomodoro_ == PomodoroPhase::Idle) return prefix;
  const std::time_t remaining = std::max<std::time_t>(0, pomodoro_end_ - std::time(nullptr));
  char timer[32];
  std::snprintf(timer, sizeof timer, " %02ld:%02ld", static_cast<long>(remaining / 60),
                static_cast<long>(remaining % 60));
  return prefix + timer;
}

void TilingEngine::toggle_side_panel(PanelKind panel) {
  side_panel_ = side_panel_ == panel ? PanelKind::None : panel;
  panel_help_visible_ = false;
  todo_input_active_ = false;
  todo_input_text_.clear();
  if (side_panel_ == PanelKind::Todo) {
    load_todos();
    todo_selected_ = todos_.empty() ? -1 : 0;
    todo_scroll_ = 0;
  }
  refresh_side_panel();
  refresh_chrome();  // dock/widget highlights track the open panel
}

void TilingEngine::refresh_side_panel() {
  SidePanel panel;
  panel.visible = side_panel_ != PanelKind::None;
  if (panel.visible) {
    const Rect full = platform_->work_area();
    const int width = std::min(360, full.width / 3);
    panel.frame = {full.x + full.width - kDockWidth - width - 8, full.y + kTopBarHeight + 8, width,
                   full.height - kTopBarHeight - kBottomBarHeight - 16};
    const std::string& mod = config_.modifier;
    if (side_panel_ == PanelKind::Help) {
      panel.title = "Keybindings";
      panel.lines = {
          {mod + " + h/j/k/l          focus pane by direction", false},
          {mod + " + shift + h/j/k/l  resize split toward direction", false},
          {mod + " + ctrl + h/j/k/l   move window to pane", false},
          {mod + " + v                split pane (side by side)", false},
          {mod + " + s                split pane (stacked)", false},
          {mod + " + p                application picker", false},
          {mod + " + i                projects picker (type a path to add)", false},
          {mod + " + o                active projects picker", false},
          {mod + " + n                next tab", false},
          {mod + " + tab / +shift+tab  next / previous tab", false},
          {mod + " + m / " + mod + " + d      merge pane with sibling", false},
          {mod + " + minus/equal      resize the selected split", false},
          {mod + " + r                retile", false},
          {mod + " + enter            open terminal", false},
          {mod + " + 1..9             switch workspace", false},
          {mod + " + shift + 1..9     send window to workspace", false},
          {mod + " + t                toggle todo sidebar", false},
          {mod + " + f                click hints", false},
          {mod + " + b                toggle bars", false},
          {mod + " + shift + t        theme picker", false},
          {mod + " + shift + w        wallpaper picker", false},
          {mod + " + shift + /        toggle this help", false},
          {mod + " + shift + q        quit mepwm", false},
          {"?                  in an open panel: its keybindings", false},
          {"", false},
          {"Click bottom-bar widgets: pom toggles the", false},
          {"pomodoro; wifi/bt/media open their panels.", false},
          {"Click a top-bar task cell to focus that window;", false},
          {"left-dock cells launch applications.", false},
          {"", false},
          {"Config: MEPWM_TERMINAL, MEPWM_MODIFIER,", false},
          {"        MEPWM_TODO_FILE, MEPWM_DEBUG", false},
      };
    } else if (side_panel_ == PanelKind::Todo) {
      const std::string project = active_project_path();
      panel.title = project.empty() ? "TODO" : "TODO - " + project_label(project);
      // Reload on every refresh so external edits and running clock times
      // stay current (the tick refresh keeps elapsed times moving).
      load_todos();
      if (todo_input_active_) panel.lines.push_back({"+ " + todo_input_text_ + "_", true});
      if (todos_.empty() && !todo_input_active_) {
        const std::string path = todo_path();
        if (std::ifstream(path).good()) {
          panel.lines.push_back({"No pending TODO items", false});
        } else {
          panel.lines.push_back({"No TODO.org found (" + path + ")", false});
        }
        panel.lines.push_back({"Press a to add one", false});
      }
      // Keep the selection inside the visible window: rows are fixed-height
      // labels on the platform side, so scrolling is index arithmetic here.
      const int capacity =
          std::max(1, (panel.frame.height - kPanelChromeHeight) / kPanelLineHeight -
                          (todo_input_active_ ? 1 : 0));
      if (todo_selected_ >= 0) {
        todo_scroll_ = std::min(todo_scroll_, todo_selected_);
        todo_scroll_ = std::max(todo_scroll_, todo_selected_ - capacity + 1);
      }
      todo_scroll_ =
          std::clamp(todo_scroll_, 0, std::max(0, static_cast<int>(todos_.size()) - capacity));
      const auto todo_row_text = [](const TodoItem& todo) {
        if (!todo.active) return "[ ] " + todo.text;
        return "[*] " + todo.text + "  (" +
               format_elapsed(std::time(nullptr) - todo.clock_start) + ")";
      };
      const int last =
          std::min(static_cast<int>(todos_.size()), todo_scroll_ + capacity);
      for (int index = todo_scroll_; index < last; ++index) {
        panel.lines.push_back({todo_row_text(todos_[static_cast<std::size_t>(index)]),
                               !todo_input_active_ && index == todo_selected_,
                               "todo:" + std::to_string(index)});
      }
    } else if (side_panel_ == PanelKind::Notifications) {
      panel.title = "Notifications";
      if (notifications_.empty()) {
        panel.lines.push_back({"No notifications yet", false});
      } else {
        for (const std::string& entry : notifications_) panel.lines.push_back({entry, false});
      }
    } else if (side_panel_ == PanelKind::Agents) {
      panel.title = "Agents";
      if (agents_.empty()) {
        panel.lines.push_back({"No agents running", false});
        panel.lines.push_back({"", false});
        panel.lines.push_back({"Watches for claude/codex processes and", false});
        panel.lines.push_back({"status files in $XDG_RUNTIME_DIR/mwm-agents", false});
        panel.lines.push_back({"(or /tmp/mwm-agents-$USER).", false});
      } else {
        for (const AgentInfo& agent : agents_) {
          std::string where = !agent.label.empty() ? agent.label : agent.cwd;
          if (where.size() > 34) where = "…" + where.substr(where.size() - 33);
          panel.lines.push_back(
              {agent.kind + "  " + agent.status + (where.empty() ? "" : "  " + where),
               agent.needs_input});
        }
      }
    } else if (side_panel_ == PanelKind::Wifi) {
      panel.title = "Wi-Fi";
      panel.lines.push_back({std::string("Power: ") + (status_.wifi_on ? "on" : "off"), false});
      panel.lines.push_back(
          {"Network: " + (status_.wifi_ssid.empty() ? std::string("(unknown)") : status_.wifi_ssid),
           false});
      if (status_.wifi_on && status_.wifi_ssid.empty()) {
        panel.lines.push_back({"", false});
        panel.lines.push_back({"macOS hides the SSID until mepwm's", false});
        panel.lines.push_back({"launcher has the Location permission.", false});
      }
    } else if (side_panel_ == PanelKind::Bluetooth) {
      panel.title = "Bluetooth";
      panel.lines.push_back({std::string("Power: ") + (status_.bluetooth_on ? "on" : "off"), false});
      panel.lines.push_back({"", false});
      if (status_.bluetooth_devices.empty()) {
        panel.lines.push_back({"No paired devices", false});
      } else {
        for (const BarCell& device : status_.bluetooth_devices) {
          panel.lines.push_back({(device.highlight ? "* " : "  ") + device.text +
                                     (device.highlight ? "  (connected)" : ""),
                                 device.highlight});
        }
      }
    } else if (side_panel_ == PanelKind::Media) {
      panel.title = "Media";
      panel.lines.push_back(
          {status_.media_title.empty() ? std::string("No active player") : status_.media_title,
           false});
      if (!status_.media_title.empty()) {
        panel.lines.push_back({"", false});
        panel.lines.push_back({"> previous", false, "media:prev"});
        panel.lines.push_back({"> play / pause", false, "media:playpause"});
        panel.lines.push_back({"> next", false, "media:next"});
      }
    } else if (side_panel_ == PanelKind::Battery) {
      panel.title = "Power";
      if (status_.battery_percent < 0) {
        panel.lines.push_back({"No battery", false});
      } else {
        panel.lines.push_back({"Battery: " + std::to_string(status_.battery_percent) + "%", false});
        panel.lines.push_back(
            {std::string("State: ") + (status_.battery_charging ? "charging" : "discharging"),
             false});
        if (status_.battery_minutes_remaining > 0) {
          char remaining[48];
          std::snprintf(remaining, sizeof remaining, "%s: %d:%02d",
                        status_.battery_charging ? "Time to full" : "Time remaining",
                        status_.battery_minutes_remaining / 60,
                        status_.battery_minutes_remaining % 60);
          panel.lines.push_back({remaining, false});
        }
      }
    } else if (side_panel_ == PanelKind::Volume) {
      panel.title = "Volume";
      panel.lines.push_back({"Output: " + std::to_string(status_.volume_percent) + "%" +
                                 (status_.volume_muted ? " (muted)" : ""),
                             false});
      panel.lines.push_back({"", false});
      panel.lines.push_back(
          {std::string("> ") + (status_.volume_muted ? "unmute" : "mute"), false, "mute"});
      for (int level : {0, 25, 50, 75, 100}) {
        panel.lines.push_back({"> set " + std::to_string(level) + "%",
                               !status_.volume_muted && status_.volume_percent == level,
                               "vol:" + std::to_string(level)});
      }
    } else if (side_panel_ == PanelKind::Mic) {
      panel.title = "Microphone";
      panel.lines.push_back(
          {"Input: " + std::to_string(status_.input_volume_percent) + "%", false});
      panel.lines.push_back({"", false});
      for (int level : {0, 25, 50, 75, 100}) {
        panel.lines.push_back({"> set " + std::to_string(level) + "%",
                               status_.input_volume_percent == level,
                               "mic:" + std::to_string(level)});
      }
    } else if (side_panel_ == PanelKind::Git) {
      panel.title = "Git";
      if (status_.git_branch.empty()) {
        panel.lines.push_back({"Launch directory is not a git repo", false});
      } else {
        panel.lines.push_back({"Branch: " + status_.git_branch, false});
        panel.lines.push_back(
            {"Changes: " + std::to_string(std::max(0, status_.git_dirty)), false});
        if (!status_.git_status_lines.empty()) panel.lines.push_back({"", false});
        for (const std::string& line : status_.git_status_lines) {
          panel.lines.push_back({line, false});
        }
      }
    } else if (side_panel_ == PanelKind::Keyboard) {
      panel.title = "Keyboard";
      panel.lines.push_back({"Layout: " + status_.keyboard_layout, false});
      panel.lines.push_back({"", false});
      for (const BarCell& layout : status_.keyboard_layouts) {
        panel.lines.push_back({"> " + layout.text, layout.highlight, "kb:" + layout.text});
      }
    } else if (side_panel_ == PanelKind::Theme) {
      panel.title = "Theme";
      panel.lines.push_back({"System appearance: " + status_.appearance, false});
      panel.lines.push_back({"> toggle dark / light", false, "toggle"});
      panel.lines.push_back({"  (first use prompts for System Events", false});
      panel.lines.push_back({"   automation permission)", false});
      panel.lines.push_back({"", false});
      panel.lines.push_back({"mepwm theme (" + mod + "+shift+t):", false});
      const auto& list = themes();
      for (std::size_t index = 0; index < list.size(); ++index) {
        panel.lines.push_back({"> " + std::string(list[index].name), index == effective_theme(),
                               "theme:" + std::to_string(index)});
      }
    } else {
      panel.title = "System";
      panel.lines.push_back({"CPU used: " + (status_.cpu_used_percent >= 0
                                                 ? std::to_string(status_.cpu_used_percent) + "%"
                                                 : std::string("collecting samples")),
                             false});
      char load[32];
      std::snprintf(load, sizeof load, "Load average: %.2f", status_.load_average);
      panel.lines.push_back({load, false});
      panel.lines.push_back(
          {"Memory used: " + std::to_string(status_.memory_used_percent) + "%", false});
      panel.lines.push_back(
          {"Disk used: " + std::to_string(status_.disk_used_percent) + "%", false});
      panel.lines.push_back(
          {"Managed windows: " + std::to_string(windows_.size()), false});
      panel.lines.push_back({"", false});
      panel.lines.push_back({"Gap: " + std::to_string(layout_.gap), false});
      panel.lines.push_back(
          {"Split ratio step: 5% (" + config_.modifier + "+-/=)", false});
    }
    // Actionable panels (everything but Help, which is already nothing but
    // keybindings) take the keyboard while open: Esc closes, ? swaps in a
    // contextual keybinding overlay, and a footer row below a separator
    // advertises it (X11 parity).
    if (side_panel_ != PanelKind::Help) {
      panel.wants_keys = true;
      panel.footer = panel_help_visible_ ? "?  Hide help" : "?  Toggle help";
      if (panel_help_visible_) panel.lines = panel_help_lines();
    }
  }
  platform_->update_side_panel(panel);
}

// Contextual keybindings for whichever panel is open, shown in place of the
// panel's normal content while the '?' overlay is toggled on.
std::vector<BarCell> TilingEngine::panel_help_lines() const {
  std::vector<BarCell> lines;
  const auto add = [&lines](const char* text) { lines.push_back({text, false}); };
  switch (side_panel_) {
    case PanelKind::Todo:
      add("a          add a todo");
      add("d          mark the selected item done");
      add("s          start/stop clocking it");
      add("e          edit TODO.org");
      add("^n / ^p    move selection (or arrows)");
      add("esc        close");
      break;
    case PanelKind::Volume:
    case PanelKind::Mic:
    case PanelKind::Theme:
    case PanelKind::Keyboard:
    case PanelKind::Media:
      add("Click a row to act on it");
      add("esc        close");
      break;
    default:
      add("esc        close");
      break;
  }
  return lines;
}

void TilingEngine::refresh_borders() {
  std::vector<Border> borders;
  for (const VisiblePane& pane : compute_visible_panes()) {
    Border border;
    border.frame = pane.content;
    border.width = config_.border_width;
    // The selected empty pane gets the focus color too: it is where the
    // next window will open (X11's empty-pane highlight).
    const bool focused_pane = (focused_ != kNoWindow && leaf_of(pane.leaf, focused_) != nullptr) ||
                              (pane.leaf->tabs.empty() && pane.leaf == ws().selected);
    border.color = focused_pane ? config_.border_color_focused : config_.border_color_normal;
    borders.push_back(std::move(border));
  }
  platform_->update_borders(borders);
}

void TilingEngine::focus_direction(Direction direction) {
  const std::vector<VisiblePane> panes = compute_visible_panes();
  if (panes.empty()) return;
  std::vector<Rect> frames;
  std::size_t from = panes.size();
  for (std::size_t index = 0; index < panes.size(); ++index) {
    frames.push_back(panes[index].content);
    if (panes[index].leaf == leaf_of(ws().root.get(), focused_)) from = index;
  }
  if (from >= panes.size()) {
    // Nothing focused: land on the selected pane's window if any.
    if (PaneNode* leaf = current_leaf(); leaf != nullptr && leaf->active_window() != kNoWindow) {
      activate_window(leaf->active_window());
    }
    return;
  }
  if (const auto target = pick_in_direction(frames, from, direction)) {
    PaneNode* leaf = panes[*target].leaf;
    ws().selected = leaf;
    if (leaf->active_window() != kNoWindow) {
      focused_ = leaf->active_window();
      platform_->focus_window(focused_);
    }
    refresh_borders();
    refresh_tab_bars();
    refresh_chrome();
  }
}

void TilingEngine::move_direction(Direction direction) {
  if (focused_ == kNoWindow) return;
  const std::vector<VisiblePane> panes = compute_visible_panes();
  std::vector<Rect> frames;
  std::size_t from = panes.size();
  for (std::size_t index = 0; index < panes.size(); ++index) {
    frames.push_back(panes[index].content);
    if (panes[index].leaf == leaf_of(ws().root.get(), focused_)) from = index;
  }
  if (from >= panes.size()) return;
  const auto target = pick_in_direction(frames, from, direction);
  if (!target) return;
  PaneNode* destination = panes[*target].leaf;
  const WindowId moved = focused_;
  WorkspaceState& state = ws();
  remove_window(state.root, moved, &state.selected);
  // `destination` survives any pruning: only empty panes are destroyed.
  destination->tabs.push_back(moved);
  destination->active_tab = destination->tabs.size() - 1;
  state.selected = destination;
  retile();
  platform_->focus_window(moved);
}

void TilingEngine::split_pane(bool vertical) {
  ensure_root();
  PaneNode* leaf = current_leaf();
  if (leaf == nullptr) return;
  ws().selected = split_leaf(leaf, vertical);
  retile();
}

void TilingEngine::cycle_tab(int delta) {
  PaneNode* leaf = current_leaf();
  if (leaf == nullptr || leaf->tabs.size() < 2) return;
  const std::size_t count = leaf->tabs.size();
  leaf->active_tab = (leaf->active_tab + count + static_cast<std::size_t>(delta > 0 ? 1 : count - 1)) % count;
  focused_ = leaf->tabs[leaf->active_tab];
  platform_->focus_window(focused_);
  retile();
}

void TilingEngine::merge_pane() {
  PaneNode* leaf = current_leaf();
  if (leaf == nullptr) return;
  WorkspaceState& state = ws();
  merge_with_sibling(state.root, leaf, &state.selected);
  retile();
}

void TilingEngine::resize_direction(Direction direction) {
  PaneNode* leaf = current_leaf();
  if (leaf == nullptr) return;
  // h/l move a column boundary, j/k a row boundary: walk up to the nearest
  // ancestor split whose axis matches the requested direction.
  const bool wants_columns = direction == Direction::Left || direction == Direction::Right;
  PaneNode* parent = leaf->parent;
  while (parent != nullptr && parent->vertical_split != wants_columns) parent = parent->parent;
  if (parent == nullptr) return;
  // The key moves the shared boundary in its direction: Left/Up shrink the
  // first child's share, Right/Down grow it.
  const float delta =
      (direction == Direction::Left || direction == Direction::Up) ? -0.05F : 0.05F;
  parent->ratio = std::clamp(parent->ratio + delta, 0.1F, 0.9F);
  retile();
}

void TilingEngine::adjust_ratio(float delta) {
  PaneNode* leaf = current_leaf();
  if (leaf == nullptr || leaf->parent == nullptr) return;
  // Growing the selected pane means growing whichever side of the parent
  // split it sits on.
  PaneNode* parent = leaf->parent;
  const bool is_first = parent->first.get() == leaf;
  parent->ratio = std::clamp(parent->ratio + (is_first ? delta : -delta), 0.1F, 0.9F);
  retile();
}

}  // namespace mepwm::core
