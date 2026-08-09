# Cross-platform porting roadmap

MEP-wm began as an X11 window manager. The goal is to run it on macOS and
Windows as well, with as much shared code as possible. On Linux/X11 (and
eventually Wayland) mepwm *is* the window manager; on macOS and Windows it
runs as an **overlay manager** (in the style of AeroSpace/Amethyst on macOS):
the native window server keeps owning windows, and mepwm observes, arranges,
and focuses them through platform APIs.

## Architecture

The code is split along a bridge pattern:

```
main.cpp
  └─ WindowManager (include/mepwm/window_manager.hpp)   public API
       └─ Backend (src/backend.hpp)                     one per platform strategy
            ├─ X11Backend        (src/x11/)             native WM, owns the display
            ├─ WaylandBackend    (src/wayland/)         native compositor (experimental)
            └─ OverlayBackend    (src/core/engine.*)    shared overlay engine ...
                 └─ core::Platform (src/core/platform.hpp)   the bridge interface
                      ├─ MacosPlatform   (src/macos/)   AX API + Carbon hotkeys
                      └─ WindowsPlatform (src/windows/) Win32 (future)
```

- **`src/core/`** — platform-agnostic building blocks:
  - `geometry.hpp` — `Rect`, directional neighbor picking.
  - `layout.hpp/.cpp` — pure layout math (master/stack with gaps, mfact, nmaster).
  - `platform.hpp` — the `Platform` bridge interface: enumerate windows, get/set
    frames, focus, register hotkeys, spawn processes, run an event loop that
    reports `Event`s (window added/removed/focus changed/hotkey).
  - `engine.hpp/.cpp` — `TilingEngine`, a `Backend` that drives any `Platform`.
    All overlay platforms share this engine; adding a platform means
    implementing `Platform` only.
- **`src/x11/`** — the original full-featured X11 backend. It predates the
  core layer and still contains its own copies of layout/model logic; see
  "X11 migration" below.
- **`src/macos/`** — macOS `Platform` implementation (Objective-C++).
- **`src/windows/`** — Windows `Platform` implementation (not started).

### Rules of thumb

- New window-management *policy* (layout algorithms, focus rules, workspace
  semantics) goes in `src/core/`, never in a platform directory.
- Platform directories contain only *mechanism*: how to move a window, how to
  hear about a new window, how to bind a key.
- `core` must not include any platform header (X11, AppKit, Win32).

## Build matrix

| Platform | Backends built | Toggle |
|----------|----------------|--------|
| Linux    | x11 (+ wayland) | `MEPWM_ENABLE_X11`, `MEPWM_ENABLE_WAYLAND` |
| macOS    | macos overlay   | `MEPWM_ENABLE_MACOS` |
| Windows  | (planned)       | `MEPWM_ENABLE_WINDOWS` |

`nix develop` provides the toolchain on Linux and macOS; `nix build` produces
an installable package on both. A native installer (Homebrew cask / .pkg on
macOS, MSI/winget on Windows) is a later milestone.

## Phases

### Phase 1 — abstraction + build plumbing (done)
- [x] `core::Platform` bridge interface and shared `TilingEngine`
- [x] Shared layout math + directional focus in `src/core/`
- [x] macOS overlay backend: enumerate/tile/focus windows via the
      Accessibility API, global hotkeys (configurable modifier, Command by
      default), polling-based window tracking, border overlay windows with
      focus highlight
- [x] Per-platform CMake (X11/Wayland Linux-only, Objective-C++ on darwin)
- [x] Nix flake specialized for darwin (devShell + `packages.default`)
- [x] CMake install rules (binaries + assets)

### Phase 2 — solid macOS overlay
- [ ] AXObserver-based event delivery (replace/augment the poll timer):
      window created/destroyed/moved/resized/title, app launched/terminated
- [ ] Stable window identity via `_AXUIElementGetWindow` (CGWindowID), with
      the CFEqual registry as public-API fallback
- [ ] Multi-monitor: one layout region per `NSScreen`, move-window-to-display
- [ ] Floating heuristics: dialogs, panels, non-resizable windows, app rules
- [x] Workspaces 1..9: Mod+1..9 / Mod+Shift+1..9, clickable top-bar cells,
      park-in-corner emulation, focus-follows across workspaces. Possible
      upgrade later: native Spaces integration (private CGS APIs)
- [ ] Fullscreen/maximize/zoom parity bindings
- [x] Manual layout tree (splits + tabs) in `core` (src/core/tree.*), used
      by the overlay engine; tab stacking = same frame + raise active.
      Known limit: raising a background app's window above another app's in
      the same pane needs activation, so a pane mixing apps may briefly
      show the wrong tab on top until clicked/focused.
- [ ] Layout modes: re-add master/stack (core/layout.cpp) behind a
      cycle-layout binding like X11's
- [x] Clickable chrome v1: bottom-bar widget and left-dock cell clicks reach
      the engine (ChromeClicked events); widgets open wifi/bluetooth/media
      panels, pomodoro toggles, dock cells focus windows
- [x] Widgets: media/now-playing (Spotify+Music), mic, keyboard layout,
      theme/appearance, git, pomodoro in core
- [x] Panel-internal clicks: media prev/play/next, volume quick-set + mute,
      mic quick-set, keyboard layout switching, dark/light toggle
- [x] Nerd Font widget icons shared with the X11 backend (core/icons.hpp;
      auto-detected installed font, MEPWM_ICON_FONT override, text fallback).
      Follow-up: migrate x11_backend.cpp's kIcon* constants to core/icons.hpp
- [ ] Remaining widget gaps: backlight (needs private DisplayServices API or
      an external tool), Lua widgets (blocked on Lua-config port), drag
      sliders (X11's slider popup), power menu, connect/disconnect bluetooth
      devices, pick a wifi network
- [ ] Handle Mission Control / Stage Manager interference gracefully
- [ ] Launch-at-login + menu bar status item (overlay UX)

### Phase 3 — X11 migration onto core
Migrate the X11 backend piecewise onto `src/core/` so policy exists once.
Do this on Linux where the result can be compiled and run under Xephyr
(`just xephyr`). Suggested order:
- [ ] Replace X11Backend's layout math (`arrange_automatic`, `stack_column`)
      with `core::layout`
- [ ] Extract the `Node` tree / `Workspace` / `Project` model into `core`
- [ ] Extract directional focus/selection (`focus_direction`,
      `select_pane_direction`, `move_client_direction`) into `core`
- [ ] Keybinding table shared between backends (single source of truth for
      the help text, too)
- [ ] X11Backend implements `core::Platform` where it makes sense (it will
      keep native-WM extras: borders, bar, docks, tray, EWMH)

### Phase 4 — Windows overlay backend
- [ ] `WindowsPlatform` using Win32: `EnumWindows`/`SetWinEventHook` for
      discovery, `SetWindowPos`/DWM for placement, `RegisterHotKey` for keys
- [ ] Same `TilingEngine`; only mechanism code is new
- [ ] Build via MSVC or clang-cl; CI cross-check

### Phase 5 — installers & distribution
- [ ] `nix build` package output polished (both platforms) — done for basics
- [ ] macOS: codesigned .app bundle + Homebrew formula/cask, Accessibility
      permission onboarding flow
- [ ] Windows: MSI / winget manifest
- [ ] Linux: nix flake app, then AUR/apt as per TODO.org

## Feature parity checklist (overlay vs X11 backend)

The X11 backend's current surface, as a target list for the overlay engine.
"n/a" = inherently native-WM-only; the overlay should find a platform-native
equivalent or skip.

| Feature | X11 | macOS overlay | Windows |
|---|---|---|---|
| Directional focus (Super/Alt+hjkl) | ✅ | ✅ phase 1 | — |
| Move window between panes | ✅ | ✅ (Mod+Shift+hjkl) | — |
| Spawn terminal | ✅ | ✅ phase 1 | — |
| Manual layout tree (splits Super+v/s, tabs) | ✅ | ✅ core tree (src/core/tree.*): splits, tabbed panes with clickable per-pane tab bars, merge, split resize, empty-pane highlight. Tabs cycle with Mod+n / Mod+Tab (Cmd+Tab is system-reserved; Mod+p is the app picker) | — |
| Master/stack automatic layout | ✅ | ☐ (core/layout.cpp exists; needs a layout-mode toggle) | — |
| Layout cycling | ✅ | ☐ | — |
| Workspaces 1–9 (switch, send, bar cells) | ✅ | ✅ park-in-corner emulation (AeroSpace technique); focus follows Cmd-Tab across workspaces | — |
| Projects (per-project workspace sets) | ✅ | ✅ Mod+i recent-projects picker (typed path adds a project; opens a terminal there), Mod+o active-projects picker; state shared with X11 in `$XDG_DATA_HOME/mepwm/projects`; active project shown in the top bar; todo sidebar follows the project | — |
| Multi-monitor (Xinerama ⇄ NSScreen) | ✅ | ☐ | — |
| Floating windows, center/move/resize | ✅ | ☐ (leave-alone only) | — |
| Maximize / fullscreen toggles | ✅ | ☐ | — |
| Scratchpad | ✅ | ☐ | — |
| Kill focused window | ✅ | ☐ (AX close button) | — |
| Window borders / focus highlight | ✅ | ✅ phase 1 (click-through overlay ring windows) | — |
| Status bar, bottom widgets, pomodoro | ✅ | ◐ title/clock top bar; widgets: pomodoro (click to toggle), media/now-playing, git, keyboard layout, theme (system appearance), battery, volume+mute, mic, wifi, bluetooth, load, mem, disk; active-TODO pill at the bottom bar's left edge (red when idle, green with task text + live elapsed timer while clocked in, click opens the todo sidebar). Missing: backlight (no public API), Lua widgets (needs Lua-config port) | — |
| Side docks, tray (XEmbed) | ✅ | ◐ dock panels (clickable window cells left, layout stats right); no tray | — |
| Side panels / sidebars | ✅ (todo, wifi, bt, media, kbd, help, power, notifications, agents) | ◐ every widget opens a panel: wifi, bluetooth, media (+prev/play/next), power/battery, volume (mute + quick-set), mic, git (porcelain status), keyboard (click to switch layout), theme (toggle dark/light), system; right dock toggles notifications (engine event log), todo, agents (status files + process scan); info widget opens help. Panels take the keyboard while open (Esc closes, ? shows a contextual help overlay, footer hint below a separator); the todo panel has full X11 parity: ^n/^p or arrow navigation with a highlighted selection, `a` add (inline input), `d` mark done, `s` org-clock in/out, `e` edit TODO.org. Missing: power menu, drag sliders | — |
| Clickable chrome | ✅ | ◐ bottom-bar widgets, left-dock cells, and side-panel rows (non-activating panels); no drag/sliders yet | — |
| D-Bus notifications daemon | ✅ | n/a (macOS has its own) | — |
| Application launcher / pickers | ✅ | ◐ app picker (Mod+p) + project pickers (Mod+i/Mod+o) on a shared non-activating key panel with fuzzy filter; no window/theme pickers yet | — |
| Theme system + wallpaper pickers | ✅ | ☐ (`osascript` desktop image) | — |
| Lua config (rules, keybinds, widgets) | ✅ | ☐ (shared once in core) | — |
| IPC socket + mep-wm-cli | ✅ | ☐ (same unix socket works) | — |
| Hints overlay (Super+g) | ✅ | ☐ | — |
| Wi-Fi/Bluetooth/media panels | ✅ | n/a-ish (different system APIs) | — |

## Status log

- **2026-08-08** — Phase 1 implemented: core bridge (`src/core/`), macOS
  overlay backend (`src/macos/`), per-platform CMake + install rules, darwin
  nix flake with package output. X11/Wayland sources moved to `src/x11/`,
  `src/wayland/` (unchanged content); Linux build needs re-verification on a
  Linux machine (`just build`, `just xephyr`).
