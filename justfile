# Linux source list; on macOS the X11/Wayland files are not in the compile
# database, so lint there with: just lint "src/window_manager.cpp src/core/*.cpp src/main.cpp src/mep_wm_cli.cpp"
mepwm_sources := "src/x11/x11_backend.cpp src/window_manager.cpp src/wayland/wayland_backend.cpp src/wayland/wayland_backend.c src/core/geometry.cpp src/core/layout.cpp src/core/engine.cpp src/core/mep_theme.cpp src/core/terminal_theme.cpp src/core/theme_palette.cpp src/core/org_todo.cpp src/main.cpp src/mep_wm_cli.cpp"

build:
  cmake -S . -B build
  cmake --build build

run: build
  ./build/mepwm

# Install mepwm, mep-wm-cli, and the wallpaper assets. Defaults to ~/.local
# (no sudo needed); pass another prefix with e.g. `just install /usr/local`
# (run under sudo for system prefixes). On macOS this also puts MEP-wm.app
# in ~/Applications so Spotlight finds it.
install prefix=(env_var('HOME') + "/.local"): build
  cmake --install build --prefix "{{prefix}}"
  @if [ "$(uname)" = "Darwin" ]; then just install-app; fi

uninstall prefix=(env_var('HOME') + "/.local"):
  rm -f "{{prefix}}/bin/mepwm" "{{prefix}}/bin/mep-wm-cli"
  rm -rf "{{prefix}}/share/mepwm"
  @if [ "$(uname)" = "Darwin" ]; then just uninstall-app; fi

# macOS: copy the MEP-wm.app bundle where Spotlight indexes it
# (~/Applications; pass /Applications for all users, may need sudo). When
# the binary actually changed, the old Accessibility grant no longer matches
# the new code hash but still shows as a checked entry that suppresses the
# system prompt -- reset it so the next launch prompts cleanly.
install-app dest=(env_var('HOME') + "/Applications"): build
  @bash -c 'set -e; dest="{{dest}}"; mkdir -p "$dest"; \
    if ! cmp -s build/MEP-wm.app/Contents/MacOS/MEP-wm "$dest/MEP-wm.app/Contents/MacOS/MEP-wm" 2>/dev/null; then \
      tccutil reset Accessibility com.mepwm >/dev/null 2>&1 || true; \
    fi; \
    rm -rf "$dest/MEP-wm.app"; ditto build/MEP-wm.app "$dest/MEP-wm.app"; \
    echo "installed $dest/MEP-wm.app"'

uninstall-app dest=(env_var('HOME') + "/Applications"):
  rm -rf "{{dest}}/MEP-wm.app"

# macOS: install (or refresh) mepwm as a login LaunchAgent running the
# installed binary -- starts now and at every login, restarts on crashes
# (Mod+Shift+q stays exited). Logs to ~/Library/Logs/mepwm.log. The first
# run prompts for the Accessibility permission.
service-install prefix=(env_var('HOME') + "/.local"): (install prefix)
  ./scripts/macos-service.sh install "{{prefix}}/bin/mepwm"

# macOS: stop the service and remove its LaunchAgent (keeps the binaries).
service-uninstall:
  ./scripts/macos-service.sh uninstall

service-start:
  ./scripts/macos-service.sh start

service-stop:
  ./scripts/macos-service.sh stop

service-restart:
  ./scripts/macos-service.sh restart

service-status:
  ./scripts/macos-service.sh status

service-log:
  tail -f ~/Library/Logs/mepwm.log

# Static analysis over mep-wm's own sources (checks/suppressions in
# .clang-tidy; cppcheck reads compile_commands.json from `build`, which
# `build` above generates via CMAKE_EXPORT_COMPILE_COMMANDS, so it only ever
# sees our translation units, not X11/wlroots/Lua headers). Both tools are
# invoked in "hard fail" mode here (clang-tidy --warnings-as-errors, cppcheck
# --error-exitcode) even though .clang-tidy itself doesn't mark anything as
# an error, so editors using .clang-tidy directly (clangd) still just show
# warnings inline while `just lint` stays pass/fail for CI use.
# -Wno-unknown-warning-option: compile_commands.json carries CMakeLists.txt's
# -Werror plus a few GCC-only flags (-Wduplicated-cond and friends) that
# clang-tidy's own clang frontend doesn't recognize -- without this, that
# -Werror turns clang's "unknown warning option" notice into a hard parse
# error before clang-tidy gets to analyze anything.
lint sources=mepwm_sources: build
  clang-tidy -p build --warnings-as-errors='*' --extra-arg=-Wno-unknown-warning-option {{sources}}
  cppcheck --project=build/compile_commands.json --enable=warning,performance,portability \
    --inline-suppr --suppress=missingIncludeSystem --std=c++17 --check-level=exhaustive --error-exitcode=1 -i build

# Starts an isolated X server on :1 (or a supplied display), then runs mepwm inside it.
# Exit mepwm with Super+Shift+q; Xephyr then shuts down automatically.
xephyr display=":1": build
  @bash -c 'Xephyr "{{display}}" -screen 1280x800 -resizeable -ac -br -noreset 2>"${MEPWM_XEPHYR_LOG:-/tmp/mepwm-xephyr.log}" & xephyr_pid=$!; wm_pid=""; cleanup() { test -n "$wm_pid" && kill "$wm_pid" 2>/dev/null || true; kill "$xephyr_pid" 2>/dev/null || true; test -n "$wm_pid" && wait "$wm_pid" 2>/dev/null || true; wait "$xephyr_pid" 2>/dev/null || true; }; trap cleanup EXIT INT TERM; sleep 1; DISPLAY="{{display}}" LIBGL_ALWAYS_SOFTWARE=1 MEPWM_TERMINAL="xterm -fa monospace -fs 12" ./build/mepwm --backend x11 & wm_pid=$!; while kill -0 "$xephyr_pid" 2>/dev/null && kill -0 "$wm_pid" 2>/dev/null; do sleep 1; done'

# Measures time until mepwm has claimed the display and completed its first render.
benchmark-startup runs="5" display=":98": build
  ./scripts/benchmark-startup.sh "{{runs}}" "{{display}}"
