#!/usr/bin/env bash
# Manages mepwm as a macOS LaunchAgent: a per-user background service that
# starts at login and restarts on crashes (clean exits -- Mod+Shift+q --
# stay exited). The agent runs the *installed* binary so the Accessibility
# grant attaches to a stable path, not a build-tree binary that moves.
set -euo pipefail

label="com.mepwm"
plist="$HOME/Library/LaunchAgents/$label.plist"
log_file="$HOME/Library/Logs/mepwm.log"
domain="gui/$(id -u)"

usage() {
  echo "usage: macos-service.sh install /path/to/mepwm | uninstall | start | stop | restart | status" >&2
  exit 64
}

case "${1:-}" in
  install)
    binary="${2:-}"
    [[ -n "$binary" ]] || usage
    if [[ ! -x "$binary" ]]; then
      echo "mepwm binary not found at $binary (run 'just install' first)" >&2
      exit 66
    fi
    mkdir -p "$(dirname "$plist")" "$(dirname "$log_file")"
    cat > "$plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key><string>$label</string>
  <key>ProgramArguments</key>
  <array>
    <string>$binary</string>
  </array>
  <key>RunAtLoad</key><true/>
  <key>KeepAlive</key>
  <dict>
    <key>SuccessfulExit</key><false/>
  </dict>
  <key>ProcessType</key><string>Interactive</string>
  <key>StandardOutPath</key><string>$log_file</string>
  <key>StandardErrorPath</key><string>$log_file</string>
</dict>
</plist>
EOF
    # Reload cleanly if a previous version of the agent is running.
    launchctl bootout "$domain/$label" 2>/dev/null || true
    launchctl bootstrap "$domain" "$plist"
    echo "mepwm service installed and started ($plist)"
    echo "logs: $log_file"
    echo "note: the first run prompts for the Accessibility permission"
    echo "(System Settings > Privacy & Security > Accessibility); reinstalls"
    echo "of the binary may require re-granting it."
    ;;
  uninstall)
    launchctl bootout "$domain/$label" 2>/dev/null || true
    rm -f "$plist"
    echo "mepwm service removed"
    ;;
  start)
    if [[ ! -f "$plist" ]]; then
      echo "mepwm service is not installed (run 'just service-install')" >&2
      exit 66
    fi
    if launchctl print "$domain/$label" >/dev/null 2>&1; then
      launchctl kickstart "$domain/$label"
    else
      launchctl bootstrap "$domain" "$plist"
    fi
    ;;
  stop)
    # Unload rather than signal: launchd would restart a signalled job
    # (KeepAlive treats a kill as an unsuccessful exit). 'start' reloads.
    launchctl bootout "$domain/$label" 2>/dev/null || true
    ;;
  restart)
    launchctl kickstart -k "$domain/$label"
    ;;
  status)
    if launchctl print "$domain/$label" >/dev/null 2>&1; then
      launchctl print "$domain/$label" | grep -E "state|pid|path|last exit" || true
    else
      echo "mepwm service is not loaded (run 'just service-install')"
    fi
    ;;
  *)
    usage
    ;;
esac
