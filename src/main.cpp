#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

#include "mepwm/window_manager.hpp"

namespace {

#if defined(__APPLE__)
// One overlay per session: a second instance (e.g. Spotlight launching
// MEP-wm.app while the LaunchAgent already runs) would fight over hotkeys
// and draw duplicate chrome. The lock's fd stays open (and thus held) for
// the process lifetime.
bool acquire_single_instance_lock() {
  const char* tmpdir = std::getenv("TMPDIR");
  const std::string path =
      std::string(tmpdir != nullptr && *tmpdir != '\0' ? tmpdir : "/tmp") + "/mepwm.lock";
  const int fd = ::open(path.c_str(), O_CREAT | O_RDWR, 0600);
  if (fd < 0) return true;  // can't create the lock file: don't block startup
  if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
    ::close(fd);
    return false;
  }
  return true;
}
#endif

void print_usage(const char* program) {
  std::cout << "Usage: " << program
            << " [--backend x11|wayland|macos|windows] [--terminal COMMAND] [--modifier cmd|alt|ctrl|super|fn|fn+cmd]\n"
            << "\n"
            << "X11 controls: Super+h/j/k/l focuses by direction; Super+p opens the application picker; Super+Enter opens a terminal; "
               "Super+Shift+q exits.\n"
            << "macOS overlay controls (Mod defaults to Globe/fn held with Command; change with --modifier or MEPWM_MODIFIER): "
               "Mod+h/j/k/l focuses panes by direction; Mod+Shift+h/j/k/l resizes the split toward that direction; "
               "Mod+Ctrl+h/j/k/l moves the window between panes; "
               "Mod+v/Mod+s split the pane (side by side / stacked); new windows open as tabs in the selected pane; "
               "Mod+p opens the application picker; Mod+i the projects picker (type a path to add a project); "
               "Mod+o the active-projects picker; "
               "Mod+n (or Mod+Tab/Mod+Shift+Tab) cycles tabs; Mod+m merges a pane with its sibling; Mod+-/= resizes the selected split; "
               "Mod+1..9 switches workspace; Mod+Shift+1..9 sends the window there; "
               "Mod+Enter opens a terminal; Mod+t toggles the todo sidebar; Mod+Shift+t opens the theme picker; "
               "Mod+Shift+/ toggles the help sidebar; Mod+Shift+q exits.\n";
}

mepwm::BackendKind default_backend() {
#if defined(__APPLE__)
  return mepwm::BackendKind::Macos;
#elif defined(_WIN32)
  return mepwm::BackendKind::Windows;
#else
  return mepwm::BackendKind::X11;
#endif
}

}  // namespace

int main(int argc, char** argv) {
  mepwm::BackendKind backend = default_backend();
  mepwm::Config config;
  if (const char* terminal = std::getenv("MEPWM_TERMINAL")) config.terminal = terminal;
  if (const char* modifier = std::getenv("MEPWM_MODIFIER")) config.modifier = modifier;
  if (const char* light_dir = std::getenv("MEPWM_WALLPAPER_LIGHT_DIR")) config.wallpaper_dir_light = light_dir;
  if (const char* dark_dir = std::getenv("MEPWM_WALLPAPER_DARK_DIR")) config.wallpaper_dir_dark = dark_dir;

  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--help" || argument == "-h") {
      print_usage(argv[0]);
      return 0;
    }
    if ((argument == "--backend" || argument == "--terminal" || argument == "--modifier") &&
        index + 1 < argc) {
      const std::string value = argv[++index];
      if (argument == "--backend") {
        backend = mepwm::parse_backend(value);
      } else if (argument == "--modifier") {
        config.modifier = value;
      } else {
        config.terminal = value;
      }
      continue;
    }
    std::cerr << "Invalid argument: " << argument << '\n';
    print_usage(argv[0]);
    return 2;
  }

#if defined(__APPLE__)
  // Exit 0 so a LaunchAgent-run duplicate is not treated as a crash (its
  // KeepAlive only restarts unsuccessful exits).
  if (backend == mepwm::BackendKind::Macos && !acquire_single_instance_lock()) {
    if (isatty(STDERR_FILENO) != 0) {
      std::cerr << "mepwm: another instance is already running\n";
    } else {
      // Launched from Spotlight/Finder: no terminal to print to, so say why
      // nothing appeared. DisplayNotice returns without waiting for the OK.
      CFUserNotificationDisplayNotice(0, kCFUserNotificationNoteAlertLevel, nullptr, nullptr,
                                      nullptr, CFSTR("MEP-wm"),
                                      CFSTR("mepwm is already running."), CFSTR("OK"));
    }
    return 0;
  }
#endif

  try {
    return mepwm::WindowManager(std::move(config)).run(backend);
  } catch (const std::exception& error) {
    std::cerr << "mepwm: " << error.what() << '\n';
    return 1;
  }
}
