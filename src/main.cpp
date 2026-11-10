#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#elif defined(_WIN32)
#include <windows.h>
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
  // O_CLOEXEC: spawned programs (terminals!) must not inherit the lock fd,
  // or they keep "another instance is running" true after mepwm exits.
  const int fd = ::open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
  if (fd < 0) return true;  // can't create the lock file: don't block startup
  if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
    ::close(fd);
    return false;
  }
  return true;
}
#elif defined(_WIN32)
// Same one-overlay-per-session rule as macOS (a second instance would
// fight over the keyboard hook and draw duplicate chrome). The named mutex
// lives as long as the process holds its handle, i.e. until exit.
bool acquire_single_instance_lock() {
  HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\mepwm-single-instance");
  return mutex == nullptr || GetLastError() != ERROR_ALREADY_EXISTS;
}

// The shared engine keys every state path off $HOME (~/.local/share/mepwm,
// ~/TODO.org, the "~" in project paths). Windows has no HOME by default, so
// point it at the user profile for this process; `just install` uses the
// matching %USERPROFILE%\.local prefix, so installed assets resolve too.
void ensure_home() {
  const char* home = std::getenv("HOME");
  if (home != nullptr && *home != '\0') return;
  if (const char* profile = std::getenv("USERPROFILE"); profile != nullptr && *profile != '\0') {
    _putenv_s("HOME", profile);
  }
}

// MEP-wm.exe (GUI subsystem, launched from the Start menu) has no console
// to print to, so user-facing messages go to a dialog instead.
void report(const std::string& message, bool error) {
#if defined(MEPWM_GUI_SUBSYSTEM)
  MessageBoxA(nullptr, message.c_str(), "MEP-wm",
              MB_OK | MB_SETFOREGROUND | (error ? MB_ICONERROR : MB_ICONINFORMATION));
#else
  (void)error;
  std::cerr << "mepwm: " << message << '\n';
#endif
}
#endif

void print_usage(const char* program) {
  std::cout << "Usage: " << program
            << " [--backend x11|wayland|macos|windows] [--terminal COMMAND] [--modifier cmd|alt|ctrl|super|fn|fn+cmd]\n"
            << "\n"
            << "X11 controls: Super+h/j/k/l focuses by direction; Super+p opens the application picker; Super+Enter opens a terminal; "
               "Super+Shift+q exits.\n"
            << "Windows overlay controls: as on macOS below, with Mod defaulting to Alt (Win+L is reserved "
               "by Windows, so --modifier win loses focus-right unless Win+L lock is disabled in the registry).\n"
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
#if defined(_WIN32)
  ensure_home();
#endif
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
#if defined(_WIN32)
    // Asks a running instance to exit the way Mod+Shift+q does, so windows
    // hidden on other workspaces are shown again (`just service-stop`).
    if (argument == "--quit") {
      HWND running = FindWindowW(L"MepwmMessageWindow", nullptr);
      if (running == nullptr) {
        report("mepwm is not running.", false);
        return 1;
      }
      PostMessageW(running, WM_CLOSE, 0, 0);
      return 0;
    }
#endif
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

#if defined(_WIN32)
  if (backend == mepwm::BackendKind::Windows && !acquire_single_instance_lock()) {
    report("mepwm is already running.", false);
    return 0;
  }
#endif

  try {
    return mepwm::WindowManager(std::move(config)).run(backend);
  } catch (const std::exception& error) {
#if defined(_WIN32)
    report(error.what(), true);
#else
    std::cerr << "mepwm: " << error.what() << '\n';
#endif
    return 1;
  }
}
