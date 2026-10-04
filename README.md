# MEP-wm

MEP-wm (Mise En Place window manager) is a window manager that has everything in place.

On Linux, mepwm is the window manager (X11 native, experimental Wayland).
On macOS it runs as an **overlay manager** (AeroSpace/Amethyst style): the
native window server keeps owning windows and mepwm arranges them through the
Accessibility API. A Windows overlay backend is planned. Shared policy
(layouts, focus rules, keybindings) lives in `src/core/` behind a
`core::Platform` bridge; see `docs/PORTING.md` for the architecture and the
parity roadmap.

## Development

Enter the Nix shell and build the project:

```sh
nix develop
just build
```

`nix build` produces an installable package (`result/bin/mepwm`) on both
Linux and macOS.

`mepwm` is split into a reusable `mepwm` library and a small executable in
`src/main.cpp`. It has an X11 tiling backend, an experimental wlroots-based
Wayland compositor backend, and a macOS overlay backend. Select one with
`--backend x11|wayland|macos` (the default is the native backend for the
platform you built on).

## Using mep-wm as a flake input

Another flake can consume mep-wm directly instead of vendoring the source
(e.g. as a git submodule):

```nix
{
  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-25.11";
    # mep-wm's Wayland backend needs wlroots 0.20, which typically lags
    # behind on stable channels, so pin it to an unstable nixpkgs input.
    unstable.url = "github:NixOS/nixpkgs/nixos-unstable";
    mep-wm.url = "github:jordanschupbach/mep-wm";
    mep-wm.inputs.nixpkgs.follows = "unstable";
  };

  outputs = { self, nixpkgs, unstable, mep-wm, ... }: {
    # e.g. inside a NixOS module or package set:
    #   mep-wm.packages.${system}.default
  };
}
```

The `default` package installs `bin/mepwm` and `bin/mep-wm-cli`. Add it to
`environment.systemPackages` (NixOS) or `home.packages` (home-manager) like
any other derivation:

```nix
{ inputs, pkgs, ... }: {
  environment.systemPackages = [
    inputs.mep-wm.packages.${pkgs.system}.default
  ];
}
```

To bump the pinned mep-wm commit later, run `nix flake lock --update-input
mep-wm` in the consuming flake (or `nix flake update` to bump everything).

## macOS overlay

```sh
nix develop
just build
./build/mepwm            # defaults to --backend macos on macOS
```

The first run triggers the system Accessibility prompt (run from a
terminal, mepwm inherits the terminal's permission; launched as an app it
needs its own grant). mepwm waits until the permission is granted under
System Settings → Privacy & Security → Accessibility and then starts on
its own -- no relaunch needed.

### Installing (macOS)

```sh
just install           # binaries + assets into ~/.local (or: just install /usr/local)
just service-install   # install + run as a login LaunchAgent
```

`just install` also copies a `MEP-wm.app` bundle into `~/Applications`, so
Spotlight and Launchpad find "MEP-wm" (`just install-app /Applications`
installs it for all users instead). The app is a menu-bar-less accessory
(`LSUIElement`) running the same program as the CLI binary; a
single-instance lock makes a Spotlight launch exit quietly when the
LaunchAgent (or a terminal run) already has an overlay running.

`just service-install` registers `~/Library/LaunchAgents/com.mepwm.plist`
pointing at the installed binary: mepwm starts immediately, at every login,
and restarts on crashes -- quitting with `Mod+Shift+q` stays quit. Logs go
to `~/Library/Logs/mepwm.log` (`just service-log` tails them). Manage it
with `just service-start` / `service-stop` / `service-restart` /
`service-status`, and remove it with `just service-uninstall` (keeps the
binaries; `just uninstall` removes those). Because the service runs the
binary directly, macOS ties the Accessibility grant to it -- rerunning
`just service-install` after a rebuild reinstalls the binary and may make
macOS ask for the Accessibility permission again.

Windows tile inside the screen's *visible frame*, so the macOS menu bar and
Dock always keep their space. Inside that area mepwm draws its own chrome as
click-through overlay panels: a top bar (focused window title + clock), a
bottom bar (pomodoro, now-playing, git branch, keyboard layout, appearance,
battery, volume, mic, wifi, bluetooth, load, memory, disk, and an info
widget that opens the keybinding help sidebar), a left dock (one cell per
managed window, focused highlighted), and a right dock with the X11-style
sidebar toggles: notifications (a log of window/pomodoro events), todos,
and AI agents (claude/codex processes plus `mwm-agents` status files; the
cell highlights when an agent needs input). The
bars accept clicks without stealing focus: the pomodoro widget starts/stops
a 25/5 timer, every other widget opens its sidebar panel (wifi, bluetooth,
media with prev/play/next, battery, volume with mute and quick-set levels,
mic, git status, keyboard with click-to-switch layouts, theme with a
dark/light toggle, and a system panel for load/mem/disk), and a left-dock
cell focuses that window. Rows starting with `>` in a panel are clickable.
The now-playing widget reads Spotify or Music when one is running; its
first use (and the dark/light toggle, via System Events) triggers macOS's
one-time Automation permission prompt.

Widgets use the same Nerd Font icons as the X11 bar when a Nerd Font is
installed (mepwm picks the first installed "* Nerd Font" family
automatically; override with `MEPWM_ICON_FONT="JetBrainsMono Nerd Font"` or
disable icons with `MEPWM_ICON_FONT=none`). Without one, widgets fall back
to text labels.
`Mod+t` opens the todo picker (a fuzzy list of the pending todos in
`MEPWM_TODO_FILE`, else `TODO.org` in the launch directory, else
`~/TODO.org`; Enter clocks the highlighted one in), `Mod+Shift+t` toggles
the todo sidebar over the same file, `Mod+Shift+u` opens the theme picker
(live preview while the highlight moves; Enter commits, Escape reverts),
and `Mod+Shift+/` toggles a keybinding help sidebar. The wifi widget shows just "on" unless
mepwm has the Location permission (macOS gates SSIDs behind it). The `gap`
setting pads the tiled windows, and each managed window gets a border ring
in the gap (`border_color_focused` highlights the focused window). Set
`MEPWM_DEBUG=1` to log hotkey registration/presses and spawns to stderr.

The manual split/tab layout (`Super+v`/`Super+s`/`Super+Tab` on X11) has not
been ported to the overlay engine yet -- macOS currently tiles master/stack
only. See docs/PORTING.md phase 2.

The layout is the same manual tree as X11: every pane holds tabs, and new
windows open as tabs in the selected pane. The modifier (`Mod` below) is
the Globe/fn key held together with Command by default. `Mod+v` splits the
selected pane side by side, `Mod+s` splits it stacked (the new empty pane
is selected and highlighted, so the next window opens there),
`Mod+n`/`Mod+p` and `Mod+Tab`/`Mod+Shift+Tab` cycle a pane's tabs,
`Mod+m` (or `Mod+d`, as on X11) merges
a pane with its sibling (absorbing its windows as tabs), and
`Mod+-`/`Mod+=` resize the selected split. Panes with more than one tab get a clickable tab bar.
`Mod+h/j/k/l` focuses panes by direction, `Mod+Shift+h/j/k/l` resizes by
moving the nearest split boundary in that direction, `Mod+Ctrl+h/j/k/l`
moves the focused window into the neighboring pane, `Mod+1..9` switches
workspace,
`Mod+Shift+1..9` sends the window there, `Mod+Enter` opens a terminal, and
`Mod+Shift+q` exits (windows keep their last arranged frames).

Workspaces are emulated the way AeroSpace does it: windows on inactive
workspaces are parked in the bottom-right corner of the screen (macOS
keeps a small sliver visible; the window is still alive and Cmd-Tab-able).
Switching re-tiles the incoming workspace; focusing a parked window by any
means (Cmd-Tab, Dock) automatically switches to its workspace. The top bar
shows a cell per occupied workspace -- click one to switch.

The default `fn+cmd` modifier avoids clashing with application shortcuts
(fn-involving hotkeys are matched via an event tap, since macOS's hotkey
API cannot register fn as a modifier). Prefer a different key? Run with
`--modifier fn|cmd|alt|ctrl` (or set `MEPWM_MODIFIER`) -- `fn` uses the
Globe/fn key alone, and note the Command modifier shadows `Cmd+H` (Hide),
`Cmd+M` (Minimize), `Cmd+R` (reload), and `Cmd+-`/`Cmd+=` (zoom) in every
app, while `alt` gives the AeroSpace-style Option layout. If you remapped
the Globe key in System Settings > Keyboard (e.g. to switch input sources),
held-down fn still works as the modifier.

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
and `Super+Shift+q` exits the manager. `Super+1`-`Super+9` switch only the
workspace of whichever monitor is currently focused -- each monitor tracks
its own workspace independently. The top bar (one per monitor, each showing
that monitor's own workspaces) shows all nine workspaces, marks the current
one, and displays its active project and layout.

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

Each project owns an independent set of nine workspaces per monitor. `Super+i` opens the
project picker; choose a saved project, or type an existing directory and press
Enter to add, switch to it, and open a terminal there. `Super+u` opens the active-project picker, which
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

The Todos panel (`Super+Shift+t`) reads the active project's `TODO.org`:
`a` adds a headline, `d` marks one DONE, `s` clocks it in or out (org-mode
`CLOCK:` lines in a `:LOGBOOK:` drawer, so the file stays usable in Emacs),
and `e` opens the file in `$EDITOR`. `Super+t` opens the todo picker
instead -- the same fuzzy popup as the application and project pickers,
with a preview pane beside the list that shows the highlighted todo's notes
and its logbook (entry count, total time, last clock-out, then each clock
line). Enter clocks that todo in, clocking out whichever one was running
first, so picking what to work on next is a few letters and a keystroke;
the red/green pill at the bottom-left shows the running todo and its
elapsed time.

The built-in widget glyphs use `UbuntuMono Nerd Font Mono`, matching MWM;
install the Ubuntu Mono Nerd Font for those icons to render.

Lua configuration can extend the bottom dock with
`mwm.widget({ name = "…", update = function() return "…" end, click = function(button) end, highlight = true })`.

When Xinerama reports more than one monitor, each monitor gets its own
independent set of nine workspaces (and its own top bar) -- switching a
workspace on one monitor never affects another. `Super+,` and `Super+.` focus
the previous or next monitor; add Shift to send the focused window there,
landing it on that monitor's currently visible workspace. `Super+o` is the
shorthand for exactly two monitors: it focuses the other one, and
`Super+Shift+o` sends the focused window there. Dropping a moved or resized
floating window on another monitor also transfers it.

## Themes

MEPWM ships 15 built-in themes, 8 dark and 7 light: `dark`, `nord`,
`dracula`, `gruvbox-dark`, `tokyo-night`, `catppuccin-mocha`, `one-dark`,
`everforest-dark`, `light`, `solarized-light`, `gruvbox-light`,
`catppuccin-latte`, `rose-pine-dawn`, `everforest-light`, and `nord-light`.
Left/scroll-click the bottom-bar theme widget to cycle through them, or
middle-click it (or press `Super+Shift+u`) to open the theme picker -- a
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

Press `Super+Shift+w` to pick a wallpaper by hand instead of waiting for the
next random reroll -- the same fuzzy-searchable popup as the application,
project, and theme pickers, listing every image from both the light and dark
directories, but with an fzf/Telescope-style preview pane beside the list
that renders the highlighted row's image (via Imlib2) as you move the
selection. Enter sets it as the current wallpaper immediately; Escape closes
the picker without changing anything. A manually-picked wallpaper sticks
until the active theme's background actually crosses the light/dark
threshold, the same as any other wallpaper (see above).

## Mouse controls

The X11 backend provides an arrow cursor in the nested display. Click a client
to focus it; the original click is replayed to the client. Click a workspace
number in the top bar to switch workspaces.
