#include "core/terminal_theme.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

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

std::string kitty_config_dir() {
  if (const char* config_home = std::getenv("XDG_CONFIG_HOME");
      config_home != nullptr && *config_home != '\0') {
    return std::string(config_home) + "/kitty";
  }
  if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
    return std::string(home) + "/.config/kitty";
  }
  return {};
}

std::string shell_single_quote(const std::string& value) {
  std::string quoted = "'";
  for (char ch : value) {
    if (ch == '\'') quoted += "'\\''";
    else quoted += ch;
  }
  quoted += "'";
  return quoted;
}

void write_kitty_theme_file(const std::string& path, const std::string& fg, const std::string& bg,
                             const std::string& accent) {
  std::ofstream file(path);
  if (!file) return;
  file << "background " << bg << '\n'
       << "foreground " << fg << '\n'
       << "cursor " << accent << '\n'
       << "cursor_text_color " << bg << '\n'
       << "url_color " << accent << '\n'
       << "selection_background " << accent << '\n'
       << "selection_foreground " << bg << '\n'
       << "active_tab_background " << accent << '\n'
       << "active_tab_foreground " << bg << '\n'
       << "inactive_tab_background " << mix_hex(bg, fg, 0.15) << '\n'
       << "tab_bar_background " << bg << '\n';
}

// kitty only picks up mepwm-theme.conf for cold-started windows if
// kitty.conf includes it; append that include once, non-destructively, so
// we never clobber the user's own kitty config.
void ensure_kitty_conf_includes_theme(const std::string& config_dir) {
  static const char kIncludeLine[] = "include mepwm-theme.conf";
  const std::string conf_path = config_dir + "/kitty.conf";
  std::ifstream existing(conf_path);
  if (existing) {
    std::string line;
    while (std::getline(existing, line)) {
      if (line.find(kIncludeLine) != std::string::npos) return;
    }
  }
  std::ofstream file(conf_path, std::ios::app);
  if (file) file << '\n' << kIncludeLine << '\n';
}

// When several kitty windows share one configured `listen_on unix:...`
// path, only the first to start actually binds it -- every later one falls
// back to the same path with "-<pid>" appended so it doesn't collide. A
// plain `kitty @` (or one pinned to the bare configured path) therefore
// only ever reaches a single instance. Turning the configured path into a
// glob (and swapping any literal "{kitty_pid}" placeholder for "*") lets
// the caller retheme every live instance instead of guessing which one.
// Returns empty when remote control isn't configured for a unix socket, so
// the caller can fall back to a single best-effort call.
std::string kitty_socket_glob(const std::string& config_dir) {
  std::ifstream file(config_dir + "/kitty.conf");
  if (!file) return {};
  std::string line;
  while (std::getline(file, line)) {
    std::istringstream tokens(line);
    std::string key;
    tokens >> key;
    if (key != "listen_on") continue;
    std::string value;
    tokens >> value;
    static const std::string kUnixPrefix = "unix:";
    if (value.compare(0, kUnixPrefix.size(), kUnixPrefix) != 0) return {};
    std::string path = value.substr(kUnixPrefix.size());
    if (path.empty() || path[0] == '@') return {};  // abstract socket: no filesystem glob
    static const std::string kPidPlaceholder = "{kitty_pid}";
    if (const auto pos = path.find(kPidPlaceholder); pos != std::string::npos) {
      path.replace(pos, kPidPlaceholder.size(), "*");
      return path;
    }
    return path + "*";
  }
  return {};
}

std::string xresources_theme_path() {
  if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
    return std::string(home) + "/.Xresources.d/mepwm-theme";
  }
  return {};
}

// xterm and urxvt both read these class resources from the X resource
// database (RESOURCE_MANAGER) at startup; `*` matches either the class or
// instance name so per-window overrides in the user's own resources still
// take precedence.
void write_xresources_theme_file(const std::string& path, const std::string& fg, const std::string& bg,
                                  const std::string& accent) {
  std::ofstream file(path);
  if (!file) return;
  file << "xterm*background: " << bg << '\n'
       << "xterm*foreground: " << fg << '\n'
       << "xterm*cursorColor: " << accent << '\n'
       << "xterm*highlightColor: " << accent << '\n'
       << "URxvt*background: " << bg << '\n'
       << "URxvt*foreground: " << fg << '\n'
       << "URxvt*cursorColor: " << accent << '\n'
       << "URxvt*highlightColor: " << accent << '\n';
}

// xrdb (which xterm/urxvt both read their resources through) parses via cpp,
// so a plain #include is the natural, non-destructive way to pull our file
// in; append it once if the user's ~/.Xresources doesn't already have it.
void ensure_xresources_includes_theme(const std::string& theme_path) {
  const char* home = std::getenv("HOME");
  if (home == nullptr || *home == '\0') return;
  const std::string xresources_path = std::string(home) + "/.Xresources";
  const std::string include_line = "#include \"" + theme_path + "\"";
  std::ifstream existing(xresources_path);
  if (existing) {
    std::string line;
    while (std::getline(existing, line)) {
      if (line.find(theme_path) != std::string::npos) return;
    }
  }
  std::ofstream file(xresources_path, std::ios::app);
  if (file) file << '\n' << include_line << '\n';
}

// Every VT100-descended terminal (xterm, urxvt, alacritty, foot, kitty)
// accepts these OSC sequences to recolor itself live; the values are
// already-validated "#rrggbb" literals from the built-in theme tables,
// never user input, so no shell-injection risk from the substitution.
std::string osc_color_sequence(const std::string& fg, const std::string& bg, const std::string& accent) {
  return "\\033]10;" + fg + "\\007" + "\\033]11;" + bg + "\\007" + "\\033]12;" + accent + "\\007";
}

// Finds every currently-running xterm/urxvt/alacritty/foot session's pty
// (the direct child of the emulator process owns it) and writes the OSC
// color sequence straight into it, exactly as if the shell inside had
// printed it -- restyling already-open windows without their cooperation.
// kitty is excluded here since it's restyled separately via its own
// remote-control call, which also primes new-window colors.
std::string live_recolor_running_terminals_command(const std::string& fg, const std::string& bg,
                                                     const std::string& accent) {
  // Substring (not exact) match: distro packaging can wrap the real binary
  // (e.g. NixOS's comm shows ".xterm-wrapped", not "xterm").
  return "for _epid in $(ps -eo pid=,comm= | awk '$2 ~ "
         "/xterm|urxvt|rxvt-unicode|alacritty|foot/{print $1}'); do "
         "ps -eo ppid=,tty= | awk -v p=\"$_epid\" '$1==p && $2!=\"?\"{print $2}'; "
         "done | sort -u | while IFS= read -r _pty; do "
         "printf '" + osc_color_sequence(fg, bg, accent) + "' > \"/dev/$_pty\" 2>/dev/null; "
         "done";
}

}  // namespace

void sync_terminal_theme(const std::string& fg, const std::string& bg, const std::string& accent,
                          const std::function<void(const std::string&)>& spawn) {
  std::error_code error;

  if (const std::string config_dir = kitty_config_dir(); !config_dir.empty()) {
    std::filesystem::create_directories(config_dir, error);
    const std::string theme_path = config_dir + "/mepwm-theme.conf";
    write_kitty_theme_file(theme_path, fg, bg, accent);
    ensure_kitty_conf_includes_theme(config_dir);
    // `--configured` updates the palette new OS windows inherit within a
    // running kitty instance, so each call below covers both an
    // already-open instance and any future window it spawns; silently
    // no-ops if kitty isn't running or its remote control is disabled.
    const std::string set_colors_suffix =
        "set-colors --all --configured " + shell_single_quote(theme_path) + " >/dev/null 2>&1";
    if (const std::string socket_glob = kitty_socket_glob(config_dir); !socket_glob.empty()) {
      // Unquoted glob: it must expand to every live socket, not be matched
      // literally. Each candidate is a plain filesystem path (no shell
      // metacharacters -- kitty.conf's listen_on is a local, user-authored
      // config, not external input).
      spawn("command -v kitty >/dev/null 2>&1 && for _sock in " + socket_glob + "; do [ -S \"$_sock\" ] && "
            "kitty @ --to \"unix:$_sock\" " + set_colors_suffix + "; done; true");
    } else {
      spawn("command -v kitty >/dev/null 2>&1 && kitty @ " + set_colors_suffix + " || true");
    }
  }

  if (const std::string xresources_path = xresources_theme_path(); !xresources_path.empty()) {
    std::filesystem::create_directories(std::filesystem::path(xresources_path).parent_path(), error);
    write_xresources_theme_file(xresources_path, fg, bg, accent);
    ensure_xresources_includes_theme(xresources_path);
    // Merging our file directly (rather than just appending the #include)
    // takes effect immediately even if the user's shell never re-sources
    // ~/.Xresources this session.
    spawn("command -v xrdb >/dev/null 2>&1 && [ -n \"$DISPLAY\" ] && xrdb -merge " +
          shell_single_quote(xresources_path) + " >/dev/null 2>&1 || true");
  }
  spawn(live_recolor_running_terminals_command(fg, bg, accent) + " >/dev/null 2>&1 || true");
}

}  // namespace mepwm::core
