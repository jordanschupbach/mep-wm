# MEP-wm

MEP-wm (Mise En Place window manager) is a window manager that has everything in place.

## Development

Enter the Nix shell and build the project:

```sh
nix develop
just build
```

`mepwm` is split into a reusable `mepwm` library and a small executable in
`src/main.cpp`. It has an X11 tiling backend and an experimental wlroots-based
Wayland compositor backend. Select one with `--backend x11` or `--backend wayland`.

For safe X11 development inside an existing X11 desktop, run:

```sh
just xephyr
```

This creates a resizable nested Xephyr display at `:1`; it never claims your host display.
If `:1` is already in use, choose another display, for example `just xephyr :2`.
Closing either the Xephyr window or mepwm stops the other process and returns to
your terminal.

Xephyr's XKB compiler may report non-fatal laptop/media-key warnings on some
systems. Its diagnostics are written to `/tmp/mepwm-xephyr.log` instead of the
terminal; set `MEPWM_XEPHYR_LOG` to choose another log file.

The Xephyr recipe uses xterm's FreeType `monospace` font rather than its legacy
bitmap default, and forces software GL for clients in the nested display. This
avoids missing-font and DRI3 warnings without changing the host session. A
Firefox `CanCreateUserNamespace` error is instead a host/container sandbox
restriction: enable unprivileged user namespaces in the environment running
Xephyr, rather than disabling Firefox's content sandbox.

To measure cold X11 startup in an isolated Xvfb server, run `just benchmark-startup`
(or `just benchmark-mepwm` from the repository root). Set
`MEPWM_STARTUP_TIMING=1` when launching `mepwm` directly to log each startup
phase. Dynamic sidebar widget data is intentionally populated after the first
frame so system-command probes cannot delay the window manager becoming ready.

## X11 controls

The default layout is **manual**: new windows open as tabs in the selected pane.
Use `Super+v` to split that pane vertically (side by side) and `Super+s` to split
it horizontally. A new empty pane is selected so the next window opens there.
Every pane can hold tabs; switch its active tab with `Super+Tab`.
Resize the selected manual-layout pane with `Super+Shift+h`, `j`, `k`, or `l`.
`h` and `k` shrink the pane; `j` and `l` grow it. At an outer edge, the
binding uses the only available split boundary.

`Super+Space` (or `Super+a`) cycles the workspace between manual, master-stack,
and monocle layouts. `Super+h`, `j`, `k`, and `l` focus the nearest visible pane
or client to the left, down, up, or right. `Super+1` through `Super+9` select a
workspace within the active project, `Super+Enter` opens the configured terminal,
and `Super+Shift+q` exits the manager. The top bar shows all nine workspaces,
marks the current one, and displays its active project and layout.

`Super+p` opens an application picker. It searches XDG `.desktop` entries by
name; type to fuzzy-filter, use Up/Down (or Ctrl+p/Ctrl+n) to choose, Enter to
launch, and Escape to dismiss.

## Projects

Each project owns an independent set of nine workspaces. `Super+i` opens the
project picker; choose a saved project, or type an existing directory and press
Enter to add, switch to it, and open a terminal there. `Super+o` opens the active-project picker, which
lists only projects with open clients. Project paths persist in
`$XDG_DATA_HOME/mepwm/projects` (or `~/.local/share/mepwm/projects`), and the
home directory is available as the default project.

## Sidebars and widgets

MEPWM provides a left application dock, a bottom system-widget dock, and a
right dock for notifications, todos, agents, layouts, and keybinding help.
The bottom dock reports battery, brightness, volume, microphone, media, theme,
Git branch, load, CPU, memory, disk, Wi-Fi, Bluetooth, keyboard layout, and clock state. Set
`MEPWM_PROJECT_DIR` to a repository path to enable the Git branch indicator. Scroll
brightness, volume, or microphone to adjust it; left-click opens its slider.
Middle-click volume/microphone toggles mute, and media controls use middle
click for play/pause plus the scroll wheel for next/previous. The notification
panel is an `org.freedesktop.Notifications` service when MEPWM owns that
session-bus name. Todos persist in `~/.local/share/mepwm/todos`; Lua config may
add entries with `mwm.todo("text")` (which returns its ID), manage them with
`mwm.todo_toggle(id)`, `mwm.todo_remove(id)`, or `mwm.todo_clear_completed()`,
and create panel notifications with `mwm.notify("summary", "body")`. The
Agents panel detects running Claude and Codex processes; selecting a matching
client focuses its window when its PID is available through EWMH.
All panels scroll with the mouse wheel. Their header `clear` actions dismiss
notifications, completed todos, or file-backed agent statuses; middle-click an
individual notification or file-backed agent to dismiss it.
Wi-Fi and Bluetooth open actionable device panels; their first row toggles the
radio and device rows connect or disconnect. Media, Git, keyboard-layout, and
theme panels likewise expose their original widget actions.

The built-in widget glyphs use `UbuntuMono Nerd Font Mono`, matching MWM;
install the Ubuntu Mono Nerd Font for those icons to render.

Lua configuration can extend the bottom dock with
`mwm.widget({ name = "…", update = function() return "…" end, click = function(button) end, highlight = true })`.

When Xinerama reports more than one monitor, workspaces remain shared globally
and each window belongs to one monitor. `Super+,` and `Super+.` focus the
previous or next monitor; add Shift to send the focused window there. Dropping
a moved or resized floating window on another monitor also transfers it.

## Mouse controls

The X11 backend provides an arrow cursor in the nested display. Click a client
to focus it; the original click is replayed to the client. Click a workspace
number in the top bar to switch workspaces.
