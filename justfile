mepwm_sources := "src/x11_backend.cpp src/window_manager.cpp src/wayland_backend.cpp src/wayland_backend.c src/main.cpp src/mep_wm_cli.cpp"

build:
  cmake -S . -B build
  cmake --build build

run: build
  ./build/mepwm

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
lint: build
  clang-tidy -p build --warnings-as-errors='*' --extra-arg=-Wno-unknown-warning-option {{mepwm_sources}}
  cppcheck --project=build/compile_commands.json --enable=warning,performance,portability \
    --inline-suppr --suppress=missingIncludeSystem --std=c++17 --check-level=exhaustive --error-exitcode=1 -i build

# Starts an isolated X server on :1 (or a supplied display), then runs mepwm inside it.
# Exit mepwm with Super+Shift+q; Xephyr then shuts down automatically.
xephyr display=":1": build
  @bash -c 'Xephyr "{{display}}" -screen 1280x800 -resizeable -ac -br -noreset 2>"${MEPWM_XEPHYR_LOG:-/tmp/mepwm-xephyr.log}" & xephyr_pid=$!; wm_pid=""; cleanup() { test -n "$wm_pid" && kill "$wm_pid" 2>/dev/null || true; kill "$xephyr_pid" 2>/dev/null || true; test -n "$wm_pid" && wait "$wm_pid" 2>/dev/null || true; wait "$xephyr_pid" 2>/dev/null || true; }; trap cleanup EXIT INT TERM; sleep 1; DISPLAY="{{display}}" LIBGL_ALWAYS_SOFTWARE=1 MEPWM_TERMINAL="xterm -fa monospace -fs 12" ./build/mepwm --backend x11 & wm_pid=$!; while kill -0 "$xephyr_pid" 2>/dev/null && kill -0 "$wm_pid" 2>/dev/null; do sleep 1; done'

# Measures time until mepwm has claimed the display and completed its first render.
benchmark-startup runs="5" display=":98": build
  ./scripts/benchmark-startup.sh "{{runs}}" "{{display}}"
