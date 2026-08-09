#pragma once

#include <array>
#include <ctime>
#include <memory>
#include <unordered_map>
#include <vector>

#include "backend.hpp"
#include "core/layout.hpp"
#include "core/platform.hpp"
#include "core/tree.hpp"

namespace mepwm::core {

// Shared overlay window-management engine: owns the tiling order, layout
// parameters, and keybinding policy, and drives any core::Platform. Each
// overlay platform (macOS, Windows) only supplies mechanism via Platform.
class TilingEngine final : public Backend {
 public:
  explicit TilingEngine(std::unique_ptr<Platform> platform);

  int run(const Config& config) override;

 private:
  enum class PanelKind {
    None,
    Help,
    Todo,
    Notifications,
    Agents,
    Wifi,
    Bluetooth,
    Media,
    Battery,
    Volume,
    Mic,
    Git,
    Keyboard,
    Theme,
    System,
  };
  enum class PomodoroPhase { Idle, Work, Break };
  // Which list picker is waiting for a PickerSelected event.
  enum class PendingPicker { None, Projects, ActiveProjects, Themes, Wallpapers };

  static constexpr std::size_t kWorkspaceCount = 9;

  // Manual layout tree per workspace (X11 parity): leaves are tabbed panes.
  struct WorkspaceState {
    std::unique_ptr<PaneNode> root;
    PaneNode* selected = nullptr;  // pane that receives the next window
  };

  // A project owns its own set of 9 workspaces (X11's Project concept).
  // The active project's set lives in workspaces_; its stash here stays
  // empty until another project is switched in.
  struct ProjectState {
    std::string path;
    std::array<WorkspaceState, kWorkspaceCount> workspaces;
  };

  // A leaf's on-screen geometry for this frame: `tab_bar` sits above
  // `content` when the pane holds more than one tab.
  struct VisiblePane {
    PaneNode* leaf = nullptr;
    Rect content;
    Rect tab_bar;
    bool has_tab_bar = false;
  };

  // One pending "* TODO" headline from the project's TODO.org (X11 parity):
  // `line` is the 0-based line it was parsed from so it can be rewritten in
  // place, `active` mirrors an open org CLOCK line in its body.
  struct TodoItem {
    std::string text;
    std::size_t line = 0;
    bool active = false;
    std::time_t clock_start = 0;
  };

  void handle_event(const Event& event);
  void handle_chrome_click(const Event& event);
  void handle_panel_click(const std::string& cell);
  void handle_panel_key(const Event& event);
  std::vector<BarCell> panel_help_lines() const;
  void run_panel_action(SystemAction action, int value = 0, const std::string& argument = {});
  void toggle_pomodoro();
  void advance_pomodoro();
  std::string pomodoro_text() const;
  void reconcile_windows();
  void retile();
  void refresh_borders();
  void refresh_chrome();
  void refresh_side_panel();
  void refresh_tab_bars();
  void toggle_side_panel(PanelKind panel);
  Rect tiling_area();

  WorkspaceState& ws() { return workspaces_[current_workspace_]; }
  std::vector<VisiblePane> compute_visible_panes();
  std::vector<WindowId> windows_in(std::size_t workspace) const;
  PaneNode* current_leaf();       // leaf of the focused window, else selected
  void ensure_root();
  void activate_window(WindowId window);  // make it its pane's active tab + focus

  void focus_direction(Direction direction);
  void move_direction(Direction direction);  // move active tab to the neighbor pane
  void resize_direction(Direction direction);  // move the nearest matching split boundary
  void split_pane(bool vertical);
  void cycle_tab(int delta);
  void merge_pane();
  void adjust_ratio(float delta);

  int workspace_of(WindowId window) const;  // -1 when unmanaged
  void switch_workspace(std::size_t index);
  void send_to_workspace(std::size_t index);

  // Projects (per-project workspace sets, X11 parity).
  void initialize_projects();
  void save_projects() const;
  bool add_project(const std::string& raw_path, bool save = true);
  void switch_project(std::size_t index);
  bool project_has_windows(std::size_t index) const;
  bool in_stashed_project(WindowId window) const;  // parked in an inactive project
  std::string active_project_path() const;
  void open_project_picker(bool active_only);

  // Themes (named fg/bg/accent palettes, X11 parity). The chosen theme
  // drives the chrome colors, the border colors, and everything the
  // platform paints with the chrome palette.
  void open_theme_picker();
  void apply_theme(std::size_t index);
  // Applies `index` as an uncommitted live preview while the theme picker's
  // highlight sits on it; -1 reverts to the committed theme.
  void preview_theme(int index);
  std::size_t effective_theme() const;  // previewed theme if any, else committed
  void sync_theme_colors();  // border colors follow the active palette
  void save_theme() const;
  void load_theme();

  // Wallpapers (both theme buckets combined, X11 parity). The chosen image
  // persists next to the theme and is re-applied on startup.
  void open_wallpaper_picker();
  void apply_wallpaper(const std::string& path);
  void load_wallpaper();
  void handle_picker_selected(const Event& event);
  void spawn_terminal_in(const std::string& directory);
  // Overlay workspaces cannot unmap windows; inactive ones are parked in
  // the bottom-right corner (the AeroSpace technique).
  void park_window(WindowId window);

  // Todo sidebar (project-centric TODO.org, X11 parity).
  std::string todo_path() const;
  void load_todos();
  void mark_todo_done(const TodoItem& item);
  void add_todo(const std::string& text);
  void start_todo_clock(const TodoItem& item);
  void stop_todo_clock(const TodoItem& item);
  void toggle_todo_active();
  void move_todo_selection(int delta);
  void edit_todo_file();

  std::unique_ptr<Platform> platform_;
  Config config_;
  LayoutParams layout_;
  std::array<WorkspaceState, kWorkspaceCount> workspaces_;
  std::size_t current_workspace_ = 0;
  std::vector<ProjectState> projects_;
  std::size_t active_project_ = 0;
  PendingPicker pending_picker_ = PendingPicker::None;
  std::size_t theme_index_ = 0;
  int preview_theme_ = -1;  // live picker preview; -1 = none
  std::string wallpaper_path_;  // currently applied wallpaper; empty = untouched
  std::unordered_map<WindowId, WindowInfo> windows_;
  WindowId focused_ = kNoWindow;
  void notify(const std::string& text);
  void refresh_agents();

  PanelKind side_panel_ = PanelKind::None;
  bool bars_visible_ = true;  // mod+b border-chrome toggle (X11 Super+b)
  bool panel_help_visible_ = false;  // '?' overlay of contextual keybindings
  std::vector<TodoItem> todos_;
  int todo_selected_ = -1;
  int todo_scroll_ = 0;  // first visible todo row (engine-side windowing)
  bool todo_input_active_ = false;
  std::string todo_input_text_;
  SystemStatus status_;
  std::vector<std::string> notifications_;  // newest first, timestamped
  std::vector<AgentInfo> agents_;           // status files + process scan
  bool first_reconcile_ = true;
  unsigned long tick_count_ = 0;
  PomodoroPhase pomodoro_ = PomodoroPhase::Idle;
  std::time_t pomodoro_end_ = 0;
  bool icons_ = false;  // platform font covers the Nerd Font glyph range
};

}  // namespace mepwm::core
