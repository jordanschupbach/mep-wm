build:
  cmake -S . -B build
  cmake --build build

run: build
  ./build/mepwm

# Starts an isolated X server on :1 (or a supplied display), then runs mepwm inside it.
# Exit mepwm with Super+Shift+q; Xephyr then shuts down automatically.
xephyr display=":1": build
  @bash -c 'Xephyr "{{display}}" -screen 1280x800 -ac -br -noreset 2>"${MEPWM_XEPHYR_LOG:-/tmp/mepwm-xephyr.log}" & xephyr_pid=$!; wm_pid=""; cleanup() { test -n "$wm_pid" && kill "$wm_pid" 2>/dev/null || true; kill "$xephyr_pid" 2>/dev/null || true; test -n "$wm_pid" && wait "$wm_pid" 2>/dev/null || true; wait "$xephyr_pid" 2>/dev/null || true; }; trap cleanup EXIT INT TERM; sleep 1; DISPLAY="{{display}}" LIBGL_ALWAYS_SOFTWARE=1 MEPWM_TERMINAL="xterm -fa monospace -fs 12" ./build/mepwm --backend x11 & wm_pid=$!; while kill -0 "$xephyr_pid" 2>/dev/null && kill -0 "$wm_pid" 2>/dev/null; do sleep 1; done'

# Measures time until mepwm has claimed the display and completed its first render.
benchmark-startup runs="5" display=":98": build
  ./scripts/benchmark-startup.sh "{{runs}}" "{{display}}"
