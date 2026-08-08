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

`Super+Shift+1` through `Super+Shift+9` send the focused window to that
workspace within the active project. The window moves off screen and focus
falls back to whatever the current workspace now shows; use `Super+1`
through `Super+9` to switch there and see it.

`Super+Space` (or `Super+a`) cycles the workspace between manual, master-stack,
and monocle layouts. `Super+h`, `j`, `k`, and `l` focus the nearest visible pane
or client to the left, down, up, or right. `Super+1` through `Super+9` select a
workspace within the active project, `Super+Enter` opens the configured terminal,
and `Super+Shift+q` exits the manager. The top bar shows all nine workspaces,
marks the current one, and displays its active project and layout.

`Super+p` opens an application picker. It searches XDG `.desktop` entries by
name; type to fuzzy-filter, use Up/Down (or Ctrl+p/Ctrl+n) to choose, Enter to
launch, and Escape to dismiss.

`Super+f` opens Vimium-style element hints: a small labeled chip appears over
every clickable spot currently on screen — workspace numbers, the layout and
task-bar entries, every dock icon and bottom-bar widget on every monitor, and
every visible window — and typing a chip's label clicks that element exactly
as a real click would. Backspace erases the last typed letter and Escape (or
a real mouse click) cancels.

`Super+b` toggles all of the border-bar chrome together -- the top bar,
bottom bar, and both side docks -- rather than just the top bar. It's
visual-only (their reserved screen space in the tiling math is unaffected),
but it also re-fits the wallpaper: see [Wallpaper](#wallpaper).

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
All of these, plus the toggleable side panel (notifications/todos/agents/
etc.), are borderless and forced fully opaque via
`_NET_WM_WINDOW_OPACITY` -- so a compositor's default translucency rules
for override-redirect or unfocused windows can't let the wallpaper show
through them.
The bottom dock reports battery, brightness, volume, microphone, media, theme,
Git branch, CPU, memory, disk, Wi-Fi, Bluetooth, keyboard layout, and clock state. Set
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
radio and device rows connect or disconnect. Media, Git, and keyboard-layout
panels likewise expose their original widget actions.
The Notifications, Todos, Agents, and Info (Wi-Fi/Bluetooth/media/etc.) panels
grab the keyboard while open, so `Escape` closes them and `?` toggles a
contextual help layer over that panel's own content -- a "? Toggle help" hint
sits at the bottom of each of these panels. This replaces those panels'
instructions being shown inline all the time; the full window-manager
keybinding list is still its own panel via `Super+Shift+/`.

The built-in widget glyphs use `UbuntuMono Nerd Font Mono`, matching MWM;
install the Ubuntu Mono Nerd Font for those icons to render.

Lua configuration can extend the bottom dock with
`mwm.widget({ name = "…", update = function() return "…" end, click = function(button) end, highlight = true })`.

When Xinerama reports more than one monitor, workspaces remain shared globally
and each window belongs to one monitor. `Super+,` and `Super+.` focus the
previous or next monitor; add Shift to send the focused window there. Dropping
a moved or resized floating window on another monitor also transfers it.

## Themes

MEPWM ships 15 built-in themes, 8 dark and 7 light: `dark`, `nord`,
`dracula`, `gruvbox-dark`, `tokyo-night`, `catppuccin-mocha`, `one-dark`,
`everforest-dark`, `light`, `solarized-light`, `gruvbox-light`,
`catppuccin-latte`, `rose-pine-dawn`, `everforest-light`, and `nord-light`.
Left/scroll-click the bottom-bar theme widget to cycle through them, or
middle-click it (or press `Super+Shift+t`) to open the theme picker -- a
fuzzy-searchable popup, the same kind used for the application and project
pickers, listing every theme with an accent-color swatch and letting you
type to filter or press Enter/click to select one directly. A theme is a
`{fg, bg, accent}` triple that drives every color in
the window manager: bar and sidebar text/background, card and highlight
fills, and both the focused (accent-colored) and unfocused window border --
switching themes recolors already-open windows immediately, not just new
ones. Lua config can register additional themes (or override a built-in
name) with `mwm.theme({name=, fg=, bg=, selected=})`.

## Wallpaper

MEPWM sets a random wallpaper (via `feh --bg-fill`) on startup and again
whenever the active theme's background crosses the light/dark threshold —
click, scroll, or select a different entry on the bottom-bar theme widget
(or its panel). Images are picked from `assets/light_comic_wallpapers` or
`assets/dark_comic_wallpapers` depending on the resulting theme; a relative
directory is first tried against the working directory `mepwm` was launched
from (so `just run`/`just xephyr`, run from the repo root, work unchanged),
then against the directory containing the running `mepwm` binary and its
parent -- which is how an installed build (e.g. the Nix package, which ships
`assets/` under `share/mep-wm/`) finds its wallpapers even when launched
from a session with an unrelated working directory. Override those
directories with the `MEPWM_WALLPAPER_LIGHT_DIR`/`MEPWM_WALLPAPER_DARK_DIR`
environment variables or `mwm.set_wallpapers(light_dir, dark_dir)` in
`config.lua`. Cycling between two themes on the same side of that threshold
(e.g. the built-in "dark" and "nord") leaves the current wallpaper alone.
Requires `feh` on `PATH`.

While the border bars are visible, the chosen image is fit to the desktop
rectangle they leave uncovered (not the full screen) and letterboxed with
the active theme's background color, so its framing matches what's actually
visible instead of being scaled full-screen and partly hidden under the
bars; `Super+b` (see [X11 controls](#x11-controls)) re-fits it edge-to-edge
the moment the bars are hidden, and back to the inset framing when they're
shown again. The letterboxing step shells out to ImageMagick's `convert`;
if it isn't on `PATH` (or fails), MEPWM falls back to filling the full
screen with the unprocessed image.

## Mouse controls

The X11 backend provides an arrow cursor in the nested display. Click a client
to focus it; the original click is replayed to the client. Click a workspace
number in the top bar to switch workspaces.
