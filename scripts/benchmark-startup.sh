#!/usr/bin/env bash
set -euo pipefail

runs="${1:-5}"
display="${2:-:98}"
case "$runs" in
  '' | *[!0-9]* | 0)
    echo "usage: $0 [positive-run-count] [display]" >&2
    exit 2
    ;;
esac
log_dir="$(mktemp -d "${TMPDIR:-/tmp}/mepwm-startup.XXXXXX")"
xvfb_pid=""

cleanup() {
  test -n "$xvfb_pid" && kill "$xvfb_pid" 2>/dev/null || true
  test -n "$xvfb_pid" && wait "$xvfb_pid" 2>/dev/null || true
  rm -rf "$log_dir"
}
trap cleanup EXIT INT TERM

Xvfb "$display" -screen 0 1280x800x24 -nolisten tcp >"$log_dir/xvfb.log" 2>&1 &
xvfb_pid=$!
sleep 0.1
if ! kill -0 "$xvfb_pid" 2>/dev/null; then
  cat "$log_dir/xvfb.log" >&2
  echo "mepwm startup benchmark: Xvfb failed to start on $display" >&2
  exit 1
fi

declare -a timings=()
for run in $(seq 1 "$runs"); do
  log="$log_dir/run-$run.log"
  DISPLAY="$display" MEPWM_STARTUP_TIMING=1 ./build/mepwm --backend x11 >"$log" 2>&1 &
  wm_pid=$!
  for _ in $(seq 1 500); do
    if grep -q 'mepwm: startup: ready ' "$log"; then break; fi
    if ! kill -0 "$wm_pid" 2>/dev/null; then break; fi
    sleep 0.01
  done
  if ! grep -q 'mepwm: startup: ready ' "$log"; then
    cat "$log" >&2
    echo "mepwm startup benchmark: run $run did not become ready" >&2
    exit 1
  fi
  timing="$(sed -n 's/.*total \([0-9.]*\)ms).*/\1/p' "$log" | tail -n 1)"
  timings+=("$timing")
  printf 'run %s: %sms\n' "$run" "$timing"
  kill "$wm_pid" 2>/dev/null || true
  wait "$wm_pid" 2>/dev/null || true
done

printf '%s\n' "${timings[@]}" | sort -n >"$log_dir/timings"
mean="$(awk '{ sum += $1 } END { printf "%.2f", sum / NR }' "$log_dir/timings")"
median="$(awk '{ values[NR] = $1 } END { if (NR % 2) printf "%.2f", values[(NR + 1) / 2]; else printf "%.2f", (values[NR / 2] + values[NR / 2 + 1]) / 2 }' "$log_dir/timings")"
minimum="$(head -n 1 "$log_dir/timings")"
maximum="$(tail -n 1 "$log_dir/timings")"
printf 'startup total: min %sms, median %sms, mean %sms, max %sms (%s runs)\n' "$minimum" "$median" "$mean" "$maximum" "$runs"
