#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>

#include "mepwm/window_manager.hpp"

namespace {

void print_usage(const char* program) {
  std::cout << "Usage: " << program << " [--backend x11|wayland] [--terminal COMMAND]\n"
            << "\n"
            << "X11 controls: Super+h/j/k/l focuses by direction; Super+Enter opens a terminal; "
               "Super+Shift+q exits.\n";
}

}  // namespace

int main(int argc, char** argv) {
  mepwm::BackendKind backend = mepwm::BackendKind::X11;
  mepwm::Config config;
  if (const char* terminal = std::getenv("MEPWM_TERMINAL")) config.terminal = terminal;

  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--help" || argument == "-h") {
      print_usage(argv[0]);
      return 0;
    }
    if ((argument == "--backend" || argument == "--terminal") && index + 1 < argc) {
      const std::string value = argv[++index];
      if (argument == "--backend") {
        backend = mepwm::parse_backend(value);
      } else {
        config.terminal = value;
      }
      continue;
    }
    std::cerr << "Invalid argument: " << argument << '\n';
    print_usage(argv[0]);
    return 2;
  }

  try {
    return mepwm::WindowManager(std::move(config)).run(backend);
  } catch (const std::exception& error) {
    std::cerr << "mepwm: " << error.what() << '\n';
    return 1;
  }
}
