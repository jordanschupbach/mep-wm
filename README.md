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

This creates a nested Xephyr display at `:1`; it never claims your host display.
If `:1` is already in use, choose another display, for example `just xephyr :2`.
Closing either the Xephyr window or mepwm stops the other process and returns to
your terminal.

Xephyr's XKB compiler may report non-fatal laptop/media-key warnings on some
systems. Its diagnostics are written to `/tmp/mepwm-xephyr.log` instead of the
terminal; set `MEPWM_XEPHYR_LOG` to choose another log file.

## X11 controls

The default layout is **manual**: new windows open as tabs in the selected pane.
Use `Super+v` to split that pane vertically (side by side) and `Super+s` to split
it horizontally. A new empty pane is selected so the next window opens there.
Every pane can hold tabs; switch its active tab with `Super+Tab`.

`Super+Space` (or `Super+a`) cycles the workspace between manual, master-stack,
and monocle layouts. `Super+j` and `Super+k` change focus, `Super+1` through
`Super+9` select a workspace, `Super+Enter` opens the configured terminal, and
`Super+Shift+q` exits the manager. The top bar shows all nine workspaces, marks
the current one, and displays its active layout.

When Xinerama reports more than one monitor, workspaces remain shared globally
and each window belongs to one monitor. `Super+,` and `Super+.` focus the
previous or next monitor; add Shift to send the focused window there. Dropping
a moved or resized floating window on another monitor also transfers it.

## Mouse controls

The X11 backend provides an arrow cursor in the nested display. Click a client
to focus it; the original click is replayed to the client. Click a workspace
number in the top bar to switch workspaces.
