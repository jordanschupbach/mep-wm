#include "backend.hpp"

#include <X11/XKBlib.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xft/Xft.h>
#include <X11/extensions/Xinerama.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>
#include <dbus/dbus.h>
#include <lua.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <ctime>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <numeric>
#include <optional>
#include <poll.h>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/statvfs.h>
#include <sys/un.h>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace mepwm {
namespace {

constexpr int kWorkspaceCount = 9;
constexpr int kDockWidth = 48;
constexpr int kBarHeight = kDockWidth;
// Height reserved above a manual-mode pane when it holds more than one
// client, for the row of clickable tabs naming each client in that pane.
constexpr int kPaneTabBarHeight = 24;
constexpr int kMinWindowSize = 20;
constexpr double kResizeStep = 0.05;
constexpr double kMinSplitWeight = 0.05;
constexpr int kTraySpacing = 4;
constexpr int kLauncherWidth = 640;
constexpr int kLauncherMaxRows = 8;
constexpr long kSystemTrayRequestDock = 0;
constexpr long kXEmbedEmbeddedNotify = 0;
constexpr long kXEmbedMapped = 1 << 0;
// These match MWM's built-in widget glyphs. They live in Nerd Font's
// private-use ranges and are drawn with the dedicated fallback font below.
constexpr const char kIconBatteryFull[] = "\uf240";
constexpr const char kIconBattery75[] = "\uf241";
constexpr const char kIconBattery50[] = "\uf242";
constexpr const char kIconBattery25[] = "\uf243";
constexpr const char kIconBatteryEmpty[] = "\uf244";
constexpr const char kIconBatteryCharging[] = "\uf0e7";
constexpr const char kIconBacklight[] = "\U000f0599";
constexpr const char kIconVolumeMuted[] = "\uf026";
constexpr const char kIconVolumeLow[] = "\uf027";
constexpr const char kIconVolumeHigh[] = "\uf028";
constexpr const char kIconThemeDark[] = "\uf186";
constexpr const char kIconThemeLight[] = "\U000f0599";
constexpr const char kIconCpu[] = "\U000f061a";
constexpr const char kIconMemory[] = "\U000f035b";
constexpr const char kIconDisk[] = "\uf0a0";
constexpr const char kIconWifi[] = "\uf1eb";
constexpr const char kIconBluetooth[] = "\uf293";
constexpr const char kIconMicrophone[] = "\uf130";
constexpr const char kIconMicrophoneMuted[] = "\uf131";
constexpr const char kIconGit[] = "\uf126";
constexpr const char kIconMedia[] = "\uf001";
constexpr const char kIconKeyboard[] = "\uf11c";
constexpr const char kIconClock[] = "\uf017";
constexpr const char kIconLauncher[] = "\uf135";
constexpr const char kIconFirefox[] = "\uf269";
constexpr const char kIconTerminal[] = "\uf120";
constexpr const char kIconInkscape[] = "\ue801";
constexpr const char kIconGimp[] = "\ue7e7";
constexpr const char kIconLibreOffice[] = "\uf376";
constexpr const char kIconVsCode[] = "\ue8da";
constexpr const char kIconEmacs[] = "\ue7cf";
constexpr const char kIconNeovim[] = "\ue6a9";
constexpr const char kIconBell[] = "\uf0f3";
constexpr const char kIconTodo[] = "\uf0ae";
constexpr const char kIconAgents[] = "\uf120";
constexpr const char kIconInfo[] = "\uf05a";
constexpr const char kIconLayoutTile[] = "\uf00a";
constexpr const char kIconLayoutMonocle[] = "\uf2d0";
constexpr const char kIconLayoutOther[] = "\uf009";
constexpr const char kIconNixOs[] = "\uf313";
int another_window_manager = 0;

class StartupTimer {
 public:
  StartupTimer() : enabled_(std::getenv("MEPWM_STARTUP_TIMING") != nullptr), started_(Clock::now()), previous_(started_) {}

  void checkpoint(const char* phase) {
    if (!enabled_) return;
    const Clock::time_point now = Clock::now();
    const auto elapsed = std::chrono::duration<double, std::milli>(now - previous_).count();
    const auto total = std::chrono::duration<double, std::milli>(now - started_).count();
    std::cerr << "mepwm: startup: " << phase << " +" << elapsed << "ms (total " << total << "ms)\n";
    previous_ = now;
  }

 private:
  using Clock = std::chrono::steady_clock;

  bool enabled_;
  Clock::time_point started_;
  Clock::time_point previous_;
};

enum class Orientation { Vertical, Horizontal };
enum class LayoutMode { Manual, MasterStack, Monocle };
enum class SliderKind { Backlight, Volume, Microphone };
enum class SidePanel { Closed, Notifications, Todos, Agents, Help, Info };
enum class LauncherMode { Applications, Projects, ActiveProjects, Windows };
// Note: cannot use "None" as a member name here — X11/X.h (pulled in via
// Xlib.h) #defines None to 0L, which breaks enum class member declarations.
enum class InfoAction { NoneAction, Wifi, Bluetooth, Media, Git, Keyboard, Theme };

// A leaf's last-arranged screen rect on a given monitor pass. Recorded even
// for leaves with zero tabs so an empty pane (freshly split, nothing opened
// in it yet) can still be targeted by directional pane selection and have a
// highlight/tab-bar window placed against it.
struct Rect {
  int x = 0, y = 0, w = 0, h = 0;
};

struct Node {
  Node* parent = nullptr;
  Orientation orientation = Orientation::Vertical;
  std::vector<std::unique_ptr<Node>> children;
  // Relative space assigned to each child.  This is empty for leaves.
  std::vector<double> weights;
  std::vector<Window> tabs;
  std::size_t active_tab = 0;
  std::unordered_map<std::size_t, Rect> rect_by_monitor;

  bool is_leaf() const { return children.empty(); }
};

struct Workspace {
  std::unique_ptr<Node> root;
  Node* selected_leaf = nullptr;
  Window focused = None;
  LayoutMode mode = LayoutMode::Manual;
  // Tiled windows in master-stack order (front = master); manual mode ignores
  // this and arranges straight from the tree instead.
  std::vector<Window> stack_order;
  std::vector<Window> floating;
};

struct Project {
  std::string path;
  std::array<Workspace, kWorkspaceCount> workspaces;
};

// Per-window state that outlives which workspace/container currently holds
// the window, so it survives being toggled between tiled and floating.
struct WindowState {
  bool floating = false;
  bool fullscreen = false;
  bool pre_fullscreen_floating = false;
  bool maximized = false;
  int float_x = 0, float_y = 0, float_w = 100, float_h = 100;
  int pre_maximize_x = 0, pre_maximize_y = 0, pre_maximize_w = 0, pre_maximize_h = 0;
  std::size_t monitor = 0;
};

struct Monitor {
  int x = 0, y = 0, width = 1, height = 1;
};

struct TrayIcon {
  Window window = None;
  int width = kBarHeight;
  int height = kBarHeight;
  bool mapped = true;
};

struct BarHit {
  int left = 0;
  int right = 0;
  Window window = None;
};

struct DockWindows {
  Window left = None;
  Window bottom = None;
  Window right = None;
  // Off-screen buffers: everything below is composited here first and blitted
  // to the live window in one XCopyArea, so the clear+redraw each refresh
  // never becomes a visible flash the way drawing straight to the window did.
  Pixmap left_buffer = None;
  Pixmap bottom_buffer = None;
  Pixmap right_buffer = None;
  int left_buffer_width = 0, left_buffer_height = 0;
  int bottom_buffer_width = 0, bottom_buffer_height = 0;
  int right_buffer_width = 0, right_buffer_height = 0;
};

struct WidgetHit {
  int left = 0;
  int right = 0;
  std::string id;
};

// One hint chip in a Vimium-style "click anything" overlay: a root-window
// position/size for its label chip, plus a synthetic click (window + local
// coordinates) replayed through the normal click handlers on selection so
// hint targets stay in lockstep with real click behavior.
struct Hint {
  std::string label;
  int x = 0, y = 0;
  int width = 0, height = 0;
  Window target = None;
  int click_x = 0, click_y = 0;
  unsigned int button = Button1;
};

struct Notification {
  unsigned int id = 0;
  std::string app;
  std::string summary;
  std::string body;
  bool unread = true;
  std::time_t expires_at = 0;
};

// `line` is the 0-based line number the item was parsed from in TODO.org, so
// it can be located again to flip TODO -> DONE without re-scanning the file.
struct TodoItem {
  std::string text;
  std::size_t line = 0;
};

struct AgentStatus {
  pid_t pid = 0;
  std::string kind;
  std::string status;
  std::string label;
  std::string cwd;
  std::string file_path;
  bool from_file = false;
  bool needs_input = false;
};

struct WifiNetwork { std::string ssid; bool secured = false; bool active = false; };
struct BluetoothDevice { std::string mac; std::string name; bool connected = false; };

struct LuaRule {
  std::string class_name;
  std::string instance;
  std::string title;
  int workspace = -1;
  bool floating = false;
  bool set_floating = false;
};

struct LuaKeybind {
  KeyCode keycode = 0;
  unsigned int modifiers = 0;
  int callback = LUA_NOREF;
  std::string spec;
  std::string description;
};
struct LuaMousebind { std::string context; unsigned int button = 0, modifiers = 0; int callback = LUA_NOREF; };
struct LuaWidget { std::string name; std::string text; bool highlight = false; int update = LUA_NOREF; int click = LUA_NOREF; };

struct LauncherApp {
  std::string name;
  std::string exec;
};

int on_x_error(Display*, XErrorEvent* error) {
  if (error->error_code == BadAccess) another_window_manager = 1;
  return 0;
}

// Installed only while we expect a race with a client tearing itself down
// (XKillClient/XGrabServer bracket) so a BadWindow doesn't abort mepwm.
int dummy_x_error(Display*, XErrorEvent*) { return 0; }

// Xlib requires an I/O error handler not to return. The nested X server may be
// closed independently of the manager, so exit quietly instead of printing an
// alarming connection-broken diagnostic in the launching terminal.
int on_x_io_error(Display*) { _exit(0); }

class X11Backend final : public Backend {
 public:
  ~X11Backend() override {
    destroy_ipc();
    if (notification_dbus_) dbus_connection_close(notification_dbus_), dbus_connection_unref(notification_dbus_);
    if (lua_) lua_close(lua_);
    destroy_tray();
    if (bar_font_) {
      XftColorFree(display_, DefaultVisual(display_, screen_), DefaultColormap(display_, screen_), &bar_foreground_);
      XftColorFree(display_, DefaultVisual(display_, screen_), DefaultColormap(display_, screen_), &bar_background_);
      XftColorFree(display_, DefaultVisual(display_, screen_), DefaultColormap(display_, screen_), &bar_selected_);
      XftColorFree(display_, DefaultVisual(display_, screen_), DefaultColormap(display_, screen_), &hint_background_);
      XftColorFree(display_, DefaultVisual(display_, screen_), DefaultColormap(display_, screen_), &hint_foreground_);
      XftColorFree(display_, DefaultVisual(display_, screen_), DefaultColormap(display_, screen_), &hint_matched_);
    }
    for (auto& entry : fallback_fonts_)
      if (entry.second != bar_font_ && entry.second != icon_font_) XftFontClose(display_, entry.second);
    if (bar_xft_draw_) XftDrawDestroy(bar_xft_draw_);
    if (launcher_xft_draw_) XftDrawDestroy(launcher_xft_draw_);
    if (bar_font_) XftFontClose(display_, bar_font_);
    if (icon_font_) XftFontClose(display_, icon_font_);
    if (hint_font_ && hint_font_ != bar_font_) XftFontClose(display_, hint_font_);
    if (bar_pixmap_) XFreePixmap(display_, bar_pixmap_);
    if (launcher_pixmap_) XFreePixmap(display_, launcher_pixmap_);
    for (const DockWindows& dock : docks_) {
      if (dock.left_buffer) XFreePixmap(display_, dock.left_buffer);
      if (dock.bottom_buffer) XFreePixmap(display_, dock.bottom_buffer);
      if (dock.right_buffer) XFreePixmap(display_, dock.right_buffer);
    }
    if (bar_gc_) XFreeGC(display_, bar_gc_);
    if (cursor_) XFreeCursor(display_, cursor_);
    if (display_) XCloseDisplay(display_);
  }

  int run(const Config& config) override {
    StartupTimer startup;
    startup_timer_ = &startup;
    config_ = config;
    display_ = XOpenDisplay(nullptr);
    if (!display_) throw std::runtime_error("could not open the X display");
    startup.checkpoint("open display");

    screen_ = DefaultScreen(display_);
    root_ = RootWindow(display_, screen_);
    XSetIOErrorHandler(on_x_io_error);
    cursor_ = XCreateFontCursor(display_, XC_left_ptr);
    XDefineCursor(display_, root_, cursor_);
    another_window_manager = 0;
    XSetErrorHandler(on_x_error);
    XSelectInput(display_, root_, SubstructureRedirectMask | SubstructureNotifyMask |
                                      StructureNotifyMask | ButtonPressMask);
    XSync(display_, False);
    if (another_window_manager) {
      throw std::runtime_error("another window manager is already running on this display");
    }
    startup.checkpoint("claim window-manager ownership");

    border_normal_pixel_ = alloc_color(config_.border_color_normal);
    border_focused_pixel_ = alloc_color(config_.border_color_focused);
    update_monitors();
    startup.checkpoint("configure display and monitors");

    create_bar();
    startup.checkpoint("create top bar");
    create_docks();
    startup.checkpoint("create sidebars");
    setup_ewmh();
    startup.checkpoint("set up EWMH");
    create_tray();
    startup.checkpoint("create system tray");
    initialize_notification_dbus();
    startup.checkpoint("initialize notification D-Bus");
    initialize_lua();
    startup.checkpoint("initialize Lua");
    refresh_wallpaper();
    startup.checkpoint("set initial wallpaper");
    initialize_projects();
    create_ipc();
    startup.checkpoint("create IPC socket");
    grab_keys();
    startup.checkpoint("grab keys");
    adopt_existing_windows();
    startup.checkpoint("adopt existing windows");
    arrange();
    startup.checkpoint("initial layout and render");
    std::cerr << "mepwm: managing X display " << DisplayString(display_) << '\n';
    startup.checkpoint("ready");
    startup_timer_ = nullptr;
    defer_widget_refresh_ = false;

    while (running_) {
      pollfd fds[] = {{ConnectionNumber(display_), POLLIN, 0}, {ipc_fd_, POLLIN, 0}};
      const int count = ipc_fd_ >= 0 ? 2 : 1;
      const int ready = poll(fds, count, 1000);
      if (ready < 0 && errno != EINTR) break;
      if (ready == 0) {
        draw_bar();
        draw_docks();
        if (side_panel_ == SidePanel::Todos) draw_side_panel();
      }
      process_notification_dbus();
      if (ipc_fd_ >= 0 && fds[1].revents & POLLIN) handle_ipc_client();
      while (XPending(display_)) {
        XEvent event;
        XNextEvent(display_, &event);
        dispatch(event);
      }
    }
    return 0;
  }

 private:
  Workspace& workspace() { return workspaces_[current_workspace_]; }

  static std::string project_state_path() {
    if (const char* data_home = std::getenv("XDG_DATA_HOME"); data_home && *data_home)
      return std::string(data_home) + "/mepwm/projects";
    if (const char* home = std::getenv("HOME"); home && *home)
      return std::string(home) + "/.local/share/mepwm/projects";
    return {};
  }

  static std::string project_label(const std::string& path) {
    const char* home = std::getenv("HOME");
    if (home && path == home) return "default";
    const std::filesystem::path value(path);
    const std::string label = value.filename().string();
    return label.empty() ? path : label;
  }

  void save_projects() const {
    const std::string state_path = project_state_path();
    if (state_path.empty()) return;
    std::error_code error;
    std::filesystem::create_directories(std::filesystem::path(state_path).parent_path(), error);
    std::ofstream file(state_path);
    if (!file) return;
    for (const Project& project : projects_) file << project.path << '\n';
  }

  static bool normalize_project_path(const std::string& raw_path, std::string* normalized) {
    if (raw_path.empty()) return false;
    std::filesystem::path path(raw_path);
    if (raw_path == "~" || raw_path.rfind("~/", 0) == 0) {
      const char* home = std::getenv("HOME");
      if (!home || !*home) return false;
      path = std::filesystem::path(home) / raw_path.substr(raw_path == "~" ? 1 : 2);
    }
    std::error_code error;
    path = std::filesystem::weakly_canonical(path, error);
    if (error || !std::filesystem::is_directory(path, error)) return false;
    *normalized = path.string();
    return true;
  }

  bool add_project(const std::string& raw_path, bool save = true) {
    std::string normalized;
    if (!normalize_project_path(raw_path, &normalized)) return false;
    if (std::any_of(projects_.begin(), projects_.end(),
                    [&](const Project& project) { return project.path == normalized; }))
      return true;
    projects_.push_back({normalized, {}});
    if (save) save_projects();
    return true;
  }

  void initialize_projects() {
    const std::string state_path = project_state_path();
    std::ifstream file(state_path);
    for (std::string path; std::getline(file, path);) add_project(path, false);
    if (const char* home = std::getenv("HOME"); home && *home) add_project(home, false);
    if (projects_.empty()) {
      std::error_code error;
      add_project(std::filesystem::current_path(error).string(), false);
    }
    if (!projects_.empty()) save_projects();
  }

  bool project_has_clients(std::size_t index) const {
    if (index == active_project_index_) {
      for (const Workspace& value : workspaces_)
        if (value.root || !value.floating.empty()) return true;
      return false;
    }
    if (index >= projects_.size()) return false;
    for (const Workspace& value : projects_[index].workspaces)
      if (value.root || !value.floating.empty()) return true;
    return false;
  }

  void switch_project(std::size_t index) {
    if (index >= projects_.size()) return;
    if (index == active_project_index_) {
      switch_workspace(0);
      return;
    }
    hide_workspace(workspace());
    projects_[active_project_index_].workspaces = std::move(workspaces_);
    active_project_index_ = index;
    workspaces_ = std::move(projects_[active_project_index_].workspaces);
    current_workspace_ = 0;
    arrange();
    if (workspace().focused != None) focus(workspace().focused);
    else {
      XSetInputFocus(display_, root_, RevertToPointerRoot, CurrentTime);
      set_active_window(None);
    }
    save_projects();
  }

  const Monitor& monitor(std::size_t index) const { return monitors_[std::min(index, monitors_.size() - 1)]; }

  bool contains(const Monitor& candidate, int x, int y) const {
    return x >= candidate.x && y >= candidate.y && x < candidate.x + candidate.width &&
           y < candidate.y + candidate.height;
  }

  std::size_t monitor_at(int x, int y) const {
    for (std::size_t index = 0; index < monitors_.size(); ++index) {
      if (contains(monitors_[index], x, y)) return index;
    }
    return current_monitor_;
  }

  void update_monitors() {
    std::vector<Monitor> discovered;
    if (XineramaIsActive(display_)) {
      int count = 0;
      XineramaScreenInfo* screens = XineramaQueryScreens(display_, &count);
      for (int index = 0; screens && index < count; ++index) {
        Monitor candidate{screens[index].x_org, screens[index].y_org, screens[index].width,
                          screens[index].height};
        const bool duplicate = std::any_of(discovered.begin(), discovered.end(), [&](const Monitor& existing) {
          return existing.x == candidate.x && existing.y == candidate.y &&
                 existing.width == candidate.width && existing.height == candidate.height;
        });
        if (!duplicate && candidate.width > 0 && candidate.height > 0) discovered.push_back(candidate);
      }
      if (screens) XFree(screens);
    }
    if (discovered.empty()) discovered.push_back({0, 0, DisplayWidth(display_, screen_), DisplayHeight(display_, screen_)});

    monitors_ = std::move(discovered);
    current_monitor_ = std::min(current_monitor_, monitors_.size() - 1);
    for (auto& entry : window_state_) entry.second.monitor = std::min(entry.second.monitor, monitors_.size() - 1);
  }

  // Xephyr can change the nested root size when its host window is resized.
  // Refresh this before laying out a workspace as well as on ConfigureNotify:
  // a workspace switch must never reuse geometry captured before that resize.
  void refresh_display_geometry() {
    update_monitors();
    if (bar_) {
      const int width = std::max(1, DisplayWidth(display_, screen_));
      XMoveResizeWindow(display_, bar_, 0, 0, width, kBarHeight);
    }
    create_docks();
  }

  void send_to_monitor(Window window, std::size_t destination) {
    if (window == None || destination >= monitors_.size()) return;
    WindowState& state = window_state_[window];
    if (state.monitor == destination) return;
    state.monitor = destination;
    if (state.floating && !state.fullscreen) {
      const Monitor& target = monitor(destination);
      state.float_x = std::clamp(state.float_x, target.x,
                                 std::max(target.x, target.x + target.width - state.float_w));
      state.float_y = std::clamp(state.float_y, target.y + kBarHeight,
                                 std::max(target.y + kBarHeight,
                                          target.y + target.height - state.float_h));
    }
    arrange();
  }

  void focus_monitor(int delta) {
    if (monitors_.size() < 2) return;
    const int count = static_cast<int>(monitors_.size());
    current_monitor_ = static_cast<std::size_t>((static_cast<int>(current_monitor_) + delta + count) % count);
    std::vector<Window> windows;
    collect_windows(workspace().root.get(), windows);
    windows.insert(windows.end(), workspace().floating.begin(), workspace().floating.end());
    const auto found = std::find_if(windows.begin(), windows.end(), [&](Window window) {
      return window_state_[window].monitor == current_monitor_;
    });
    if (found != windows.end()) focus(*found);
    draw_bar();
  }

  static std::unique_ptr<Node> make_leaf(Node* parent = nullptr) {
    auto leaf = std::make_unique<Node>();
    leaf->parent = parent;
    return leaf;
  }

  unsigned long alloc_color(const std::string& spec) {
    XColor color;
    Colormap colormap = DefaultColormap(display_, screen_);
    if (XAllocNamedColor(display_, colormap, spec.c_str(), &color, &color)) return color.pixel;
    return BlackPixel(display_, screen_);
  }

  void initialize_notification_dbus() {
    DBusError error;
    dbus_error_init(&error);
    notification_dbus_ = dbus_bus_get_private(DBUS_BUS_SESSION, &error);
    if (dbus_error_is_set(&error) || !notification_dbus_) {
      if (dbus_error_is_set(&error)) dbus_error_free(&error);
      return;
    }
    dbus_connection_set_exit_on_disconnect(notification_dbus_, FALSE);
    const int result = dbus_bus_request_name(notification_dbus_, "org.freedesktop.Notifications",
                                             DBUS_NAME_FLAG_DO_NOT_QUEUE, &error);
    notification_dbus_owned_ = result == DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER;
    if (dbus_error_is_set(&error)) dbus_error_free(&error);
  }

  void try_acquire_notification_name() {
    if (!notification_dbus_ || notification_dbus_owned_) return;
    DBusError error;
    dbus_error_init(&error);
    const int result = dbus_bus_request_name(notification_dbus_, "org.freedesktop.Notifications",
                                             DBUS_NAME_FLAG_DO_NOT_QUEUE, &error);
    notification_dbus_owned_ = result == DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER;
    if (dbus_error_is_set(&error)) dbus_error_free(&error);
  }

  void notification_reply_empty(DBusMessage* message) {
    DBusMessage* reply = dbus_message_new_method_return(message);
    if (reply) dbus_connection_send(notification_dbus_, reply, nullptr), dbus_message_unref(reply);
  }

  void emit_notification_closed(unsigned int id, unsigned int reason) {
    if (!notification_dbus_ || !notification_dbus_owned_) return;
    DBusMessage* signal = dbus_message_new_signal("/org/freedesktop/Notifications",
                                                  "org.freedesktop.Notifications", "NotificationClosed");
    if (!signal) return;
    dbus_uint32_t notification_id = id;
    dbus_uint32_t close_reason = reason;
    dbus_message_append_args(signal, DBUS_TYPE_UINT32, &notification_id, DBUS_TYPE_UINT32, &close_reason,
                             DBUS_TYPE_INVALID);
    dbus_connection_send(notification_dbus_, signal, nullptr);
    dbus_message_unref(signal);
  }

  void handle_notification_dbus(DBusMessage* message) {
    const char* interface = dbus_message_get_interface(message);
    const char* member = dbus_message_get_member(message);
    if (!interface || !member) return;
    if (std::strcmp(interface, DBUS_INTERFACE_INTROSPECTABLE) == 0 && std::strcmp(member, "Introspect") == 0) {
      static const char xml[] =
          "<node><interface name='org.freedesktop.Notifications'>"
          "<method name='Notify'><arg type='s' direction='in'/><arg type='u' direction='in'/><arg type='s' direction='in'/>"
          "<arg type='s' direction='in'/><arg type='s' direction='in'/><arg type='as' direction='in'/><arg type='a{sv}' direction='in'/>"
          "<arg type='i' direction='in'/><arg type='u' direction='out'/></method>"
          "<method name='CloseNotification'><arg type='u' direction='in'/></method>"
          "<method name='GetCapabilities'><arg type='as' direction='out'/></method>"
          "<method name='GetServerInformation'><arg type='s' direction='out'/><arg type='s' direction='out'/><arg type='s' direction='out'/><arg type='s' direction='out'/></method>"
          "<signal name='NotificationClosed'><arg type='u'/><arg type='u'/></signal></interface></node>";
      DBusMessage* reply = dbus_message_new_method_return(message);
      if (reply) {
        const char* result = xml;
        dbus_message_append_args(reply, DBUS_TYPE_STRING, &result, DBUS_TYPE_INVALID);
        dbus_connection_send(notification_dbus_, reply, nullptr);
        dbus_message_unref(reply);
      }
    } else if (std::strcmp(interface, "org.freedesktop.Notifications") == 0 && std::strcmp(member, "Notify") == 0) {
      DBusMessageIter iterator;
      if (!dbus_message_iter_init(message, &iterator)) return;
      const char* app = "";
      const char* summary = "";
      const char* body = "";
      dbus_uint32_t replaces_id = 0;
      if (dbus_message_iter_get_arg_type(&iterator) == DBUS_TYPE_STRING) dbus_message_iter_get_basic(&iterator, &app);
      dbus_message_iter_next(&iterator);
      if (dbus_message_iter_get_arg_type(&iterator) == DBUS_TYPE_UINT32) dbus_message_iter_get_basic(&iterator, &replaces_id);
      dbus_message_iter_next(&iterator);  // icon
      dbus_message_iter_next(&iterator);
      if (dbus_message_iter_get_arg_type(&iterator) == DBUS_TYPE_STRING) dbus_message_iter_get_basic(&iterator, &summary);
      dbus_message_iter_next(&iterator);
      if (dbus_message_iter_get_arg_type(&iterator) == DBUS_TYPE_STRING) dbus_message_iter_get_basic(&iterator, &body);
      dbus_message_iter_next(&iterator);  // actions
      dbus_message_iter_next(&iterator);  // hints
      dbus_int32_t timeout_ms = -1;
      dbus_message_iter_next(&iterator);
      if (dbus_message_iter_get_arg_type(&iterator) == DBUS_TYPE_INT32) dbus_message_iter_get_basic(&iterator, &timeout_ms);
      const unsigned int id = replaces_id ? replaces_id : next_notification_id_++;
      auto existing = std::find_if(notifications_.begin(), notifications_.end(), [id](const Notification& item) {
        return item.id == id;
      });
      // A zero timeout asks the server to choose a reasonable default.  Keep
      // those notices visible for ten seconds; negative values are persistent.
      const std::time_t expires_at = timeout_ms < 0 ? 0 : std::time(nullptr) + std::max(1, timeout_ms == 0 ? 10000 : timeout_ms) / 1000;
      Notification notification{id, app ? app : "", summary ? summary : "", body ? body : "", true, expires_at};
      if (existing == notifications_.end()) notifications_.push_back(std::move(notification));
      else *existing = std::move(notification);
      DBusMessage* reply = dbus_message_new_method_return(message);
      if (reply) {
        dbus_uint32_t reply_id = id;
        dbus_message_append_args(reply, DBUS_TYPE_UINT32, &reply_id, DBUS_TYPE_INVALID);
        dbus_connection_send(notification_dbus_, reply, nullptr);
        dbus_message_unref(reply);
      }
      if (side_panel_ == SidePanel::Notifications) draw_side_panel();
      draw_docks();
    } else if (std::strcmp(interface, "org.freedesktop.Notifications") == 0 && std::strcmp(member, "CloseNotification") == 0) {
      dbus_uint32_t id = 0;
      if (dbus_message_get_args(message, nullptr, DBUS_TYPE_UINT32, &id, DBUS_TYPE_INVALID)) {
        notifications_.erase(std::remove_if(notifications_.begin(), notifications_.end(), [id](const Notification& item) {
          return item.id == id;
        }), notifications_.end());
        emit_notification_closed(id, 3);
      }
      notification_reply_empty(message);
      if (side_panel_ == SidePanel::Notifications) draw_side_panel();
      draw_docks();
    } else if (std::strcmp(interface, "org.freedesktop.Notifications") == 0 && std::strcmp(member, "GetCapabilities") == 0) {
      DBusMessage* reply = dbus_message_new_method_return(message);
      if (!reply) return;
      DBusMessageIter outer, array;
      dbus_message_iter_init_append(reply, &outer);
      dbus_message_iter_open_container(&outer, DBUS_TYPE_ARRAY, "s", &array);
      const char* capabilities[] = {"body"};
      for (const char* capability : capabilities) dbus_message_iter_append_basic(&array, DBUS_TYPE_STRING, &capability);
      dbus_message_iter_close_container(&outer, &array);
      dbus_connection_send(notification_dbus_, reply, nullptr);
      dbus_message_unref(reply);
    } else if (std::strcmp(interface, "org.freedesktop.Notifications") == 0 && std::strcmp(member, "GetServerInformation") == 0) {
      DBusMessage* reply = dbus_message_new_method_return(message);
      const char* name = "mepwm"; const char* vendor = "MEP"; const char* version = "0.1.0"; const char* specification = "1.2";
      if (reply) {
        dbus_message_append_args(reply, DBUS_TYPE_STRING, &name, DBUS_TYPE_STRING, &vendor, DBUS_TYPE_STRING, &version,
                                 DBUS_TYPE_STRING, &specification, DBUS_TYPE_INVALID);
        dbus_connection_send(notification_dbus_, reply, nullptr);
        dbus_message_unref(reply);
      }
    } else if (dbus_message_get_type(message) == DBUS_MESSAGE_TYPE_METHOD_CALL) {
      DBusMessage* reply = dbus_message_new_error(message, DBUS_ERROR_UNKNOWN_METHOD, "mepwm: method not implemented");
      if (reply) dbus_connection_send(notification_dbus_, reply, nullptr), dbus_message_unref(reply);
    }
  }

  void process_notification_dbus() {
    if (!notification_dbus_) return;
    try_acquire_notification_name();
    dbus_connection_read_write(notification_dbus_, 0);
    while (DBusMessage* message = dbus_connection_pop_message(notification_dbus_)) {
      handle_notification_dbus(message);
      dbus_message_unref(message);
    }
  }

  static X11Backend* lua_backend(lua_State* state) {
    lua_getfield(state, LUA_REGISTRYINDEX, "mepwm.backend");
    auto* backend = static_cast<X11Backend*>(lua_touserdata(state, -1));
    lua_pop(state, 1);
    return backend;
  }

  static int lua_set_terminal(lua_State* state) {
    lua_backend(state)->config_.terminal = luaL_checkstring(state, 1);
    return 0;
  }

  static int lua_notify(lua_State* state) {
    X11Backend* backend = lua_backend(state);
    const char* summary = luaL_checkstring(state, 1);
    const char* body = luaL_optstring(state, 2, "");
    backend->notifications_.push_back({backend->next_notification_id_++, "mepwm", summary, body, true});
    if (backend->side_panel_ == SidePanel::Notifications) backend->draw_side_panel();
    backend->draw_docks();
    return 0;
  }

  static int lua_widget(lua_State* state) {
    X11Backend* backend = lua_backend(state);
    luaL_checktype(state, 1, LUA_TTABLE);
    if (backend->lua_widgets_.size() >= 16) return luaL_error(state, "too many widgets (max 16)");
    LuaWidget widget;
    lua_getfield(state, 1, "name");
    widget.name = lua_isstring(state, -1) ? lua_tostring(state, -1) : "widget" + std::to_string(backend->lua_widgets_.size() + 1);
    lua_pop(state, 1);
    widget.text = widget.name;
    lua_getfield(state, 1, "highlight"); widget.highlight = lua_toboolean(state, -1); lua_pop(state, 1);
    lua_getfield(state, 1, "update");
    if (lua_isfunction(state, -1)) widget.update = luaL_ref(state, LUA_REGISTRYINDEX); else lua_pop(state, 1);
    lua_getfield(state, 1, "click");
    if (lua_isfunction(state, -1)) widget.click = luaL_ref(state, LUA_REGISTRYINDEX); else lua_pop(state, 1);
    backend->lua_widgets_.push_back(std::move(widget));
    backend->widgets_refreshed_ = 0;
    backend->draw_docks();
    return 0;
  }

  static int lua_set_mfact(lua_State* state) {
    X11Backend* backend = lua_backend(state);
    backend->config_.mfact = std::clamp(static_cast<float>(luaL_checknumber(state, 1)), 0.05f, 0.95f);
    backend->arrange();
    return 0;
  }

  static int lua_set_unsigned(lua_State* state, unsigned int Config::*member) {
    X11Backend* backend = lua_backend(state);
    backend->config_.*member = static_cast<unsigned int>(std::max<lua_Integer>(0, luaL_checkinteger(state, 1)));
    backend->arrange();
    return 0;
  }

  static int lua_set_nmaster(lua_State* state) { return lua_set_unsigned(state, &Config::nmaster); }
  static int lua_set_snap(lua_State* state) { return lua_set_unsigned(state, &Config::snap); }
  static int lua_set_gaps(lua_State* state) { return lua_set_unsigned(state, &Config::gap); }
  static int lua_set_border_width(lua_State* state) {
    X11Backend* backend = lua_backend(state);
    backend->config_.border_width = static_cast<unsigned int>(std::max<lua_Integer>(0, luaL_checkinteger(state, 1)));
    for (const auto& entry : backend->window_state_) XSetWindowBorderWidth(backend->display_, entry.first, backend->config_.border_width);
    backend->arrange();
    return 0;
  }

  static int lua_rule(lua_State* state) {
    X11Backend* backend = lua_backend(state);
    luaL_checktype(state, 1, LUA_TTABLE);
    LuaRule rule;
    auto string_field = [&](const char* name, std::string* value) {
      lua_getfield(state, 1, name);
      if (lua_isstring(state, -1)) *value = lua_tostring(state, -1);
      lua_pop(state, 1);
    };
    string_field("class", &rule.class_name);
    string_field("instance", &rule.instance);
    string_field("title", &rule.title);
    lua_getfield(state, 1, "workspace");
    if (lua_isnumber(state, -1)) rule.workspace = static_cast<int>(lua_tointeger(state, -1)) - 1;
    lua_pop(state, 1);
    lua_getfield(state, 1, "floating");
    if (lua_isboolean(state, -1)) { rule.set_floating = true; rule.floating = lua_toboolean(state, -1); }
    lua_pop(state, 1);
    if (rule.class_name.empty() && rule.instance.empty() && rule.title.empty())
      return luaL_error(state, "mwm.rule requires class, instance, or title");
    backend->lua_rules_.push_back(std::move(rule));
    return 0;
  }

  static int lua_keybind(lua_State* state) {
    X11Backend* backend = lua_backend(state);
    const std::string spec = luaL_checkstring(state, 1);
    luaL_checktype(state, 2, LUA_TFUNCTION);
    unsigned int modifiers = 0;
    std::string key_name;
    std::size_t start = 0;
    while (start <= spec.size()) {
      const std::size_t end = spec.find('+', start);
      const std::string part = spec.substr(start, end == std::string::npos ? std::string::npos : end - start);
      if (part == "mod4" || part == "super") modifiers |= Mod4Mask;
      else if (part == "shift") modifiers |= ShiftMask;
      else if (part == "ctrl" || part == "control") modifiers |= ControlMask;
      else if (part == "alt" || part == "mod1") modifiers |= Mod1Mask;
      else key_name = part;
      if (end == std::string::npos) break;
      start = end + 1;
    }
    const KeySym symbol = XStringToKeysym(key_name.c_str());
    const KeyCode keycode = symbol == NoSymbol ? 0 : XKeysymToKeycode(backend->display_, symbol);
    if (!keycode) return luaL_error(state, "invalid keybinding: %s", spec.c_str());
    lua_pushvalue(state, 2);
    const int callback = luaL_ref(state, LUA_REGISTRYINDEX);
    const std::string description = luaL_optstring(state, 3, "");
    backend->lua_keybinds_.push_back({keycode, modifiers, callback, spec, description});
    const unsigned int ignored[] = {0, LockMask, Mod2Mask, LockMask | Mod2Mask};
    for (unsigned int mask : ignored)
      XGrabKey(backend->display_, keycode, modifiers | mask, backend->root_, True, GrabModeAsync, GrabModeAsync);
    return 0;
  }

  static int lua_mousebind(lua_State* state) {
    X11Backend* backend = lua_backend(state);
    const std::string context = luaL_checkstring(state, 1);
    const std::string spec = luaL_checkstring(state, 2);
    luaL_checktype(state, 3, LUA_TFUNCTION);
    unsigned int modifiers = 0, button = 0;
    std::size_t start = 0;
    while (start <= spec.size()) {
      const std::size_t end = spec.find('+', start);
      const std::string part = spec.substr(start, end == std::string::npos ? std::string::npos : end - start);
      if (part == "mod4" || part == "super") modifiers |= Mod4Mask;
      else if (part == "shift") modifiers |= ShiftMask;
      else if (part == "ctrl" || part == "control") modifiers |= ControlMask;
      else if (part == "alt" || part == "mod1") modifiers |= Mod1Mask;
      else if (part.rfind("button", 0) == 0) {
        const std::string number = part.substr(6);
        char* end_ptr = nullptr;
        const unsigned long parsed = std::strtoul(number.c_str(), &end_ptr, 10);
        if (number.empty() || *end_ptr != '\0') return luaL_error(state, "invalid mouse binding: %s", spec.c_str());
        button = static_cast<unsigned int>(parsed);
      }
      if (end == std::string::npos) break;
      start = end + 1;
    }
    if (button < Button1 || button > Button5) return luaL_error(state, "invalid mouse binding: %s", spec.c_str());
    lua_pushvalue(state, 3);
    const int callback = luaL_ref(state, LUA_REGISTRYINDEX);
    backend->lua_mousebinds_.push_back({context, button, modifiers, callback});
    Window target = context == "root" ? backend->root_ : backend->bar_;
    XGrabButton(backend->display_, button, modifiers, target, False, ButtonPressMask, GrabModeAsync, GrabModeAsync, None, None);
    if (context == "client") {
      for (const auto& entry : backend->window_state_)
        XGrabButton(backend->display_, button, modifiers, entry.first, False, ButtonPressMask,
                    GrabModeAsync, GrabModeAsync, None, None);
    }
    return 0;
  }

  static int lua_exec(lua_State* state) {
    lua_pushstring(state, capture_command(luaL_checkstring(state, 1)).c_str());
    return 1;
  }

  static int lua_zoom(lua_State* state) { lua_backend(state)->zoom(); return 0; }
  static int lua_toggle_floating(lua_State* state) { lua_backend(state)->toggle_floating(); return 0; }
  static int lua_kill_client(lua_State* state) { lua_backend(state)->kill_focused(); return 0; }
  static int lua_scratchpad_set(lua_State* state) { lua_backend(state)->set_scratchpad(); return 0; }
  static int lua_scratchpad_toggle(lua_State* state) { lua_backend(state)->toggle_scratchpad(); return 0; }
  static int lua_toggle_bar(lua_State* state) { lua_backend(state)->toggle_bar(); return 0; }

  static int lua_set_layout(lua_State* state) {
    X11Backend* backend = lua_backend(state);
    std::string name = luaL_checkstring(state, 1);
    for (char& letter : name) letter = static_cast<char>(std::tolower(static_cast<unsigned char>(letter)));
    LayoutMode mode;
    if (name == "tile" || name == "master" || name == "masterstack" || name == "master-stack") mode = LayoutMode::MasterStack;
    else if (name == "monocle") mode = LayoutMode::Monocle;
    else if (name == "manual" || name == "floating") mode = LayoutMode::Manual;
    else return luaL_error(state, "unknown layout: %s (expected tile, monocle, or manual)", name.c_str());
    backend->workspace().mode = mode;
    backend->arrange();
    return 0;
  }

  static int lua_cycle_layout(lua_State* state) {
    X11Backend* backend = lua_backend(state);
    if (luaL_optinteger(state, 1, 1) < 0) backend->cycle_layout_reverse(); else backend->cycle_layout();
    return 0;
  }

  static int lua_list_projects(lua_State* state) {
    X11Backend* backend = lua_backend(state);
    lua_newtable(state);
    for (std::size_t index = 0; index < backend->projects_.size(); ++index) {
      lua_pushstring(state, backend->projects_[index].path.c_str());
      lua_rawseti(state, -2, static_cast<int>(index + 1));
    }
    return 1;
  }

  static int lua_current_project(lua_State* state) {
    X11Backend* backend = lua_backend(state);
    if (backend->active_project_index_ >= backend->projects_.size()) { lua_pushnil(state); return 1; }
    lua_pushstring(state, backend->projects_[backend->active_project_index_].path.c_str());
    return 1;
  }

  static int lua_switch_project(lua_State* state) {
    X11Backend* backend = lua_backend(state);
    const std::string raw_path = luaL_checkstring(state, 1);
    std::string normalized;
    if (!normalize_project_path(raw_path, &normalized)) return luaL_error(state, "not a directory: %s", raw_path.c_str());
    backend->add_project(normalized);
    const auto it = std::find_if(backend->projects_.begin(), backend->projects_.end(),
                                 [&](const Project& project) { return project.path == normalized; });
    if (it == backend->projects_.end()) return 0;
    backend->switch_project(static_cast<std::size_t>(it - backend->projects_.begin()));
    backend->spawn_terminal_in(normalized);
    return 0;
  }

  static int lua_agent_command(lua_State* state) {
    lua_backend(state)->config_.agent_command = luaL_checkstring(state, 1);
    return 0;
  }

  // mwm.set_wallpapers(light_dir, dark_dir): directories scanned for a
  // random image (feh --bg-fill) on startup and whenever the active theme's
  // background luminance crosses the light/dark threshold (see
  // theme_is_light()/refresh_wallpaper()).
  static int lua_set_wallpapers(lua_State* state) {
    X11Backend* backend = lua_backend(state);
    backend->config_.wallpaper_dir_light = luaL_checkstring(state, 1);
    backend->config_.wallpaper_dir_dark = luaL_checkstring(state, 2);
    return 0;
  }

  // Adapted from mwm's mwm.theme({topbar={normal=,selected=},...}) to
  // mep-wm's simpler palette-cycling bar (fg/bg/selected, no separate
  // light/dark tables): mwm.theme({name=, fg=, bg=, selected=}) adds a new
  // cyclable theme, or replaces the built-in of the same name.
  static int lua_theme(lua_State* state) {
    X11Backend* backend = lua_backend(state);
    luaL_checktype(state, 1, LUA_TTABLE);
    std::array<std::string, 3> palette = backend->theme_palettes_.empty()
        ? std::array<std::string, 3>{"#f8f8f2", "#202124", "#5294e2"} : backend->theme_palettes_[0];
    std::string name = "theme" + std::to_string(backend->theme_palettes_.size() + 1);
    auto string_field = [&](const char* field, std::string* out) {
      lua_getfield(state, 1, field);
      if (lua_isstring(state, -1)) *out = lua_tostring(state, -1);
      lua_pop(state, 1);
    };
    string_field("name", &name);
    string_field("fg", &palette[0]);
    string_field("bg", &palette[1]);
    string_field("selected", &palette[2]);
    const auto it = std::find(backend->theme_names_.begin(), backend->theme_names_.end(), name);
    if (it != backend->theme_names_.end()) {
      const std::size_t index = static_cast<std::size_t>(it - backend->theme_names_.begin());
      backend->theme_palettes_[index] = palette;
      if (static_cast<int>(index) == backend->theme_index_) backend->apply_current_theme();
    } else {
      backend->theme_names_.push_back(name);
      backend->theme_palettes_.push_back(palette);
    }
    return 0;
  }

  void initialize_lua() {
    if (lua_) lua_close(lua_);
    lua_ = luaL_newstate();
    if (!lua_) throw std::runtime_error("could not create Lua state");
    luaL_openlibs(lua_);
    lua_pushlightuserdata(lua_, this);
    lua_setfield(lua_, LUA_REGISTRYINDEX, "mepwm.backend");
    lua_newtable(lua_);
    lua_pushcfunction(lua_, lua_set_terminal); lua_setfield(lua_, -2, "set_terminal");
    lua_pushcfunction(lua_, lua_notify); lua_setfield(lua_, -2, "notify");
    lua_pushcfunction(lua_, lua_widget); lua_setfield(lua_, -2, "widget");
    lua_pushcfunction(lua_, lua_set_mfact); lua_setfield(lua_, -2, "set_mfact");
    lua_pushcfunction(lua_, lua_set_nmaster); lua_setfield(lua_, -2, "set_nmaster");
    lua_pushcfunction(lua_, lua_set_snap); lua_setfield(lua_, -2, "set_snap");
    lua_pushcfunction(lua_, lua_set_gaps); lua_setfield(lua_, -2, "set_gaps");
    lua_pushcfunction(lua_, lua_set_border_width); lua_setfield(lua_, -2, "set_border_width");
    lua_pushcfunction(lua_, lua_rule); lua_setfield(lua_, -2, "rule");
    lua_pushcfunction(lua_, lua_keybind); lua_setfield(lua_, -2, "keybind");
    lua_pushcfunction(lua_, lua_mousebind); lua_setfield(lua_, -2, "mousebind");
    lua_pushcfunction(lua_, lua_exec); lua_setfield(lua_, -2, "exec");
    lua_pushcfunction(lua_, lua_zoom); lua_setfield(lua_, -2, "zoom");
    lua_pushcfunction(lua_, lua_toggle_floating); lua_setfield(lua_, -2, "toggle_floating");
    lua_pushcfunction(lua_, lua_kill_client); lua_setfield(lua_, -2, "kill_client");
    lua_pushcfunction(lua_, lua_scratchpad_set); lua_setfield(lua_, -2, "scratchpad_set");
    lua_pushcfunction(lua_, lua_scratchpad_toggle); lua_setfield(lua_, -2, "scratchpad_toggle");
    lua_pushcfunction(lua_, lua_toggle_bar); lua_setfield(lua_, -2, "toggle_bar");
    lua_pushcfunction(lua_, lua_set_layout); lua_setfield(lua_, -2, "set_layout");
    lua_pushcfunction(lua_, lua_cycle_layout); lua_setfield(lua_, -2, "cycle_layout");
    lua_pushcfunction(lua_, lua_list_projects); lua_setfield(lua_, -2, "list_projects");
    lua_pushcfunction(lua_, lua_current_project); lua_setfield(lua_, -2, "current_project");
    lua_pushcfunction(lua_, lua_switch_project); lua_setfield(lua_, -2, "switch_project");
    lua_pushcfunction(lua_, lua_agent_command); lua_setfield(lua_, -2, "agent_command");
    lua_pushcfunction(lua_, lua_theme); lua_setfield(lua_, -2, "theme");
    lua_pushcfunction(lua_, lua_set_wallpapers); lua_setfield(lua_, -2, "set_wallpapers");
    lua_setglobal(lua_, "mwm");
    const char* home = std::getenv("HOME");
    if (home) {
      const std::string config_path = std::string(home) + "/.config/mep-wm/config.lua";
      if (access(config_path.c_str(), R_OK) == 0 && luaL_dofile(lua_, config_path.c_str()) != LUA_OK) {
        std::cerr << "mepwm: Lua config error: " << lua_tostring(lua_, -1) << '\n';
        lua_pop(lua_, 1);
      }
    }
  }

  void reload_lua() {
    reset_theme_palettes();
    lua_rules_.clear();
    lua_keybinds_.clear();
    // Unconditional, even though the vector is about to be emptied anyway:
    // a reload that registers fewer (or zero) mousebinds than before must
    // still drop the old X-server grabs, or they outlive the C++-side
    // bindings that would have handled them.
    XUngrabButton(display_, AnyButton, AnyModifier, root_);
    XUngrabButton(display_, AnyButton, AnyModifier, bar_);
    for (const auto& entry : window_state_) XUngrabButton(display_, AnyButton, AnyModifier, entry.first);
    lua_mousebinds_.clear();
    lua_widgets_.clear();
    initialize_lua();
    apply_current_theme();
    XUngrabKey(display_, AnyKey, AnyModifier, root_);
    grab_keys();
    arrange();
    std::cerr << "mepwm: reloaded Lua configuration\n";
  }

  void create_ipc() {
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    const char* display_name = std::getenv("DISPLAY");
    ipc_path_ = std::string(runtime && *runtime ? runtime : "/tmp") + "/mep-wm-" +
                (display_name && *display_name ? display_name : "display") + ".sock";
    if (ipc_path_.size() >= sizeof(((sockaddr_un*)nullptr)->sun_path)) return;
    ipc_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ipc_fd_ < 0) return;
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::strcpy(address.sun_path, ipc_path_.c_str());
    unlink(ipc_path_.c_str());
    if (bind(ipc_fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || listen(ipc_fd_, 8) != 0) {
      close(ipc_fd_); ipc_fd_ = -1; unlink(ipc_path_.c_str());
    }
  }

  void destroy_ipc() {
    if (ipc_fd_ >= 0) close(ipc_fd_);
    if (!ipc_path_.empty()) unlink(ipc_path_.c_str());
  }

  void handle_ipc_client() {
    const int client = accept(ipc_fd_, nullptr, nullptr);
    if (client < 0) return;
    std::string source;
    char buffer[4096];
    ssize_t count;
    while ((count = read(client, buffer, sizeof(buffer))) > 0) source.append(buffer, count);
    if (source.size() > 1024 * 1024) { write(client, "ERR request too large\n", 22); close(client); return; }
    if (luaL_loadbuffer(lua_, source.data(), source.size(), "mep-wm-cli") != LUA_OK || lua_pcall(lua_, 0, LUA_MULTRET, 0) != LUA_OK) {
      const std::string error = std::string("ERR ") + lua_tostring(lua_, -1) + "\n";
      write(client, error.data(), error.size()); lua_pop(lua_, 1); close(client); return;
    }
    const int results = lua_gettop(lua_);
    for (int index = 1; index <= results; ++index) {
      size_t length; const char* value = luaL_tolstring(lua_, index, &length);
      write(client, value, length); write(client, "\n", 1); lua_pop(lua_, 1);
    }
    lua_settop(lua_, 0);
    close(client);
  }

  // {{{ EWMH

  void setup_ewmh() {
    net_supported_atom_ = XInternAtom(display_, "_NET_SUPPORTED", False);
    net_supporting_wm_check_atom_ = XInternAtom(display_, "_NET_SUPPORTING_WM_CHECK", False);
    net_wm_name_atom_ = XInternAtom(display_, "_NET_WM_NAME", False);
    utf8_string_atom_ = XInternAtom(display_, "UTF8_STRING", False);
    net_active_window_atom_ = XInternAtom(display_, "_NET_ACTIVE_WINDOW", False);
    net_client_list_atom_ = XInternAtom(display_, "_NET_CLIENT_LIST", False);
    net_wm_state_atom_ = XInternAtom(display_, "_NET_WM_STATE", False);
    net_wm_state_fullscreen_atom_ = XInternAtom(display_, "_NET_WM_STATE_FULLSCREEN", False);
    net_wm_window_type_atom_ = XInternAtom(display_, "_NET_WM_WINDOW_TYPE", False);
    net_wm_window_type_dialog_atom_ = XInternAtom(display_, "_NET_WM_WINDOW_TYPE_DIALOG", False);
    net_wm_pid_atom_ = XInternAtom(display_, "_NET_WM_PID", False);
    wm_protocols_atom_ = XInternAtom(display_, "WM_PROTOCOLS", False);
    wm_delete_window_atom_ = XInternAtom(display_, "WM_DELETE_WINDOW", False);
    manager_atom_ = XInternAtom(display_, "MANAGER", False);
    const std::string tray_selection = "_NET_SYSTEM_TRAY_S" + std::to_string(screen_);
    net_system_tray_atom_ = XInternAtom(display_, tray_selection.c_str(), False);
    net_system_tray_opcode_atom_ = XInternAtom(display_, "_NET_SYSTEM_TRAY_OPCODE", False);
    net_system_tray_orientation_atom_ = XInternAtom(display_, "_NET_SYSTEM_TRAY_ORIENTATION", False);
    xembed_atom_ = XInternAtom(display_, "_XEMBED", False);
    xembed_info_atom_ = XInternAtom(display_, "_XEMBED_INFO", False);

    wm_check_window_ = XCreateSimpleWindow(display_, root_, -1, -1, 1, 1, 0, 0, 0);
    XChangeProperty(display_, wm_check_window_, net_supporting_wm_check_atom_, XA_WINDOW, 32,
                     PropModeReplace, reinterpret_cast<unsigned char*>(&wm_check_window_), 1);
    XChangeProperty(display_, wm_check_window_, net_wm_name_atom_, utf8_string_atom_, 8,
                     PropModeReplace, reinterpret_cast<const unsigned char*>("mepwm"), 5);
    XChangeProperty(display_, root_, net_supporting_wm_check_atom_, XA_WINDOW, 32, PropModeReplace,
                     reinterpret_cast<unsigned char*>(&wm_check_window_), 1);

    const Atom supported[] = {
        net_wm_name_atom_,          net_active_window_atom_,        net_client_list_atom_,
        net_wm_state_atom_,         net_wm_state_fullscreen_atom_,  net_supporting_wm_check_atom_,
        net_wm_window_type_atom_,   net_wm_window_type_dialog_atom_,
    };
    XChangeProperty(display_, root_, net_supported_atom_, XA_ATOM, 32, PropModeReplace,
                     reinterpret_cast<const unsigned char*>(supported),
                     static_cast<int>(sizeof(supported) / sizeof(Atom)));

    XDeleteProperty(display_, root_, net_client_list_atom_);
  }

  void add_to_client_list(Window window) {
    XChangeProperty(display_, root_, net_client_list_atom_, XA_WINDOW, 32, PropModeAppend,
                     reinterpret_cast<unsigned char*>(&window), 1);
  }

  void rebuild_client_list() {
    std::vector<Window> all;
    for (Workspace& ws : workspaces_) {
      collect_windows(ws.root.get(), all);
      all.insert(all.end(), ws.floating.begin(), ws.floating.end());
    }
    for (Project& project : projects_) {
      if (&project == &projects_[active_project_index_]) continue;
      for (Workspace& ws : project.workspaces) {
        collect_windows(ws.root.get(), all);
        all.insert(all.end(), ws.floating.begin(), ws.floating.end());
      }
    }
    XChangeProperty(display_, root_, net_client_list_atom_, XA_WINDOW, 32, PropModeReplace,
                     reinterpret_cast<unsigned char*>(all.data()), static_cast<int>(all.size()));
  }

  void set_active_window(Window window) {
    XChangeProperty(display_, root_, net_active_window_atom_, XA_WINDOW, 32, PropModeReplace,
                     reinterpret_cast<unsigned char*>(&window), 1);
  }

  void set_net_wm_state_fullscreen(Window window, bool enable) {
    Atom value = net_wm_state_fullscreen_atom_;
    if (enable) {
      XChangeProperty(display_, window, net_wm_state_atom_, XA_ATOM, 32, PropModeReplace,
                       reinterpret_cast<unsigned char*>(&value), 1);
    } else {
      Atom empty[1] = {0};
      XChangeProperty(display_, window, net_wm_state_atom_, XA_ATOM, 32, PropModeReplace,
                       reinterpret_cast<unsigned char*>(empty), 0);
    }
  }

  bool is_dialog_window_type(Window window) const {
    Atom actual_type;
    int actual_format;
    unsigned long count = 0, remaining = 0;
    unsigned char* data = nullptr;
    bool result = false;
    if (XGetWindowProperty(display_, window, net_wm_window_type_atom_, 0, 16, False, XA_ATOM,
                           &actual_type, &actual_format, &count, &remaining, &data) == Success &&
        data) {
      Atom* atoms = reinterpret_cast<Atom*>(data);
      for (unsigned long index = 0; index < count; ++index) {
        if (atoms[index] == net_wm_window_type_dialog_atom_) {
          result = true;
          break;
        }
      }
      XFree(data);
    }
    return result;
  }

  bool supports_protocol(Window window, Atom protocol) const {
    Atom* protocols = nullptr;
    int count = 0;
    bool found = false;
    if (XGetWMProtocols(display_, window, &protocols, &count)) {
      for (int index = 0; index < count; ++index) {
        if (protocols[index] == protocol) {
          found = true;
          break;
        }
      }
      XFree(protocols);
    }
    return found;
  }

  void handle_client_message(const XClientMessageEvent& event) {
    if (event.message_type == net_system_tray_opcode_atom_ && event.format == 32 &&
        event.data.l[1] == kSystemTrayRequestDock) {
      dock_tray_icon(static_cast<Window>(event.data.l[2]));
    } else if (event.message_type == net_wm_state_atom_ && event.format == 32) {
      if (find_workspace(event.window) < 0) return;
      const long action = event.data.l[0];
      const Atom first = static_cast<Atom>(event.data.l[1]);
      const Atom second = static_cast<Atom>(event.data.l[2]);
      if (first != net_wm_state_fullscreen_atom_ && second != net_wm_state_fullscreen_atom_) return;
      const bool currently = window_state_[event.window].fullscreen;
      const bool enable = action == 1 ? true : action == 0 ? false : !currently;
      set_fullscreen(event.window, enable);
    } else if (event.message_type == net_active_window_atom_) {
      const int index = find_workspace(event.window);
      if (index < 0) return;
      if (index != current_workspace_) switch_workspace(index);
      focus(event.window);
    }
  }

  // }}} EWMH

  bool is_tray_icon(Window window) const {
    return std::any_of(tray_icons_.begin(), tray_icons_.end(), [window](const TrayIcon& icon) {
      return icon.window == window;
    });
  }

  void destroy_tray() {
    if (!display_ || tray_ == None) return;
    for (const TrayIcon& icon : tray_icons_) {
      XReparentWindow(display_, icon.window, root_, 0, 0);
      XRemoveFromSaveSet(display_, icon.window);
    }
    tray_icons_.clear();
    if (XGetSelectionOwner(display_, net_system_tray_atom_) == tray_) {
      XSetSelectionOwner(display_, net_system_tray_atom_, None, CurrentTime);
    }
    XDestroyWindow(display_, tray_);
    tray_ = None;
  }

  void create_tray() {
    tray_ = XCreateSimpleWindow(display_, root_, 0, 0, 1, kBarHeight, 0,
                                BlackPixel(display_, screen_), BlackPixel(display_, screen_));
    const unsigned long orientation = 0;  // _NET_SYSTEM_TRAY_ORIENTATION_HORZ
    XChangeProperty(display_, tray_, net_system_tray_orientation_atom_, XA_CARDINAL, 32,
                    PropModeReplace, reinterpret_cast<const unsigned char*>(&orientation), 1);
    XSelectInput(display_, tray_, ExposureMask | ButtonPressMask | SubstructureNotifyMask);
    XSetSelectionOwner(display_, net_system_tray_atom_, tray_, CurrentTime);
    if (XGetSelectionOwner(display_, net_system_tray_atom_) != tray_) {
      XDestroyWindow(display_, tray_);
      tray_ = None;
      return;
    }
    XEvent announcement{};
    announcement.xclient.type = ClientMessage;
    announcement.xclient.window = root_;
    announcement.xclient.message_type = manager_atom_;
    announcement.xclient.format = 32;
    announcement.xclient.data.l[0] = CurrentTime;
    announcement.xclient.data.l[1] = net_system_tray_atom_;
    announcement.xclient.data.l[2] = tray_;
    XSendEvent(display_, root_, False, StructureNotifyMask, &announcement);
  }

  void update_tray_icon_state(TrayIcon& icon) {
    Atom actual_type;
    int format;
    unsigned long count = 0, remaining = 0;
    unsigned char* data = nullptr;
    if (XGetWindowProperty(display_, icon.window, xembed_info_atom_, 0, 2, False, xembed_info_atom_,
                           &actual_type, &format, &count, &remaining, &data) == Success && data && count >= 2) {
      icon.mapped = (reinterpret_cast<unsigned long*>(data)[1] & kXEmbedMapped) != 0;
      XFree(data);
    }
  }

  // On-screen width currently occupied by the tray window (0 when hidden),
  // so the bar can reserve space for it and avoid overlapping tray icons.
  int tray_pixel_width() const {
    if (tray_ == None) return 0;
    int width = 0;
    for (const TrayIcon& icon : tray_icons_) if (icon.mapped) width += icon.width + kTraySpacing;
    return width == 0 ? 0 : width + kTraySpacing;
  }

  void update_tray() {
    if (tray_ == None) return;
    int width = 0;
    for (const TrayIcon& icon : tray_icons_) if (icon.mapped) width += icon.width + kTraySpacing;
    if (width == 0) {
      XUnmapWindow(display_, tray_);
      return;
    }
    width += kTraySpacing;
    int x = kTraySpacing;
    for (const TrayIcon& icon : tray_icons_) {
      if (!icon.mapped) { XUnmapWindow(display_, icon.window); continue; }
      XMoveResizeWindow(display_, icon.window, x, (kBarHeight - icon.height) / 2, icon.width, icon.height);
      XMapRaised(display_, icon.window);
      x += icon.width + kTraySpacing;
    }
    XMoveResizeWindow(display_, tray_, DisplayWidth(display_, screen_) - kDockWidth - width, 0, width, kBarHeight);
    XMapRaised(display_, tray_);
  }

  void dock_tray_icon(Window window) {
    if (tray_ == None || is_tray_icon(window)) return;
    XWindowAttributes attributes;
    if (!XGetWindowAttributes(display_, window, &attributes)) return;
    TrayIcon icon;
    icon.window = window;
    icon.height = kBarHeight;
    icon.width = std::clamp(attributes.height > 0 ? attributes.width * kBarHeight / attributes.height : kBarHeight,
                            1, kBarHeight * 2);
    XAddToSaveSet(display_, window);
    XSelectInput(display_, window, StructureNotifyMask | PropertyChangeMask | ResizeRedirectMask);
    XSetWindowBorderWidth(display_, window, 0);
    XReparentWindow(display_, window, tray_, 0, 0);
    tray_icons_.push_back(icon);
    XEvent notification{};
    notification.xclient.type = ClientMessage;
    notification.xclient.window = window;
    notification.xclient.message_type = xembed_atom_;
    notification.xclient.format = 32;
    notification.xclient.data.l[0] = CurrentTime;
    notification.xclient.data.l[1] = kXEmbedEmbeddedNotify;
    notification.xclient.data.l[3] = tray_;
    XSendEvent(display_, window, False, NoEventMask, &notification);
    update_tray_icon_state(tray_icons_.back());
    update_tray();
  }

  void remove_tray_icon(Window window) {
    tray_icons_.erase(std::remove_if(tray_icons_.begin(), tray_icons_.end(), [window](const TrayIcon& icon) {
      return icon.window == window;
    }), tray_icons_.end());
    update_tray();
  }

  void create_docks() {
    while (docks_.size() < monitors_.size()) {
      XSetWindowAttributes attributes{};
      attributes.override_redirect = True;
      attributes.background_pixel = bar_background_.pixel;
      attributes.event_mask = ExposureMask | ButtonPressMask;
      DockWindows dock;
      dock.left = XCreateWindow(display_, root_, 0, 0, kDockWidth, 1, 0, DefaultDepth(display_, screen_),
                                CopyFromParent, DefaultVisual(display_, screen_),
                                CWOverrideRedirect | CWBackPixel | CWEventMask, &attributes);
      dock.bottom = XCreateWindow(display_, root_, 0, 0, 1, kBarHeight, 0, DefaultDepth(display_, screen_),
                                  CopyFromParent, DefaultVisual(display_, screen_),
                                  CWOverrideRedirect | CWBackPixel | CWEventMask, &attributes);
      dock.right = XCreateWindow(display_, root_, 0, 0, kDockWidth, 1, 0, DefaultDepth(display_, screen_),
                                 CopyFromParent, DefaultVisual(display_, screen_),
                                 CWOverrideRedirect | CWBackPixel | CWEventMask, &attributes);
      XDefineCursor(display_, dock.left, cursor_);
      XDefineCursor(display_, dock.bottom, cursor_);
      XDefineCursor(display_, dock.right, cursor_);
      docks_.push_back(dock);
    }
    for (std::size_t index = 0; index < monitors_.size(); ++index) {
      const Monitor& target = monitors_[index];
      const DockWindows& dock = docks_[index];
      const int side_height = std::max(1, target.height - 2 * kBarHeight);
      // The vertical docks now sit between the top and bottom bars (which
      // own the corners: launcher/layout up top, os/info down below), and
      // the bottom dock spans the full monitor width to match the top bar.
      XMoveResizeWindow(display_, dock.left, target.x, target.y + kBarHeight, kDockWidth, side_height);
      XMoveResizeWindow(display_, dock.right, target.x + target.width - kDockWidth, target.y + kBarHeight,
                        kDockWidth, side_height);
      XMoveResizeWindow(display_, dock.bottom, target.x, target.y + target.height - kBarHeight,
                        std::max(1, target.width), kBarHeight);
      XMapRaised(display_, dock.left);
      XMapRaised(display_, dock.bottom);
      XMapRaised(display_, dock.right);
    }
  }

  void draw_dock_text(Window window, int x, int baseline, const std::string& value, const XftColor& color) {
    XftDraw* draw = XftDrawCreate(display_, window, DefaultVisual(display_, screen_), DefaultColormap(display_, screen_));
    int cursor = x;
    for (std::size_t offset = 0; draw && offset < value.size();) {
      FcChar32 codepoint;
      const std::size_t bytes = utf8_codepoint(value, offset, &codepoint);
      XftFont* font = XftCharExists(display_, bar_font_, codepoint) ? bar_font_ : fallback_font(codepoint);
      XftDrawStringUtf8(draw, &color, font, cursor, baseline,
                        reinterpret_cast<const FcChar8*>(value.data() + offset), static_cast<int>(bytes));
      XGlyphInfo extent{};
      XftTextExtentsUtf8(display_, font, reinterpret_cast<const FcChar8*>(value.data() + offset),
                          static_cast<int>(bytes), &extent);
      cursor += extent.xOff;
      offset += bytes;
    }
    if (draw) XftDrawDestroy(draw);
  }

  void draw_dock_cell(Window window, int y, int width, int height, const std::string& label, bool selected = false) {
    if (selected) {
      XSetForeground(display_, bar_gc_, bar_selected_.pixel);
      XFillRectangle(display_, window, bar_gc_, 0, y, width, height);
    }
    const int text_x = std::max(3, (width - text_width(label)) / 2);
    draw_dock_text(window, text_x, y + (height + bar_font_->ascent - bar_font_->descent) / 2, label,
                   selected ? bar_background_ : bar_foreground_);
  }

  // Same as draw_dock_cell but laid out along x instead of y, for cells that
  // sit inside a horizontal bar (the launcher/layout corners of the top bar,
  // the os/info corners of the bottom bar).
  void draw_dock_cell_h(Window window, int x, int width, int height, const std::string& label, bool selected = false) {
    if (selected) {
      XSetForeground(display_, bar_gc_, bar_selected_.pixel);
      XFillRectangle(display_, window, bar_gc_, x, 0, width, height);
    }
    const int text_x = x + std::max(3, (width - text_width(label)) / 2);
    draw_dock_text(window, text_x, (height + bar_font_->ascent - bar_font_->descent) / 2, label,
                   selected ? bar_background_ : bar_foreground_);
  }

  static long read_long(const std::string& path, long fallback = -1) {
    std::ifstream file(path);
    long value = fallback;
    file >> value;
    return file ? value : fallback;
  }

  static std::string capture_command(const char* command) {
    std::string output;
    FILE* process = popen(command, "r");
    if (!process) return output;
    char buffer[256];
    while (fgets(buffer, sizeof(buffer), process)) output += buffer;
    pclose(process);
    while (!output.empty() && std::isspace(static_cast<unsigned char>(output.back()))) output.pop_back();
    return output;
  }

  static int percent_from_command_output(const std::string& output) {
    const std::size_t percent = output.find('%');
    if (percent == std::string::npos) return -1;
    std::size_t begin = percent;
    while (begin > 0 && std::isdigit(static_cast<unsigned char>(output[begin - 1]))) --begin;
    return begin == percent ? -1 : std::atoi(output.substr(begin, percent - begin).c_str());
  }

  void refresh_agents() {
    agents_.clear();
    agent_needs_input_ = 0;
    auto field = [](const std::string& json, const char* key) {
      const std::string needle = std::string("\"") + key + "\"";
      const std::size_t key_at = json.find(needle);
      if (key_at == std::string::npos) return std::string{};
      const std::size_t quote = json.find('\"', json.find(':', key_at) + 1);
      if (quote == std::string::npos) return std::string{};
      const std::size_t end = json.find('\"', quote + 1);
      return end == std::string::npos ? std::string{} : json.substr(quote + 1, end - quote - 1);
    };
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    const std::string status_dir = runtime && *runtime ? std::string(runtime) + "/mwm-agents" :
        std::string("/tmp/mwm-agents-") + (std::getenv("USER") ? std::getenv("USER") : "mwm");
    if (DIR* directory = opendir(status_dir.c_str())) {
      while (dirent* entry = readdir(directory)) {
        const std::string name(entry->d_name);
        if (name.size() < 6 || name.substr(name.size() - 5) != ".json" || agents_.size() >= 64) continue;
        std::ifstream file(status_dir + "/" + name);
        const std::string json((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        AgentStatus agent;
        agent.kind = field(json, "agent");
        agent.status = field(json, "status");
        agent.cwd = field(json, "cwd");
        agent.label = field(json, "label");
        if (agent.kind.empty() || agent.status.empty() || agent.cwd.empty()) continue;
        agent.from_file = true;
        agent.file_path = status_dir + "/" + name;
        agent.needs_input = agent.status == "needs_input";
        if (agent.needs_input) ++agent_needs_input_;
        agents_.push_back(std::move(agent));
      }
      closedir(directory);
    }
    if (DIR* directory = opendir("/proc")) {
      while (dirent* entry = readdir(directory)) {
        const std::string name(entry->d_name);
        if (name.empty() || !std::all_of(name.begin(), name.end(), [](unsigned char c) { return std::isdigit(c); }) || agents_.size() >= 64) continue;
        std::ifstream comm_file("/proc/" + name + "/comm");
        std::string kind;
        std::getline(comm_file, kind);
        if (kind != "claude" && kind != "codex") continue;
        char cwd[384]{};
        const ssize_t length = readlink(("/proc/" + name + "/cwd").c_str(), cwd, sizeof(cwd) - 1);
        if (length <= 0) continue;
        cwd[length] = '\0';
        auto existing = std::find_if(agents_.begin(), agents_.end(), [&](const AgentStatus& a) { return a.kind == kind && a.cwd == cwd; });
        if (existing != agents_.end()) { existing->pid = static_cast<pid_t>(std::atoi(name.c_str())); continue; }
        agents_.push_back({static_cast<pid_t>(std::atoi(name.c_str())), kind, "running", "running", cwd});
      }
      closedir(directory);
    }
  }

  static pid_t parent_pid(pid_t pid) {
    std::ifstream file("/proc/" + std::to_string(pid) + "/stat");
    std::string contents((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    const std::size_t close = contents.rfind(')');
    if (close == std::string::npos) return 0;
    std::istringstream fields(contents.substr(close + 1));
    char state = 0;
    pid_t parent = 0;
    fields >> state >> parent;
    return parent;
  }

  void refresh_git_widget() {
    git_status_.clear();
    const char* project = std::getenv("MEPWM_PROJECT_DIR");
    if (!project || !*project) return;
    std::filesystem::path git_dir = std::filesystem::path(project) / ".git";
    std::error_code error;
    if (std::filesystem::is_regular_file(git_dir, error)) {
      std::ifstream pointer(git_dir);
      std::string line;
      std::getline(pointer, line);
      constexpr const char* prefix = "gitdir: ";
      if (line.rfind(prefix, 0) != 0) return;
      git_dir = line.substr(std::strlen(prefix));
      if (git_dir.is_relative()) git_dir = std::filesystem::path(project) / git_dir;
    }
    std::ifstream head(git_dir / "HEAD");
    std::string reference;
    std::getline(head, reference);
    constexpr const char* ref_prefix = "ref: refs/heads/";
    if (reference.rfind(ref_prefix, 0) == 0) reference = reference.substr(std::strlen(ref_prefix));
    if (!reference.empty()) git_status_ = reference;
  }

  void refresh_widgets() {
    // The widget probe launches several external commands (audio, network,
    // Bluetooth, media, and keyboard queries).  Do not make the first mapped
    // frame wait for them; the regular event-loop refresh fills these values.
    if (defer_widget_refresh_) {
      if (startup_timer_) startup_timer_->checkpoint("defer dock widget refresh");
      return;
    }
    const std::time_t now = std::time(nullptr);
    if (widgets_refreshed_ != 0 && now - widgets_refreshed_ < 2) return;
    widgets_refreshed_ = now;

    if (battery_capacity_path_.empty()) {
      if (DIR* directory = opendir("/sys/class/power_supply")) {
        while (dirent* entry = readdir(directory)) {
          const std::string name(entry->d_name);
          if (name.rfind("BAT", 0) == 0) {
            battery_capacity_path_ = "/sys/class/power_supply/" + name + "/capacity";
            battery_status_path_ = "/sys/class/power_supply/" + name + "/status";
            break;
          }
        }
        closedir(directory);
      }
    }
    if (backlight_path_.empty()) {
      if (DIR* directory = opendir("/sys/class/backlight")) {
        while (dirent* entry = readdir(directory)) {
          if (entry->d_name[0] == '.') continue;
          const std::string base = std::string("/sys/class/backlight/") + entry->d_name;
          backlight_path_ = base + "/brightness";
          backlight_max_path_ = base + "/max_brightness";
          break;
        }
        closedir(directory);
      }
    }
    battery_percent_ = read_long(battery_capacity_path_);
    std::ifstream battery_status(battery_status_path_);
    std::string status;
    std::getline(battery_status, status);
    battery_charging_ = status == "Charging" || status == "Full";
    const long brightness = read_long(backlight_path_);
    const long brightness_max = read_long(backlight_max_path_);
    backlight_percent_ = brightness >= 0 && brightness_max > 0 ? static_cast<int>(brightness * 100 / brightness_max) : -1;

    std::ifstream stat("/proc/stat");
    std::string cpu_label;
    long user = 0, nice = 0, system = 0, idle = 0, wait = 0, irq = 0, softirq = 0, steal = 0;
    stat >> cpu_label >> user >> nice >> system >> idle >> wait >> irq >> softirq >> steal;
    const long total = user + nice + system + idle + wait + irq + softirq + steal;
    const long idle_all = idle + wait;
    if (cpu_previous_total_ > 0 && total > cpu_previous_total_)
      cpu_percent_ = static_cast<int>(100 * ((total - cpu_previous_total_) - (idle_all - cpu_previous_idle_)) /
                                      (total - cpu_previous_total_));
    cpu_previous_total_ = total;
    cpu_previous_idle_ = idle_all;

    std::ifstream memory("/proc/meminfo");
    std::string key;
    long value = 0, mem_total = 0, mem_available = 0;
    while (memory >> key >> value) {
      if (key == "MemTotal:") mem_total = value;
      if (key == "MemAvailable:") mem_available = value;
      memory.ignore(256, '\n');
    }
    mem_percent_ = mem_total > 0 ? static_cast<int>(100 * (mem_total - mem_available) / mem_total) : -1;
    struct statvfs filesystem {};
    disk_percent_ = statvfs("/", &filesystem) == 0 && filesystem.f_blocks > 0
                        ? static_cast<int>(100 * (filesystem.f_blocks - filesystem.f_bfree) /
                                           std::max<fsblkcnt_t>(1, filesystem.f_blocks - filesystem.f_bfree + filesystem.f_bavail))
                        : -1;
    std::ifstream routes("/proc/net/route");
    std::string line;
    std::getline(routes, line);
    network_interface_.clear();
    while (std::getline(routes, line)) {
      std::istringstream route(line);
      std::string iface, destination;
      route >> iface >> destination;
      if (destination == "00000000") { network_interface_ = iface; break; }
    }
    network_up_ = false;
    if (!network_interface_.empty()) {
      std::ifstream operstate("/sys/class/net/" + network_interface_ + "/operstate");
      std::getline(operstate, status);
      network_up_ = status == "up";
    }
    wifi_interface_.clear();
    std::ifstream wireless("/proc/net/wireless");
    std::getline(wireless, line); std::getline(wireless, line);
    if (std::getline(wireless, line)) {
      const std::size_t colon = line.find(':');
      if (colon != std::string::npos) wifi_interface_ = trim(line.substr(0, colon));
    }
    wifi_ssid_.clear();
    if (!wifi_interface_.empty()) {
      const std::string active = capture_command("nmcli -t -f active,ssid dev wifi list --rescan no 2>/dev/null | grep '^yes:' | head -1");
      if (active.rfind("yes:", 0) == 0) wifi_ssid_ = active.substr(4);
    }
    wifi_blocked_ = capture_command("rfkill list wifi 2>/dev/null | grep -q 'Soft blocked: yes' && echo yes") == "yes";
    bluetooth_status_ = capture_command("bluetoothctl devices Connected 2>/dev/null | sed -n '1s/^Device [^ ]* //p'");
    bluetooth_blocked_ = capture_command("rfkill list bluetooth 2>/dev/null | grep -q 'Soft blocked: yes' && echo yes") == "yes";
    if (keyboard_layouts_.empty()) {
      const std::string configured = capture_command("setxkbmap -query 2>/dev/null | sed -n 's/^layout:[[:space:]]*//p'");
      for (std::size_t start = 0; start < configured.size();) {
        const std::size_t end = configured.find(',', start);
        keyboard_layouts_.push_back(configured.substr(start, end - start));
        if (end == std::string::npos) break;
        start = end + 1;
      }
    }
    XkbStateRec keyboard_state{};
    if (XkbGetState(display_, XkbUseCoreKbd, &keyboard_state) == Success && keyboard_state.group >= 0 &&
        static_cast<std::size_t>(keyboard_state.group) < keyboard_layouts_.size())
      keyboard_layout_ = keyboard_layouts_[keyboard_state.group];
    const std::string volume = capture_command("amixer get Master 2>/dev/null");
    volume_percent_ = percent_from_command_output(volume);
    volume_muted_ = volume.find("[off]") != std::string::npos;
    const std::string microphone = capture_command("amixer get Capture 2>/dev/null");
    mic_percent_ = percent_from_command_output(microphone);
    mic_muted_ = microphone.find("[off]") != std::string::npos;
    media_title_ = capture_command("playerctl metadata --format '{{ artist }} - {{ title }}' 2>/dev/null");
    std::time_t clock = std::time(nullptr);
    char clock_text[32];
    std::strftime(clock_text, sizeof(clock_text), "%a %m-%d %H:%M", std::localtime(&clock));
    clock_text_ = clock_text;
    refresh_agents();
    refresh_git_widget();
    const std::time_t expiry_now = std::time(nullptr);
    for (auto it = notifications_.begin(); it != notifications_.end();) {
      if (it->expires_at != 0 && it->expires_at <= expiry_now) {
        emit_notification_closed(it->id, 1);
        it = notifications_.erase(it);
      } else {
        ++it;
      }
    }
    for (LuaWidget& widget : lua_widgets_) {
      if (widget.update == LUA_NOREF) continue;
      lua_rawgeti(lua_, LUA_REGISTRYINDEX, widget.update);
      if (lua_pcall(lua_, 0, 1, 0) != LUA_OK) {
        std::cerr << "mepwm: widget '" << widget.name << "' update error: " << lua_tostring(lua_, -1) << '\n';
        lua_pop(lua_, 1);
        widget.text = widget.name + ": error";
      } else {
        widget.text = lua_isstring(lua_, -1) ? lua_tostring(lua_, -1) : "";
        lua_pop(lua_, 1);
      }
    }
    if (startup_timer_) startup_timer_->checkpoint("refresh dock widgets");
  }

  void draw_bottom_widgets(std::size_t monitor_index) {
    refresh_widgets();
    const DockWindows& dock = docks_[monitor_index];
    const char* battery_level_icon = battery_percent_ >= 90 ? kIconBatteryFull
                                     : battery_percent_ >= 65 ? kIconBattery75
                                     : battery_percent_ >= 40 ? kIconBattery50
                                     : battery_percent_ >= 15 ? kIconBattery25
                                                              : kIconBatteryEmpty;
    std::string battery_text;
    if (battery_percent_ < 0) {
      battery_text = std::string(kIconBatteryFull) + " n/a";
    } else {
      battery_text = battery_level_icon;
      if (battery_charging_) battery_text += kIconBatteryCharging;
      // Below 20% the icon alone is too coarse to judge urgency, so fall
      // back to the exact number; otherwise the icon carries enough detail.
      if (battery_percent_ < 20) battery_text += " " + std::to_string(battery_percent_) + "%";
    }
    const char* volume_icon = volume_percent_ < 0 ? kIconVolumeHigh
                              : volume_muted_ || volume_percent_ == 0 ? kIconVolumeMuted
                              : volume_percent_ < 50 ? kIconVolumeLow
                                                     : kIconVolumeHigh;
    const std::array<std::pair<const char*, std::string>, 13> widgets = {{
        {"battery", battery_text},
        {"backlight", kIconBacklight},
        {"volume", volume_icon},
        {"mic", mic_percent_ < 0 ? "" : (mic_muted_ ? kIconMicrophoneMuted : kIconMicrophone)},
        {"media", media_title_.empty() ? "" : std::string(kIconMedia) + " " + media_title_},
        {"theme", theme_is_light() ? kIconThemeLight : kIconThemeDark},
        {"git", git_status_.empty() ? "" : std::string(kIconGit) + " " + git_status_},
        {"cpu", cpu_percent_ < 0 ? std::string(kIconCpu) + " ..." : std::string(kIconCpu) + " " + std::to_string(cpu_percent_) + "%"},
        {"memory", mem_percent_ < 0 ? std::string(kIconMemory) + " n/a" : std::string(kIconMemory) + " " + std::to_string(mem_percent_) + "%"},
        {"disk", disk_percent_ < 0 ? std::string(kIconDisk) + " n/a" : std::string(kIconDisk) + " " + std::to_string(disk_percent_) + "%"},
        {"wifi", wifi_interface_.empty() ? std::string(kIconWifi) + " n/a" : std::string(kIconWifi) + " " + (wifi_ssid_.empty() ? wifi_interface_ : wifi_ssid_)},
        {"bluetooth", bluetooth_status_.empty() ? kIconBluetooth : std::string(kIconBluetooth) + " " + bluetooth_status_},
        {"keyboard", keyboard_layouts_.size() > 1 ? std::string(kIconKeyboard) + " " + keyboard_layout_ : ""},
    }};
    bottom_widget_hits_.clear();
    // The bottom bar now spans the full monitor width with the os/info icons
    // in its outer kDockWidth corners, so the widget group is packed into the
    // same span it always had, just offset past the left (os) corner.
    const int content_x = kDockWidth;
    const int width = std::max(1, monitors_[monitor_index].width - 2 * kDockWidth);
    int total_width = 0;
    for (const auto& widget : widgets)
      if (!widget.second.empty()) total_width += text_width(widget.second) + 14;
    for (const LuaWidget& widget : lua_widgets_)
      if (!widget.text.empty()) total_width += text_width(widget.text) + 14;
    // The bottom dock is a system-status area, so anchor its complete widget
    // group to the right edge. Keep the existing left margin as a safe
    // fallback when a narrow monitor cannot fit every widget.
    int x = std::max(content_x + 8, content_x + width - total_width);
    for (const auto& widget : widgets) {
      if (widget.second.empty()) continue;
      const int widget_width = text_width(widget.second) + 14;
      if (x + widget_width > content_x + width) break;
      draw_dock_text(dock.bottom_buffer, x + 7, (kBarHeight + bar_font_->ascent - bar_font_->descent) / 2, widget.second,
                     bar_foreground_);
      bottom_widget_hits_.push_back({x, x + widget_width, widget.first});
      x += widget_width;
    }
    for (std::size_t index = 0; index < lua_widgets_.size(); ++index) {
      const LuaWidget& widget = lua_widgets_[index];
      if (widget.text.empty()) continue;
      const int widget_width = text_width(widget.text) + 14;
      if (x + widget_width > content_x + width) break;
      if (widget.highlight) {
        XSetForeground(display_, bar_gc_, bar_selected_.pixel);
        XFillRectangle(display_, dock.bottom_buffer, bar_gc_, x, 0, widget_width, kBarHeight);
      }
      draw_dock_text(dock.bottom_buffer, x + 7, (kBarHeight + bar_font_->ascent - bar_font_->descent) / 2, widget.text,
                     widget.highlight ? bar_background_ : bar_foreground_);
      bottom_widget_hits_.push_back({x, x + widget_width, "lua:" + std::to_string(index)});
      x += widget_width;
    }
  }

  int slider_percent() const {
    if (slider_kind_ == SliderKind::Backlight) return backlight_percent_;
    if (slider_kind_ == SliderKind::Volume) return volume_percent_;
    return mic_percent_;
  }

  std::string slider_label() const {
    if (slider_kind_ == SliderKind::Backlight) return "Brightness";
    if (slider_kind_ == SliderKind::Volume) return "Volume";
    return "Microphone";
  }

  void set_slider_percent(int percent) {
    percent = std::clamp(percent, 0, 100);
    if (slider_kind_ == SliderKind::Backlight) {
      const long maximum = read_long(backlight_max_path_);
      if (maximum > 0) {
        std::ofstream file(backlight_path_);
        if (file) file << std::max(1L, maximum * percent / 100) << '\n';
      }
    } else if (slider_kind_ == SliderKind::Volume) {
      spawn_command("amixer -q set Master " + std::to_string(percent) + "% unmute");
      volume_percent_ = percent;
      volume_muted_ = false;
    } else {
      spawn_command("amixer -q set Capture " + std::to_string(percent) + "% unmute");
      mic_percent_ = percent;
    }
    widgets_refreshed_ = 0;
  }

  void draw_slider_popup() {
    if (!slider_visible_) return;
    constexpr int width = 240;
    constexpr int height = 64;
    constexpr int pad = 12;
    XSetForeground(display_, bar_gc_, bar_background_.pixel);
    XFillRectangle(display_, slider_window_, bar_gc_, 0, 0, width, height);
    draw_dock_text(slider_window_, pad, 20, slider_label() + " " + std::to_string(std::max(0, slider_percent())) + "%", bar_foreground_);
    const int track_y = 38;
    const int track_width = width - 2 * pad;
    XSetForeground(display_, bar_gc_, border_normal_pixel_);
    XFillRectangle(display_, slider_window_, bar_gc_, pad, track_y, track_width, 8);
    const int fill = std::max(0, slider_percent()) * track_width / 100;
    XSetForeground(display_, bar_gc_, bar_selected_.pixel);
    XFillRectangle(display_, slider_window_, bar_gc_, pad, track_y, fill, 8);
    XFlush(display_);
  }

  void close_slider_popup() {
    slider_visible_ = false;
    slider_dragging_ = false;
    if (slider_window_ != None) XUnmapWindow(display_, slider_window_);
  }

  void open_slider_popup(SliderKind kind) {
    slider_kind_ = kind;
    refresh_widgets();
    if (slider_window_ == None) {
      XSetWindowAttributes attributes{};
      attributes.override_redirect = True;
      attributes.background_pixel = bar_background_.pixel;
      attributes.border_pixel = bar_selected_.pixel;
      attributes.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask;
      slider_window_ = XCreateWindow(display_, root_, 0, 0, 240, 64, 1, DefaultDepth(display_, screen_), CopyFromParent,
                                     DefaultVisual(display_, screen_), CWOverrideRedirect | CWBackPixel | CWBorderPixel | CWEventMask,
                                     &attributes);
      XDefineCursor(display_, slider_window_, cursor_);
    }
    const Monitor& target = monitor(current_monitor_);
    XMoveResizeWindow(display_, slider_window_, target.x + (target.width - 240) / 2,
                      target.y + target.height - 2 * kBarHeight - 72, 240, 64);
    slider_visible_ = true;
    XMapRaised(display_, slider_window_);
    draw_slider_popup();
  }

  void handle_slider_position(int x) {
    constexpr int pad = 12;
    constexpr int track_width = 216;
    set_slider_percent(std::clamp((x - pad) * 100 / track_width, 0, 100));
    draw_slider_popup();
    draw_docks();
  }

  // Todos are project-centric: sourced read-only from a TODO.org file in the
  // active project's directory rather than a manually-managed store, so the
  // sidebar always reflects whichever project is currently active.
  std::string todo_org_path() const {
    if (active_project_index_ >= projects_.size()) return {};
    return projects_[active_project_index_].path + "/TODO.org";
  }

  // True if `line` is an org headline ("* TODO text") with the given
  // keyword; on success, *keyword_start/*text_start bound the keyword and
  // the headline text that follows it.
  static bool parse_org_headline(const std::string& line, const std::string& keyword,
                                  std::size_t* keyword_start, std::size_t* text_start) {
    const std::size_t stars = line.find_first_not_of('*');
    if (stars == 0 || stars == std::string::npos || line[stars] != ' ') return false;
    *keyword_start = line.find_first_not_of(' ', stars + 1);
    if (*keyword_start == std::string::npos) return false;
    const std::size_t space = line.find(' ', *keyword_start);
    if (space == std::string::npos || line.compare(*keyword_start, space - *keyword_start, keyword) != 0) return false;
    *text_start = line.find_first_not_of(' ', space + 1);
    return *text_start != std::string::npos;
  }

  // Parses org-mode headlines ("* TODO Buy milk"); only the plain TODO
  // keyword is treated as a pending item, DONE (and everything else) is
  // skipped so completed items never show up.
  void load_todos() {
    todos_.clear();
    std::ifstream file(todo_org_path());
    std::size_t line_number = 0;
    for (std::string line; std::getline(file, line); ++line_number) {
      std::size_t keyword_start = 0, text_start = 0;
      if (parse_org_headline(line, "TODO", &keyword_start, &text_start)) todos_.push_back({line.substr(text_start), line_number});
    }
    if (todo_selected_ >= static_cast<int>(todos_.size())) todo_selected_ = todos_.empty() ? -1 : static_cast<int>(todos_.size()) - 1;
  }

  // Rewrites a single line in-place, flipping its TODO keyword to DONE.
  // Re-checks the line still looks like the expected TODO headline first, in
  // case the file changed underneath us since it was last parsed.
  void mark_todo_done(const TodoItem& item) {
    const std::string path = todo_org_path();
    if (path.empty()) return;
    std::vector<std::string> lines;
    {
      std::ifstream file(path);
      if (!file) return;
      for (std::string line; std::getline(file, line);) lines.push_back(std::move(line));
    }
    if (item.line >= lines.size()) return;
    std::size_t keyword_start = 0, text_start = 0;
    if (!parse_org_headline(lines[item.line], "TODO", &keyword_start, &text_start)) return;
    lines[item.line].replace(keyword_start, std::string("TODO").size(), "DONE");
    std::ofstream file(path);
    for (const std::string& line : lines) file << line << '\n';
  }

  // Appends a new TODO headline to the project's TODO.org, creating the file
  // if it doesn't exist yet.
  void add_todo_item(const std::string& text) {
    const std::string path = todo_org_path();
    if (path.empty() || text.empty()) return;
    std::ofstream file(path, std::ios::app);
    if (file) file << "* TODO " << text << '\n';
  }

  // Kept in sync by hand with handle_key/handle_button/grab_keys -- there's
  // no single compiled keybind table to derive this from (unlike mwm.cpp's
  // keys[]), since mep-wm dispatches via if-chains keyed on (state, KeySym).
  static const std::vector<std::pair<std::string, std::string>>& compiled_keybind_help() {
    static const std::vector<std::pair<std::string, std::string>> bindings = {
        {"Super+Return", "Open a terminal"},
        {"Super+p", "Application picker"},
        {"Super+w", "Window switcher"},
        {"Super+f", "Element hints (click anything by typing its label)"},
        {"Super+i", "Recent projects picker"},
        {"Super+o", "Active projects picker"},
        {"Super+h/j/k/l", "Focus direction (selects empty panes too in manual layout)"},
        {"Super+Ctrl+j/k", "Focus next/previous in stack order"},
        {"Super+Ctrl+h/l", "Shrink/grow master area"},
        {"Super+v", "Split pane vertically"},
        {"Super+s", "Split pane horizontally"},
        {"Super+Tab", "Next tab"},
        {"Super+Shift+Tab", "Previous tab"},
        {"Super+Space / Super+a", "Cycle layout"},
        {"Super+Ctrl+Space", "Cycle layout (reverse, Lua-bound)"},
        {"Super+z", "Zoom focused window to master"},
        {"Super+m", "Toggle maximize"},
        {"Super+d", "Decrease master count (or delete/merge current pane in manual mode)"},
        {"Super+Shift+d", "Increase master count"},
        {"Super+b", "Toggle bar"},
        {"Super+minus", "Toggle scratchpad"},
        {"Super+Shift+minus", "Designate scratchpad"},
        {"Super+Shift+space", "Toggle floating"},
        {"Super+Shift+c", "Kill focused window"},
        {"Super+Shift+h/j/k/l", "Resize pane"},
        {"Super+comma / Super+period", "Focus previous/next monitor"},
        {"Super+Shift+comma / Super+Shift+period", "Send window to previous/next monitor"},
        {"Super+1..9", "Switch workspace"},
        {"Super+r", "Reload config"},
        {"Super+Shift+r", "Restart mepwm"},
        {"Super+Shift+t", "Theme picker"},
        {"Super+Shift+slash", "Toggle this help panel"},
        {"Super+Shift+q", "Quit"},
        {"Super+drag (left click)", "Move window"},
        {"Super+drag (right click)", "Resize window"},
    };
    return bindings;
  }

  void draw_side_panel() {
    if (side_panel_ == SidePanel::Closed || side_panel_window_ == None) return;
    if (side_panel_ == SidePanel::Todos) load_todos();
    if (side_panel_ == SidePanel::Agents) refresh_agents();
    constexpr int width = 360;
    const Monitor& target = monitor(side_panel_monitor_);
    const int height = std::max(1, target.height - 2 * kBarHeight);
    XMoveResizeWindow(display_, side_panel_window_, target.x + target.width - kDockWidth - width,
                      target.y + kBarHeight, width, height);
    XSetForeground(display_, bar_gc_, bar_background_.pixel);
    XFillRectangle(display_, side_panel_window_, bar_gc_, 0, 0, width, height);
    std::string title;
    if (side_panel_ == SidePanel::Notifications) title = "Notifications";
    if (side_panel_ == SidePanel::Todos)
      title = "Todos - " + (projects_.empty() ? std::string("no project") : project_label(projects_[active_project_index_].path));
    if (side_panel_ == SidePanel::Agents) title = "Agents";
    if (side_panel_ == SidePanel::Help) title = "Keybindings";
    if (side_panel_ == SidePanel::Info) title = info_panel_title_;
    draw_dock_text(side_panel_window_, 12, 20, title, bar_foreground_);
    if (side_panel_ == SidePanel::Notifications && !notifications_.empty())
      draw_dock_text(side_panel_window_, width - 52, 20, "clear", bar_foreground_);
    if (side_panel_ == SidePanel::Agents && std::any_of(agents_.begin(), agents_.end(), [](const AgentStatus& agent) { return agent.from_file; }))
      draw_dock_text(side_panel_window_, width - 52, 20, "clear", bar_foreground_);
    XSetForeground(display_, bar_gc_, border_normal_pixel_);
    XFillRectangle(display_, side_panel_window_, bar_gc_, 0, kBarHeight - 1, width, 1);
    side_panel_rows_.clear();
    int y = kBarHeight + 6 - side_panel_scroll_offset_;
    auto row = [&](const std::string& text, bool selected = false) {
      if (y + kBarHeight > kBarHeight && y < height && selected) {
        XSetForeground(display_, bar_gc_, bar_selected_.pixel);
        XFillRectangle(display_, side_panel_window_, bar_gc_, 6, y, width - 12, kBarHeight);
      }
      if (y + kBarHeight > kBarHeight && y < height)
        draw_dock_text(side_panel_window_, 12, y + (kBarHeight + bar_font_->ascent - bar_font_->descent) / 2,
                       text, selected ? bar_background_ : bar_foreground_);
      side_panel_rows_.push_back(y);
      y += kBarHeight + 2;
    };
    if (side_panel_ == SidePanel::Notifications) {
      if (notifications_.empty()) row("No notifications");
      for (const Notification& notification : notifications_) {
        row((notification.unread ? "• " : "  ") + notification.app + ": " + notification.summary, notification.unread);
        row(notification.body.empty() ? "" : "  " + notification.body, notification.unread);
      }
    } else if (side_panel_ == SidePanel::Todos) {
      row("a add   d done   ^n/^p move");
      if (todo_input_active_) row("+ " + todo_input_text_ + "_", true);
      if (todos_.empty() && !todo_input_active_) {
        const std::string path = todo_org_path();
        row(path.empty() || !std::filesystem::exists(path) ? "No TODO.org in this project" : "No pending TODO items");
      }
      for (std::size_t index = 0; index < todos_.size(); ++index)
        row("[ ] " + todos_[index].text, !todo_input_active_ && static_cast<int>(index) == todo_selected_);
    } else if (side_panel_ == SidePanel::Agents) {
      if (agents_.empty()) row("No coding agents are running");
      for (const AgentStatus& agent : agents_)
        row(agent.kind + " " + agent.status + ": " + (agent.label.empty() ? agent.cwd : agent.label), agent.needs_input);
    } else if (side_panel_ == SidePanel::Help) {
      for (const auto& [spec, description] : compiled_keybind_help()) row(spec + "  " + description);
      if (!lua_keybinds_.empty()) {
        row("");
        row("-- from config.lua --");
        for (const LuaKeybind& binding : lua_keybinds_)
          row(binding.spec + "  " + (binding.description.empty() ? "(no description)" : binding.description));
      }
    } else {
      std::istringstream lines(info_panel_body_);
      for (std::string line; std::getline(lines, line);) row(line);
    }
    XFlush(display_);
  }

  // Todos is the one side panel that takes keyboard focus (arrow/^n/^p
  // navigation, a/d actions), so entering/leaving it grabs/ungrabs the
  // keyboard the same way the launcher and hint overlays do.
  void enter_todo_panel() {
    load_todos();
    todo_selected_ = todos_.empty() ? -1 : 0;
    todo_input_active_ = false;
    todo_input_text_.clear();
    XGrabKeyboard(display_, root_, False, GrabModeAsync, GrabModeAsync, CurrentTime);
  }

  void leave_todo_panel() {
    XUngrabKeyboard(display_, CurrentTime);
    todo_input_active_ = false;
    todo_input_text_.clear();
  }

  void close_side_panel() {
    if (side_panel_ == SidePanel::Todos) leave_todo_panel();
    side_panel_ = SidePanel::Closed;
    if (side_panel_window_ != None) XUnmapWindow(display_, side_panel_window_);
    draw_docks();
  }

  void toggle_side_panel(SidePanel panel) {
    if (side_panel_ == panel) { close_side_panel(); return; }
    if (side_panel_window_ == None) {
      XSetWindowAttributes attributes{};
      attributes.override_redirect = True;
      attributes.background_pixel = bar_background_.pixel;
      attributes.border_pixel = bar_selected_.pixel;
      attributes.event_mask = ExposureMask | ButtonPressMask;
      side_panel_window_ = XCreateWindow(display_, root_, 0, 0, 360, 200, 1, DefaultDepth(display_, screen_), CopyFromParent,
                                         DefaultVisual(display_, screen_), CWOverrideRedirect | CWBackPixel | CWBorderPixel | CWEventMask,
                                         &attributes);
      XDefineCursor(display_, side_panel_window_, cursor_);
    }
    if (side_panel_ == SidePanel::Todos) leave_todo_panel();
    side_panel_ = panel;
    side_panel_monitor_ = current_monitor_;
    side_panel_scroll_offset_ = 0;
    if (panel == SidePanel::Notifications)
      for (Notification& notification : notifications_) notification.unread = false;
    if (panel == SidePanel::Todos) enter_todo_panel();
    XMapRaised(display_, side_panel_window_);
    draw_side_panel();
    draw_docks();
  }

  void move_todo_selection(int delta) {
    if (todos_.empty()) { todo_selected_ = -1; return; }
    const int count = static_cast<int>(todos_.size());
    todo_selected_ = todo_selected_ < 0 ? 0 : (todo_selected_ + delta + count) % count;
  }

  // Keeps the selected row inside the panel's visible scroll window,
  // mirroring the row-height math handle_side_panel_button's wheel handler
  // already uses for this panel.
  void ensure_todo_selection_visible() {
    if (todo_selected_ < 0) return;
    const int row_height = kBarHeight + 2;
    const int header_rows = 1 + (todo_input_active_ ? 1 : 0);
    const int index = header_rows + todo_selected_;
    const int visible = std::max(1, (monitor(side_panel_monitor_).height - 3 * kBarHeight) / row_height);
    int scroll_row = side_panel_scroll_offset_ / row_height;
    if (index < scroll_row) scroll_row = index;
    else if (index >= scroll_row + visible) scroll_row = index - visible + 1;
    side_panel_scroll_offset_ = std::max(0, scroll_row) * row_height;
  }

  void handle_todo_key(const XKeyEvent& event) {
    char text[32];
    KeySym key = NoSymbol;
    const int length = XLookupString(const_cast<XKeyEvent*>(&event), text, sizeof(text), &key, nullptr);
    const unsigned int state = event.state & ~(LockMask | Mod2Mask);
    if (todo_input_active_) {
      if (key == XK_Escape) { todo_input_active_ = false; todo_input_text_.clear(); draw_side_panel(); return; }
      if (key == XK_Return || key == XK_KP_Enter) {
        add_todo_item(todo_input_text_);
        todo_input_active_ = false;
        todo_input_text_.clear();
        load_todos();
        todo_selected_ = todos_.empty() ? -1 : static_cast<int>(todos_.size()) - 1;
        ensure_todo_selection_visible();
        draw_side_panel();
        draw_docks();
        return;
      }
      if (key == XK_BackSpace) {
        if (!todo_input_text_.empty()) todo_input_text_.pop_back();
        draw_side_panel();
        return;
      }
      for (int index = 0; index < length && todo_input_text_.size() < 255; ++index)
        if (std::isprint(static_cast<unsigned char>(text[index]))) todo_input_text_ += text[index];
      if (length > 0) draw_side_panel();
      return;
    }
    if (key == XK_Escape) { close_side_panel(); return; }
    if (state == 0 && key == XK_a) { todo_input_active_ = true; todo_input_text_.clear(); draw_side_panel(); return; }
    if (state == 0 && key == XK_d && todo_selected_ >= 0 && static_cast<std::size_t>(todo_selected_) < todos_.size()) {
      mark_todo_done(todos_[static_cast<std::size_t>(todo_selected_)]);
      load_todos();
      ensure_todo_selection_visible();
      draw_side_panel();
      draw_docks();
      return;
    }
    if (key == XK_Up || (state == ControlMask && key == XK_p)) {
      move_todo_selection(-1);
      ensure_todo_selection_visible();
      draw_side_panel();
      return;
    }
    if (key == XK_Down || (state == ControlMask && key == XK_n)) {
      move_todo_selection(1);
      ensure_todo_selection_visible();
      draw_side_panel();
      return;
    }
  }

  void open_info_panel(const std::string& title, const std::string& body) {
    info_panel_title_ = title;
    info_panel_body_ = body;
    info_action_ = InfoAction::NoneAction;
    if (side_panel_ == SidePanel::Info) draw_side_panel();
    else toggle_side_panel(SidePanel::Info);
  }

  void open_action_panel(InfoAction action, const std::string& title, const std::string& body) {
    info_action_ = action;
    info_panel_title_ = title;
    info_panel_body_ = body;
    if (side_panel_ == SidePanel::Info) draw_side_panel();
    else toggle_side_panel(SidePanel::Info);
  }

  void spawn_argv(std::vector<std::string> arguments) const {
    if (arguments.empty()) return;
    const pid_t child = fork();
    if (child < 0) return;
    if (child == 0) {
      setsid();
      std::vector<char*> argv;
      argv.reserve(arguments.size() + 1);
      for (std::string& argument : arguments) argv.push_back(argument.data());
      argv.push_back(nullptr);
      execvp(argv[0], argv.data());
      _exit(127);
    }
    signal(SIGCHLD, SIG_IGN);
  }

  void open_wifi_panel() {
    wifi_networks_.clear();
    const std::string output = capture_command("nmcli -t -f ACTIVE,SSID,SECURITY dev wifi list --rescan no 2>/dev/null");
    std::istringstream lines(output);
    for (std::string line; std::getline(lines, line);) {
      const std::size_t first = line.find(':');
      const std::size_t last = line.rfind(':');
      if (first == std::string::npos || last == first) continue;
      const std::string ssid = line.substr(first + 1, last - first - 1);
      if (ssid.empty()) continue;
      wifi_networks_.push_back({ssid, !line.substr(last + 1).empty(), line.substr(0, first) == "yes"});
    }
    std::string body = std::string("Wi-Fi: ") + (wifi_blocked_ ? "Off" : "On") + " (click to toggle)";
    for (const WifiNetwork& network : wifi_networks_)
      body += "\n" + std::string(network.secured ? "[lock] " : "") + network.ssid + (network.active ? " (connected)" : "");
    if (wifi_networks_.empty()) body += "\nNo networks found";
    open_action_panel(InfoAction::Wifi, "Wi-Fi", body);
  }

  void open_bluetooth_panel() {
    bluetooth_devices_.clear();
    const std::string connected = capture_command("bluetoothctl devices Connected 2>/dev/null");
    const std::string output = capture_command("bluetoothctl paired-devices 2>/dev/null");
    std::istringstream lines(output);
    for (std::string line; std::getline(lines, line);) {
      if (line.rfind("Device ", 0) != 0) continue;
      const std::size_t space = line.find(' ', 7);
      if (space == std::string::npos) continue;
      const std::string mac = line.substr(7, space - 7);
      bluetooth_devices_.push_back({mac, line.substr(space + 1), connected.find(mac) != std::string::npos});
    }
    std::string body = std::string("Bluetooth: ") + (bluetooth_blocked_ ? "Off" : "On") + " (click to toggle)";
    for (const BluetoothDevice& device : bluetooth_devices_)
      body += "\n" + device.name + (device.connected ? " (connected)" : "");
    if (bluetooth_devices_.empty()) body += "\nNo paired devices";
    open_action_panel(InfoAction::Bluetooth, "Bluetooth", body);
  }

  void open_media_panel() {
    open_action_panel(InfoAction::Media, "Media", media_title_.empty() ? "No active player" :
                      media_title_ + "\nPrevious   Play/Pause   Next");
  }

  void open_keyboard_panel() {
    std::string body;
    for (std::size_t index = 0; index < keyboard_layouts_.size(); ++index)
      body += (index ? "\n" : "") + std::string(index < keyboard_layouts_.size() && keyboard_layouts_[index] == keyboard_layout_ ? "• " : "  ") + keyboard_layouts_[index];
    open_action_panel(InfoAction::Keyboard, "Keyboard layout", body.empty() ? "No layouts configured" : body);
  }

  void open_theme_panel() {
    std::string body;
    for (std::size_t index = 0; index < theme_names_.size(); ++index)
      body += (index ? "\n" : "") + std::string(static_cast<int>(index) == theme_index_ ? "• " : "  ") + theme_names_[index];
    open_action_panel(InfoAction::Theme, "Theme", body);
  }

  void handle_side_panel_button(const XButtonEvent& event) {
    if (event.window != side_panel_window_) return;
    if (event.button == Button3) { close_side_panel(); return; }
    if (event.button == Button4 || event.button == Button5) {
      const int delta = event.button == Button4 ? -3 * (kBarHeight + 2) : 3 * (kBarHeight + 2);
      const int rows = side_panel_ == SidePanel::Notifications ? static_cast<int>(notifications_.size() * 2) :
                       side_panel_ == SidePanel::Todos ? static_cast<int>(todos_.size()) :
                       side_panel_ == SidePanel::Agents ? static_cast<int>(agents_.size()) : static_cast<int>(side_panel_rows_.size());
      const int visible = std::max(1, (monitor(side_panel_monitor_).height - 3 * kBarHeight) / (kBarHeight + 2));
      side_panel_scroll_offset_ = std::clamp(side_panel_scroll_offset_ + delta, 0, std::max(0, (rows - visible) * (kBarHeight + 2)));
      draw_side_panel();
      return;
    }
    if (event.y < kBarHeight && event.button == Button1 && event.x >= 260 && side_panel_ == SidePanel::Notifications) {
      for (const Notification& notification : notifications_) emit_notification_closed(notification.id, 2);
      notifications_.clear();
      draw_side_panel(); draw_docks(); return;
    }
    if (side_panel_ == SidePanel::Agents && event.y < kBarHeight && event.button == Button1 && event.x >= 260) {
      for (const AgentStatus& agent : agents_) if (agent.from_file && !agent.file_path.empty()) unlink(agent.file_path.c_str());
      refresh_agents(); draw_side_panel(); draw_docks(); return;
    }
    const int row = static_cast<int>((event.y - kBarHeight - 6 + side_panel_scroll_offset_) / (kBarHeight + 2));
    if (row < 0) return;
    if (side_panel_ == SidePanel::Info && event.button == Button1) {
      if (info_action_ == InfoAction::Wifi) {
        if (row == 0) { spawn_argv({"rfkill", wifi_blocked_ ? "unblock" : "block", "wifi"}); wifi_blocked_ = !wifi_blocked_; }
        else if (static_cast<std::size_t>(row - 1) < wifi_networks_.size() && !wifi_networks_[row - 1].active)
          spawn_argv({"nmcli", "device", "wifi", "connect", wifi_networks_[row - 1].ssid});
      } else if (info_action_ == InfoAction::Bluetooth) {
        if (row == 0) { spawn_argv({"rfkill", bluetooth_blocked_ ? "unblock" : "block", "bluetooth"}); bluetooth_blocked_ = !bluetooth_blocked_; }
        else if (static_cast<std::size_t>(row - 1) < bluetooth_devices_.size()) {
          const BluetoothDevice& device = bluetooth_devices_[row - 1];
          spawn_argv({"bluetoothctl", device.connected ? "disconnect" : "connect", device.mac});
        }
      } else if (info_action_ == InfoAction::Media && row == 1) {
        const int third = event.x / 120;
        spawn_argv({"playerctl", third == 0 ? "previous" : third == 2 ? "next" : "play-pause"});
      } else if (info_action_ == InfoAction::Keyboard && static_cast<std::size_t>(row) < keyboard_layouts_.size()) {
        XkbLockGroup(display_, XkbUseCoreKbd, row);
        widgets_refreshed_ = 0;
      } else if (info_action_ == InfoAction::Theme && row >= 0 && static_cast<std::size_t>(row) < theme_palettes_.size()) {
        const int direction = row - theme_index_;
        if (direction) cycle_theme(direction);
      } else if (info_action_ == InfoAction::Git) {
        const char* project = std::getenv("MEPWM_PROJECT_DIR");
        if (project && *project) {
          const pid_t child = fork();
          if (child == 0) { setsid(); chdir(project); execl("/bin/sh", "sh", "-c", config_.terminal.c_str(), static_cast<char*>(nullptr)); _exit(127); }
        }
      }
      widgets_refreshed_ = 0;
      return;
    }
    if (side_panel_ == SidePanel::Notifications && static_cast<std::size_t>(row / 2) < notifications_.size()) {
      const std::size_t notification_index = static_cast<std::size_t>(row / 2);
      if (event.button == Button2) {
        const unsigned int id = notifications_[notification_index].id;
        notifications_.erase(notifications_.begin() + notification_index);
        emit_notification_closed(id, 2);
      } else {
        notifications_[notification_index].unread = false;
      }
    } else if (side_panel_ == SidePanel::Agents && static_cast<std::size_t>(row) < agents_.size()) {
      if (event.button == Button2 && agents_[row].from_file && !agents_[row].file_path.empty()) {
        unlink(agents_[row].file_path.c_str());
        refresh_agents(); draw_side_panel(); draw_docks(); return;
      }
      for (pid_t agent_pid = agents_[row].pid, hop = 0; agent_pid > 1 && hop < 32; agent_pid = parent_pid(agent_pid), ++hop)
        for (const auto& entry : window_state_) {
          Atom type = None;
          int format = 0;
          unsigned long count = 0, after = 0;
          unsigned char* value = nullptr;
          if (XGetWindowProperty(display_, entry.first, net_wm_pid_atom_, 0, 1, False, XA_CARDINAL,
                                 &type, &format, &count, &after, &value) == Success && value) {
            const pid_t window_pid = static_cast<pid_t>(*reinterpret_cast<unsigned long*>(value));
            XFree(value);
            if (window_pid == agent_pid) { focus(entry.first); close_side_panel(); return; }
          }
        }
    }
    draw_side_panel();
    draw_docks();
  }

  // (Re)creates a dock's off-screen buffer only when its size actually
  // changed, so steady-state redraws reuse the same pixmap.
  void ensure_dock_buffer(Pixmap& pixmap, int& buffer_width, int& buffer_height, int width, int height) {
    width = std::max(1, width);
    height = std::max(1, height);
    if (pixmap != None && buffer_width == width && buffer_height == height) return;
    if (pixmap != None) XFreePixmap(display_, pixmap);
    pixmap = XCreatePixmap(display_, root_, width, height, DefaultDepth(display_, screen_));
    buffer_width = width;
    buffer_height = height;
  }

  void draw_docks() {
    static const std::array<const char*, 8> left_labels = {
        kIconFirefox, kIconTerminal, kIconInkscape, kIconGimp,
        kIconLibreOffice, kIconVsCode, kIconEmacs, kIconNeovim,
    };
    static const std::array<const char*, 3> right_labels = {kIconBell, kIconTodo, kIconAgents};
    for (std::size_t index = 0; index < monitors_.size() && index < docks_.size(); ++index) {
      const Monitor& target = monitors_[index];
      DockWindows& dock = docks_[index];
      // The vertical docks sit between the top and bottom bars now, so they
      // are shorter than the monitor by one bar-height on each end.
      const int side_height = std::max(1, target.height - 2 * kBarHeight);
      const int bottom_width = std::max(1, target.width);
      ensure_dock_buffer(dock.left_buffer, dock.left_buffer_width, dock.left_buffer_height, kDockWidth, side_height);
      ensure_dock_buffer(dock.right_buffer, dock.right_buffer_width, dock.right_buffer_height, kDockWidth, side_height);
      ensure_dock_buffer(dock.bottom_buffer, dock.bottom_buffer_width, dock.bottom_buffer_height, bottom_width, kBarHeight);
      XSetForeground(display_, bar_gc_, bar_background_.pixel);
      XFillRectangle(display_, dock.left_buffer, bar_gc_, 0, 0, kDockWidth, side_height);
      XFillRectangle(display_, dock.right_buffer, bar_gc_, 0, 0, kDockWidth, side_height);
      XFillRectangle(display_, dock.bottom_buffer, bar_gc_, 0, 0, bottom_width, kBarHeight);
      for (std::size_t row = 0; row < left_labels.size(); ++row)
        draw_dock_cell(dock.left_buffer, static_cast<int>(row) * kDockWidth, kDockWidth, kDockWidth, left_labels[row]);
      const int unread = static_cast<int>(std::count_if(notifications_.begin(), notifications_.end(), [](const Notification& item) {
        return item.unread;
      }));
      load_todos();
      const std::array<bool, 3> highlighted = {unread > 0, !todos_.empty(), agent_needs_input_ > 0};
      for (std::size_t row = 0; row < right_labels.size(); ++row)
        draw_dock_cell(dock.right_buffer, static_cast<int>(row) * kDockWidth, kDockWidth, kDockWidth,
                       right_labels[row], highlighted[row]);
      draw_bottom_widgets(index);
      // The os and info icons anchor the bottom bar's outer corners, mirroring
      // the launcher/layout corners of the top bar.
      draw_dock_cell_h(dock.bottom_buffer, 0, kDockWidth, kBarHeight, kIconNixOs);
      draw_dock_cell_h(dock.bottom_buffer, bottom_width - kDockWidth, kDockWidth, kBarHeight, kIconInfo);
      XCopyArea(display_, dock.left_buffer, dock.left, bar_gc_, 0, 0, kDockWidth, side_height, 0, 0);
      XCopyArea(display_, dock.right_buffer, dock.right, bar_gc_, 0, 0, kDockWidth, side_height, 0, 0);
      XCopyArea(display_, dock.bottom_buffer, dock.bottom, bar_gc_, 0, 0, bottom_width, kBarHeight, 0, 0);
    }
    XFlush(display_);
    if (startup_timer_) startup_timer_->checkpoint("draw docks");
  }

  int dock_monitor(Window window) const {
    for (std::size_t index = 0; index < docks_.size(); ++index) {
      if (docks_[index].left == window || docks_[index].bottom == window || docks_[index].right == window)
        return static_cast<int>(index);
    }
    return -1;
  }

  void adjust_backlight(int delta) {
    const long value = read_long(backlight_path_);
    const long maximum = read_long(backlight_max_path_);
    if (value < 0 || maximum <= 0) return;
    const long next = std::clamp(value + maximum * delta / 100, 1L, maximum);
    std::ofstream file(backlight_path_);
    if (file) file << next << '\n';
    widgets_refreshed_ = 0;
  }

  // Built-ins plus anything mwm.theme() registered from Lua; reset back to
  // just the built-ins on reload_lua() (see reset_theme_palettes()).
  void apply_current_theme() {
    if (theme_palettes_.empty()) return;
    theme_index_ = std::clamp(theme_index_, 0, static_cast<int>(theme_palettes_.size()) - 1);
    const std::array<std::string, 3>& palette = theme_palettes_[static_cast<std::size_t>(theme_index_)];
    XftColorFree(display_, DefaultVisual(display_, screen_), DefaultColormap(display_, screen_), &bar_foreground_);
    XftColorFree(display_, DefaultVisual(display_, screen_), DefaultColormap(display_, screen_), &bar_background_);
    XftColorFree(display_, DefaultVisual(display_, screen_), DefaultColormap(display_, screen_), &bar_selected_);
    XftColorAllocName(display_, DefaultVisual(display_, screen_), DefaultColormap(display_, screen_), palette[0].c_str(), &bar_foreground_);
    XftColorAllocName(display_, DefaultVisual(display_, screen_), DefaultColormap(display_, screen_), palette[1].c_str(), &bar_background_);
    XftColorAllocName(display_, DefaultVisual(display_, screen_), DefaultColormap(display_, screen_), palette[2].c_str(), &bar_selected_);
    draw_bar(); draw_docks();
    refresh_wallpaper();
  }

  void cycle_theme(int direction) {
    if (theme_palettes_.empty()) return;
    theme_index_ = (theme_index_ + direction + static_cast<int>(theme_palettes_.size())) % static_cast<int>(theme_palettes_.size());
    apply_current_theme();
  }

  void reset_theme_palettes() {
    theme_palettes_ = {{{"#f8f8f2", "#202124", "#5294e2"}}, {{"#202124", "#f4f4f4", "#3971ed"}}, {{"#d8dee9", "#2e3440", "#88c0d0"}}};
    theme_names_ = {"dark", "blue", "nord"};
    if (theme_index_ >= static_cast<int>(theme_palettes_.size())) theme_index_ = 0;
  }

  static bool color_is_light(const std::string& hex) {
    if (hex.size() != 7 || hex[0] != '#') return false;
    int r = 0, g = 0, b = 0;
    try {
      r = std::stoi(hex.substr(1, 2), nullptr, 16);
      g = std::stoi(hex.substr(3, 2), nullptr, 16);
      b = std::stoi(hex.substr(5, 2), nullptr, 16);
    } catch (const std::exception&) {
      return false;
    }
    return (0.299 * r + 0.587 * g + 0.114 * b) > 140.0;
  }

  // Derived from the active palette's background (palette[1], see
  // apply_current_theme()) rather than a hardcoded theme index, so
  // Lua-registered themes (mwm.theme()) pick the right wallpaper set too.
  bool theme_is_light() const {
    if (theme_palettes_.empty()) return false;
    const std::size_t index = static_cast<std::size_t>(
        std::clamp(theme_index_, 0, static_cast<int>(theme_palettes_.size()) - 1));
    return color_is_light(theme_palettes_[index][1]);
  }

  // No caching: this only runs on theme switches (rare -- a keypress or a
  // reload), so re-reading a small directory each time is cheap and avoids
  // having to invalidate a cache when mwm.set_wallpapers() changes the dirs.
  std::vector<std::string> scan_wallpaper_dir(const std::string& raw_dir) const {
    std::vector<std::string> result;
    if (raw_dir.empty()) return result;
    std::filesystem::path dir(raw_dir);
    if (raw_dir == "~" || raw_dir.rfind("~/", 0) == 0) {
      if (const char* home = std::getenv("HOME"); home && *home)
        dir = std::filesystem::path(home) / raw_dir.substr(raw_dir == "~" ? 1 : 2);
    }
    DIR* directory = opendir(dir.c_str());
    if (!directory) return result;
    while (dirent* entry = readdir(directory)) {
      const std::string name(entry->d_name);
      const std::size_t dot = name.find_last_of('.');
      if (dot == std::string::npos) continue;
      std::string extension = name.substr(dot + 1);
      std::transform(extension.begin(), extension.end(), extension.begin(),
                      [](unsigned char c) { return std::tolower(c); });
      if (extension != "png" && extension != "jpg" && extension != "jpeg" &&
          extension != "bmp" && extension != "gif" && extension != "webp") continue;
      result.push_back((dir / name).string());
    }
    closedir(directory);
    std::sort(result.begin(), result.end());
    return result;
  }

  void set_random_wallpaper(bool light) {
    const std::vector<std::string> wallpapers =
        scan_wallpaper_dir(light ? config_.wallpaper_dir_light : config_.wallpaper_dir_dark);
    if (wallpapers.empty()) return;
    std::uniform_int_distribution<std::size_t> distribution(0, wallpapers.size() - 1);
    const std::string& path = wallpapers[distribution(wallpaper_rng_)];
    const pid_t child = fork();
    if (child < 0) {
      std::cerr << "mepwm: could not set wallpaper: " << std::strerror(errno) << '\n';
      return;
    }
    if (child == 0) {
      setsid();
      execlp("feh", "feh", "--bg-fill", path.c_str(), static_cast<char*>(nullptr));
      _exit(127);
    }
    signal(SIGCHLD, SIG_IGN);
  }

  // Only rerolls the wallpaper when the light/dark bucket actually changes,
  // so cycling between two dark themes (or a plain config reload) leaves
  // the current wallpaper alone.
  void refresh_wallpaper() {
    const bool light = theme_is_light();
    if (wallpaper_is_light_.has_value() && *wallpaper_is_light_ == light) return;
    wallpaper_is_light_ = light;
    set_random_wallpaper(light);
  }

  void cycle_keyboard_layout() {
    if (keyboard_layouts_.size() < 2) return;
    XkbStateRec state{};
    if (XkbGetState(display_, XkbUseCoreKbd, &state) == Success)
      XkbLockGroup(display_, XkbUseCoreKbd, (state.group + 1) % keyboard_layouts_.size());
    widgets_refreshed_ = 0;
  }

  void handle_bottom_widget(const XButtonEvent& event) {
    const auto hit = std::find_if(bottom_widget_hits_.begin(), bottom_widget_hits_.end(), [&](const WidgetHit& item) {
      return event.x >= item.left && event.x < item.right;
    });
    if (hit == bottom_widget_hits_.end()) return;
    if (hit->id == "backlight") {
      if (event.button == Button1) slider_visible_ && slider_kind_ == SliderKind::Backlight ? close_slider_popup() : open_slider_popup(SliderKind::Backlight);
      if (event.button == Button4) adjust_backlight(5);
      if (event.button == Button3 || event.button == Button5) adjust_backlight(-5);
    } else if (hit->id == "volume") {
      if (event.button == Button1) slider_visible_ && slider_kind_ == SliderKind::Volume ? close_slider_popup() : open_slider_popup(SliderKind::Volume);
      if (event.button == Button2) spawn_command("amixer -q set Master toggle");
      if (event.button == Button4) spawn_command("amixer -q set Master 5%+ unmute");
      if (event.button == Button3 || event.button == Button5) spawn_command("amixer -q set Master 5%- unmute");
      widgets_refreshed_ = 0;
    } else if (hit->id == "mic") {
      if (event.button == Button1) slider_visible_ && slider_kind_ == SliderKind::Microphone ? close_slider_popup() : open_slider_popup(SliderKind::Microphone);
      if (event.button == Button2) spawn_command("amixer -q set Capture toggle");
      if (event.button == Button4) spawn_command("amixer -q set Capture 5%+ unmute");
      if (event.button == Button3 || event.button == Button5) spawn_command("amixer -q set Capture 5%- unmute");
      widgets_refreshed_ = 0;
    } else if (hit->id == "media") {
      if (event.button == Button1) open_media_panel();
      if (event.button == Button2) spawn_command("playerctl play-pause");
      if (event.button == Button4) spawn_command("playerctl next");
      if (event.button == Button5) spawn_command("playerctl previous");
      widgets_refreshed_ = 0;
    } else if (hit->id == "wifi" && event.button == Button1) {
      open_wifi_panel();
    } else if (hit->id == "bluetooth" && event.button == Button1) {
      open_bluetooth_panel();
    } else if (hit->id == "theme") {
      if (event.button == Button1 || event.button == Button4) cycle_theme(1);
      if (event.button == Button3 || event.button == Button5) cycle_theme(-1);
      if (event.button == Button2) open_theme_panel();
    } else if (hit->id == "keyboard") {
      if (event.button == Button1) open_keyboard_panel();
      if (event.button == Button4) cycle_keyboard_layout();
      if (event.button == Button5 && keyboard_layouts_.size() > 1) {
        XkbStateRec state{};
        if (XkbGetState(display_, XkbUseCoreKbd, &state) == Success)
          XkbLockGroup(display_, XkbUseCoreKbd, (state.group + keyboard_layouts_.size() - 1) % keyboard_layouts_.size());
        widgets_refreshed_ = 0;
      }
    } else if (event.button == Button1) {
      if (hit->id == "git") open_action_panel(InfoAction::Git, "Git", git_status_.empty() ? "No repository selected" : git_status_ + "\nClick to open terminal here");
      if (hit->id == "battery") open_info_panel("Battery", battery_percent_ < 0 ? "No battery detected" : std::to_string(battery_percent_) + "%");
      if (hit->id == "cpu") open_info_panel("CPU", cpu_percent_ < 0 ? "Collecting samples" : std::to_string(cpu_percent_) + "% in use");
      if (hit->id == "memory") open_info_panel("Memory", std::to_string(mem_percent_) + "% in use");
      if (hit->id == "disk") open_info_panel("Disk", std::to_string(disk_percent_) + "% in use");
    }
  }

  void handle_dock_button(const XButtonEvent& event) {
    const int index = dock_monitor(event.window);
    if (index < 0) return;
    current_monitor_ = static_cast<std::size_t>(index);
    const DockWindows& dock = docks_[current_monitor_];
    if (event.window == dock.left && event.button == Button1) {
      switch (event.y / kDockWidth) {
        case 0: spawn_command("firefox"); break;
        case 1: spawn_terminal(); break;
        case 2: spawn_command("inkscape"); break;
        case 3: spawn_command("gimp"); break;
        case 4: spawn_command("libreoffice"); break;
        case 5: spawn_command("code"); break;
        case 6: spawn_command("if command -v nix >/dev/null 2>&1; then exec nix run 'git:jordanschupbach/emc' --refresh; else exec emacs; fi"); break;
        case 7: spawn_command("${TERMINAL:-xterm} -e nvim"); break;
      }
    } else if (event.window == dock.bottom) {
      // The info icon lives in the bottom bar's right-hand corner now.
      if (event.button == Button1 && event.x >= monitors_[current_monitor_].width - kDockWidth) {
        toggle_side_panel(SidePanel::Help);
      } else {
        handle_bottom_widget(event);
      }
    } else if (event.window == dock.right && event.button == Button1) {
      const int row = event.y / kDockWidth;
      if (row == 0) toggle_side_panel(SidePanel::Notifications);
      if (row == 1) toggle_side_panel(SidePanel::Todos);
      if (row == 2) toggle_side_panel(SidePanel::Agents);
    }
    draw_docks();
  }

  // Vimium-style "click anything" overlay bound to Super+f. Builds a hint for
  // every bar/dock element and every currently visible window, then lets the
  // user type a short label to trigger it. Selecting a hint replays a
  // synthetic click through the same handlers a real click would use
  // (handle_button/handle_dock_button), so hint targets can never drift out
  // of sync with actual click behavior.
  static std::string hint_label(std::size_t index, std::size_t total) {
    static constexpr char kCharset[] = "asdfghjklqwertyuiopzxcvbnm";
    constexpr std::size_t kBase = 26;
    if (total <= kBase) return std::string(1, kCharset[index]);
    const std::size_t first = std::min(index / kBase, kBase - 1);
    return std::string(1, kCharset[first]) + kCharset[index % kBase];
  }

  int hint_text_width(const std::string& value) const {
    XGlyphInfo extent{};
    XftTextExtentsUtf8(display_, hint_font_, reinterpret_cast<const FcChar8*>(value.data()),
                       static_cast<int>(value.size()), &extent);
    return extent.xOff;
  }

  void build_hints() {
    hints_.clear();
    struct Candidate {
      int x, y;
      Window target;
      int click_x, click_y;
    };
    std::vector<Candidate> candidates;

    if (bar_visible_ && bar_ != None) {
      // Bar-relative x now equals screen x, since bar_ starts at x=0.
      candidates.push_back({4, 4, bar_, kDockWidth / 2, kBarHeight / 2});
      for (int index = 0; index < kWorkspaceCount; ++index) {
        const int x = kDockWidth + index * 84;
        candidates.push_back({x + 4, 4, bar_, x + 10, kBarHeight / 2});
      }
      const int mode_x = kDockWidth + 756;
      candidates.push_back({mode_x + 4, 4, bar_, mode_x + 10, kBarHeight / 2});
      for (const BarHit& hit : task_hits_) {
        candidates.push_back({hit.left + 4, 4, bar_, (hit.left + hit.right) / 2, kBarHeight / 2});
      }
      const int layout_x = DisplayWidth(display_, screen_) - kDockWidth;
      candidates.push_back({layout_x + 4, 4, bar_, layout_x + kDockWidth / 2, kBarHeight / 2});
    }

    for (std::size_t index = 0; index < monitors_.size() && index < docks_.size(); ++index) {
      const Monitor& target_monitor = monitors_[index];
      const DockWindows& dock = docks_[index];
      const int dock_y = target_monitor.y + kBarHeight;
      for (int row = 0; row < 8; ++row) {
        candidates.push_back({target_monitor.x + 4, dock_y + row * kDockWidth + 4, dock.left, kDockWidth / 2,
                              row * kDockWidth + kDockWidth / 2});
      }
      const int dock_right_x = target_monitor.x + target_monitor.width - kDockWidth;
      for (int row = 0; row < 3; ++row) {
        candidates.push_back({dock_right_x + 4, dock_y + row * kDockWidth + 4, dock.right,
                              kDockWidth / 2, row * kDockWidth + kDockWidth / 2});
      }
      draw_bottom_widgets(index);
      const int bottom_y = target_monitor.y + target_monitor.height - kBarHeight;
      candidates.push_back({target_monitor.x + 4, bottom_y + 4, dock.bottom, kDockWidth / 2, kBarHeight / 2});
      for (const WidgetHit& hit : bottom_widget_hits_) {
        candidates.push_back({target_monitor.x + hit.left + 4, bottom_y + 4, dock.bottom,
                              (hit.left + hit.right) / 2, kBarHeight / 2});
      }
      candidates.push_back({target_monitor.x + target_monitor.width - kDockWidth + 4, bottom_y + 4, dock.bottom,
                            target_monitor.width - kDockWidth / 2, kBarHeight / 2});
    }

    std::vector<Window> windows;
    collect_windows(workspace().root.get(), windows);
    windows.insert(windows.end(), workspace().floating.begin(), workspace().floating.end());
    for (Window window : windows) {
      XWindowAttributes attributes;
      if (!XGetWindowAttributes(display_, window, &attributes) || attributes.map_state != IsViewable) continue;
      candidates.push_back({attributes.x + 4, attributes.y + 4, window, attributes.width / 2, attributes.height / 2});
    }

    std::sort(candidates.begin(), candidates.end(), [](const Candidate& left, const Candidate& right) {
      return left.y != right.y ? left.y < right.y : left.x < right.x;
    });

    hints_.reserve(candidates.size());
    for (std::size_t index = 0; index < candidates.size(); ++index) {
      const Candidate& candidate = candidates[index];
      Hint hint;
      hint.label = hint_label(index, candidates.size());
      hint.x = candidate.x;
      hint.y = candidate.y;
      hint.target = candidate.target;
      hint.click_x = candidate.click_x;
      hint.click_y = candidate.click_y;
      hints_.push_back(std::move(hint));
    }
  }

  void draw_hint_chip(std::size_t index) {
    const Hint& hint = hints_[index];
    const Window window = hint_windows_[index];
    if (window == None) return;
    XSetForeground(display_, bar_gc_, hint_background_.pixel);
    XFillRectangle(display_, window, bar_gc_, 0, 0, hint.width, hint.height);
    XftDraw* draw = XftDrawCreate(display_, window, DefaultVisual(display_, screen_), DefaultColormap(display_, screen_));
    if (!draw) return;
    const int baseline = (hint.height + hint_font_->ascent - hint_font_->descent) / 2;
    int cursor = 4;
    for (std::size_t character = 0; character < hint.label.size(); ++character) {
      const XftColor& color = character < hint_query_.size() ? hint_matched_ : hint_foreground_;
      XftDrawStringUtf8(draw, &color, hint_font_, cursor, baseline,
                        reinterpret_cast<const FcChar8*>(&hint.label[character]), 1);
      XGlyphInfo extent{};
      XftTextExtentsUtf8(display_, hint_font_, reinterpret_cast<const FcChar8*>(&hint.label[character]), 1, &extent);
      cursor += extent.xOff;
    }
    XftDrawDestroy(draw);
  }

  void show_hint_windows() {
    hint_windows_.assign(hints_.size(), None);
    for (std::size_t index = 0; index < hints_.size(); ++index) {
      Hint& hint = hints_[index];
      hint.width = hint_text_width(hint.label) + 8;
      hint.height = hint_font_->ascent + hint_font_->descent + 6;
      XSetWindowAttributes attributes{};
      attributes.override_redirect = True;
      attributes.background_pixel = hint_background_.pixel;
      attributes.border_pixel = bar_selected_.pixel;
      Window window = XCreateWindow(display_, root_, hint.x, hint.y, hint.width, hint.height, 1,
                                    DefaultDepth(display_, screen_), CopyFromParent, DefaultVisual(display_, screen_),
                                    CWOverrideRedirect | CWBackPixel | CWBorderPixel, &attributes);
      hint_windows_[index] = window;
      XMapRaised(display_, window);
      draw_hint_chip(index);
    }
    XFlush(display_);
  }

  void close_hints() {
    if (hints_visible_) {
      hints_visible_ = false;
      XUngrabKeyboard(display_, CurrentTime);
    }
    for (Window window : hint_windows_)
      if (window != None) XDestroyWindow(display_, window);
    hint_windows_.clear();
    hints_.clear();
    hint_query_.clear();
    XFlush(display_);
  }

  void toggle_hints() {
    if (hints_visible_) { close_hints(); return; }
    if (launcher_visible_ || slider_visible_ || bar_ == None) return;
    draw_bar();
    draw_docks();
    build_hints();
    if (hints_.empty()) return;
    if (XGrabKeyboard(display_, root_, False, GrabModeAsync, GrabModeAsync, CurrentTime) != GrabSuccess) return;
    hints_visible_ = true;
    hint_query_.clear();
    show_hint_windows();
  }

  // Dispatches a click at the hint's target exactly as handle_button/
  // handle_dock_button would receive it from the X server, so the action
  // taken always matches what a literal click at that spot would do.
  void trigger_hint(const Hint& hint) {
    const Window target = hint.target;
    const int click_x = hint.click_x;
    const int click_y = hint.click_y;
    const unsigned int button = hint.button;
    close_hints();
    XButtonEvent synthetic{};
    synthetic.type = ButtonPress;
    synthetic.display = display_;
    synthetic.window = target;
    synthetic.root = root_;
    synthetic.x = click_x;
    synthetic.y = click_y;
    synthetic.button = button;
    synthetic.state = 0;
    synthetic.time = CurrentTime;
    synthetic.same_screen = True;
    if (dock_monitor(target) >= 0) handle_dock_button(synthetic);
    else handle_button(synthetic);
  }

  void refresh_hint_visibility() {
    const Hint* exact = nullptr;
    for (std::size_t index = 0; index < hints_.size(); ++index) {
      const Hint& hint = hints_[index];
      const bool visible = hint.label.compare(0, hint_query_.size(), hint_query_) == 0;
      if (visible) {
        draw_hint_chip(index);
        XMapRaised(display_, hint_windows_[index]);
        if (hint.label == hint_query_) exact = &hint;
      } else {
        XUnmapWindow(display_, hint_windows_[index]);
      }
    }
    XFlush(display_);
    if (exact) trigger_hint(*exact);
  }

  void handle_hint_key(const XKeyEvent& event) {
    char text[8];
    KeySym key = NoSymbol;
    const int length = XLookupString(const_cast<XKeyEvent*>(&event), text, sizeof(text), &key, nullptr);
    if (key == XK_Escape) { close_hints(); return; }
    if (key == XK_BackSpace) {
      if (!hint_query_.empty()) { hint_query_.pop_back(); refresh_hint_visibility(); }
      return;
    }
    for (int index = 0; index < length; ++index) {
      const char typed = static_cast<char>(std::tolower(static_cast<unsigned char>(text[index])));
      if (typed < 'a' || typed > 'z') continue;
      const std::string candidate = hint_query_ + typed;
      const bool any_match = std::any_of(hints_.begin(), hints_.end(), [&](const Hint& hint) {
        return hint.label.compare(0, candidate.size(), candidate) == 0;
      });
      if (any_match) hint_query_ = candidate;
    }
    refresh_hint_visibility();
  }

  void create_bar() {
    const int width = std::max(1, DisplayWidth(display_, screen_));
    bar_ = XCreateSimpleWindow(display_, root_, 0, 0, width, kBarHeight,
                               0, BlackPixel(display_, screen_), BlackPixel(display_, screen_));
    XSetWindowAttributes attributes{};
    attributes.override_redirect = True;
    XChangeWindowAttributes(display_, bar_, CWOverrideRedirect, &attributes);
    XSelectInput(display_, bar_, ExposureMask | ButtonPressMask);
    XStoreName(display_, bar_, "mepwm-bar");
    XDefineCursor(display_, bar_, cursor_);
    bar_gc_ = XCreateGC(display_, bar_, 0, nullptr);
    bar_font_ = XftFontOpenName(display_, screen_, "sans-16");
    if (!bar_font_) throw std::runtime_error("could not open an Xft bar font");
    icon_font_ = XftFontOpenName(display_, screen_, "UbuntuMono Nerd Font Mono:size=34");
    XftColorAllocName(display_, DefaultVisual(display_, screen_), DefaultColormap(display_, screen_),
                      "#f8f8f2", &bar_foreground_);
    XftColorAllocName(display_, DefaultVisual(display_, screen_), DefaultColormap(display_, screen_),
                      "#202124", &bar_background_);
    XftColorAllocName(display_, DefaultVisual(display_, screen_), DefaultColormap(display_, screen_),
                      "#5294e2", &bar_selected_);
    hint_font_ = XftFontOpenName(display_, screen_, "sans-11");
    if (!hint_font_) hint_font_ = bar_font_;
    XftColorAllocName(display_, DefaultVisual(display_, screen_), DefaultColormap(display_, screen_),
                      "#ffd76e", &hint_background_);
    XftColorAllocName(display_, DefaultVisual(display_, screen_), DefaultColormap(display_, screen_),
                      "#202124", &hint_foreground_);
    XftColorAllocName(display_, DefaultVisual(display_, screen_), DefaultColormap(display_, screen_),
                      "#c23616", &hint_matched_);
    XMapRaised(display_, bar_);
    draw_bar();
  }

  XftFont* fallback_font(FcChar32 codepoint) {
    const auto cached = fallback_fonts_.find(codepoint);
    if (cached != fallback_fonts_.end()) return cached->second;
    if (icon_font_ && XftCharExists(display_, icon_font_, codepoint)) {
      fallback_fonts_.emplace(codepoint, icon_font_);
      return icon_font_;
    }
    FcPattern* pattern = FcNameParse(reinterpret_cast<const FcChar8*>("sans-16"));
    FcCharSet* charset = FcCharSetCreate();
    FcCharSetAddChar(charset, codepoint);
    FcPatternAddCharSet(pattern, FC_CHARSET, charset);
    FcCharSetDestroy(charset);
    FcConfigSubstitute(nullptr, pattern, FcMatchPattern);
    FcDefaultSubstitute(pattern);
    FcResult result;
    FcPattern* matched = XftFontMatch(display_, screen_, pattern, &result);
    FcPatternDestroy(pattern);
    XftFont* font = matched ? XftFontOpenPattern(display_, matched) : nullptr;
    if (!font) font = bar_font_;
    fallback_fonts_.emplace(codepoint, font);
    return font;
  }

  static std::size_t utf8_codepoint(const std::string& text, std::size_t offset, FcChar32* codepoint) {
    const unsigned char first = static_cast<unsigned char>(text[offset]);
    if (first < 0x80) { *codepoint = first; return 1; }
    const int bytes = (first & 0xe0) == 0xc0 ? 2 : (first & 0xf0) == 0xe0 ? 3 :
                      (first & 0xf8) == 0xf0 ? 4 : 1;
    if (offset + static_cast<std::size_t>(bytes) > text.size()) { *codepoint = 0xfffd; return 1; }
    FcChar32 value = first & ((1u << (7 - bytes)) - 1);
    for (int index = 1; index < bytes; ++index) {
      const unsigned char next = static_cast<unsigned char>(text[offset + index]);
      if ((next & 0xc0) != 0x80) { *codepoint = 0xfffd; return 1; }
      value = (value << 6) | (next & 0x3f);
    }
    *codepoint = value;
    return bytes;
  }

  int text_width(const std::string& value) {
    int width = 0;
    for (std::size_t offset = 0; offset < value.size();) {
      FcChar32 codepoint;
      const std::size_t bytes = utf8_codepoint(value, offset, &codepoint);
      XGlyphInfo extent{};
      XftTextExtentsUtf8(display_, XftCharExists(display_, bar_font_, codepoint) ? bar_font_ : fallback_font(codepoint),
                          reinterpret_cast<const FcChar8*>(value.data() + offset), static_cast<int>(bytes), &extent);
      width += extent.xOff;
      offset += bytes;
    }
    return width;
  }

  void draw_text(int x, const std::string& value, const XftColor& color) {
    int cursor = x;
    for (std::size_t offset = 0; offset < value.size();) {
      FcChar32 codepoint;
      const std::size_t bytes = utf8_codepoint(value, offset, &codepoint);
      XftFont* font = XftCharExists(display_, bar_font_, codepoint) ? bar_font_ : fallback_font(codepoint);
      XftDrawStringUtf8(bar_xft_draw_, &color, font, cursor,
                        (kBarHeight + font->ascent - font->descent) / 2,
                        reinterpret_cast<const FcChar8*>(value.data() + offset), static_cast<int>(bytes));
      XGlyphInfo extent{};
      XftTextExtentsUtf8(display_, font, reinterpret_cast<const FcChar8*>(value.data() + offset),
                          static_cast<int>(bytes), &extent);
      cursor += extent.xOff;
      offset += bytes;
    }
  }

  void draw_bar() {
    if (!bar_) return;
    const int width = std::max(1, DisplayWidth(display_, screen_));
    if (bar_pixmap_width_ != width) {
      if (bar_xft_draw_) XftDrawDestroy(bar_xft_draw_);
      if (bar_pixmap_) XFreePixmap(display_, bar_pixmap_);
      bar_pixmap_ = XCreatePixmap(display_, bar_, width, kBarHeight, DefaultDepth(display_, screen_));
      bar_xft_draw_ = XftDrawCreate(display_, bar_pixmap_, DefaultVisual(display_, screen_),
                                    DefaultColormap(display_, screen_));
      bar_pixmap_width_ = width;
    }
    XSetForeground(display_, bar_gc_, bar_background_.pixel);
    XFillRectangle(display_, bar_pixmap_, bar_gc_, 0, 0, width, kBarHeight);

    auto text = [&](int x, const std::string& value, const XftColor& color) { draw_text(x, value, color); };

    // The launcher and layout-mode toggle live in the outermost kDockWidth
    // cells so the bar can span the full display width; everything else
    // that used to be bar-relative x=0 now starts after the launcher cell.
    draw_dock_cell_h(bar_pixmap_, 0, kDockWidth, kBarHeight, kIconLauncher);
    const char* layout_icon = workspace().mode == LayoutMode::Manual ? kIconLayoutOther :
                              workspace().mode == LayoutMode::MasterStack ? kIconLayoutTile : kIconLayoutMonocle;
    draw_dock_cell_h(bar_pixmap_, width - kDockWidth, kDockWidth, kBarHeight, layout_icon);

    const int content_x = kDockWidth;
    for (int index = 0; index < kWorkspaceCount; ++index) {
      const int x = content_x + index * 84;
      std::vector<Window> occupied;
      collect_windows(workspaces_[index].root.get(), occupied);
      occupied.insert(occupied.end(), workspaces_[index].floating.begin(), workspaces_[index].floating.end());
      if (index == current_workspace_) {
        XSetForeground(display_, bar_gc_, bar_selected_.pixel);
        XFillRectangle(display_, bar_pixmap_, bar_gc_, x, 0, 76, kBarHeight);
      }
      if (!occupied.empty()) {
        XSetForeground(display_, bar_gc_, index == current_workspace_ ? bar_background_.pixel : bar_foreground_.pixel);
        XFillRectangle(display_, bar_pixmap_, bar_gc_, x + 6, 18, 10, 10);
      }
      text(x + 24, std::to_string(index + 1), index == current_workspace_ ? bar_background_ : bar_foreground_);
    }

    const char* mode = workspace().mode == LayoutMode::Manual
                           ? "manual"
                           : workspace().mode == LayoutMode::MasterStack ? "master-stack" : "monocle";
    const std::string project = projects_.empty() ? "default" : project_label(projects_[active_project_index_].path);
    const std::string project_label_text = project + " · " + mode;
    text(content_x + 780, project_label_text, bar_foreground_);
    const std::string clock_widget = std::string(kIconClock) + " " + clock_text_;
    const int clock_width = text_width(clock_widget) + 32;
    const int clock_x = width - kDockWidth - clock_width - tray_pixel_width();
    text(clock_x, clock_widget, bar_foreground_);
    task_hits_.clear();
    std::vector<Window> windows;
    collect_windows(workspace().root.get(), windows);
    windows.insert(windows.end(), workspace().floating.begin(), workspace().floating.end());
    bar_task_list_x_ = std::max(content_x + 960, content_x + 780 + text_width(project_label_text) + 32);
    int x = bar_task_list_x_;
    const int end = std::max(x, clock_x);
    for (Window window : windows) {
      char* title = nullptr;
      std::string label = XFetchName(display_, window, &title) && title ? title : "untitled";
      if (title) XFree(title);
      if (label.size() > 24) label.resize(23), label += "…";
      const int item_width = text_width(label) + 32;
      if (x + item_width > end) break;
      if (window == workspace().focused) {
        XSetForeground(display_, bar_gc_, bar_selected_.pixel);
        XFillRectangle(display_, bar_pixmap_, bar_gc_, x, 4, item_width, kBarHeight - 8);
      }
      text(x + 16, label, window == workspace().focused ? bar_background_ : bar_foreground_);
      task_hits_.push_back({x, x + item_width, window});
      x += item_width + 1;
    }
    XCopyArea(display_, bar_pixmap_, bar_, bar_gc_, 0, 0, width, kBarHeight, 0, 0);
    XFlush(display_);
  }

  static std::string trim(std::string value) {
    const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char c) { return std::isspace(c); });
    const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char c) { return std::isspace(c); }).base();
    return first < last ? std::string(first, last) : std::string();
  }

  static bool desktop_true(const std::string& value) {
    std::string lower;
    lower.reserve(value.size());
    for (const unsigned char character : value) lower += static_cast<char>(std::tolower(character));
    return lower == "true";
  }

  // Desktop-entry field codes describe files/URIs supplied by a launcher. A
  // picker has none, so remove them before passing Exec to the shell.
  static std::string strip_exec_field_codes(const std::string& exec) {
    std::string cleaned;
    cleaned.reserve(exec.size());
    for (std::size_t index = 0; index < exec.size(); ++index) {
      if (exec[index] == '%' && index + 1 < exec.size()) {
        if (exec[index + 1] == '%') cleaned += '%';
        ++index;
      } else {
        cleaned += exec[index];
      }
    }
    return trim(std::move(cleaned));
  }

  // Mirrors the XDG lookup order used by mwm: earlier data directories win,
  // and a Hidden/NoDisplay entry also shadows an entry with the same ID.
  void scan_launcher_apps() {
    launcher_apps_.clear();
    std::unordered_set<std::string> seen;
    std::vector<std::string> data_dirs;
    if (const char* xdg_home = std::getenv("XDG_DATA_HOME"); xdg_home && *xdg_home) {
      data_dirs.emplace_back(xdg_home);
    } else if (const char* home = std::getenv("HOME"); home && *home) {
      data_dirs.emplace_back(std::string(home) + "/.local/share");
    }
    if (const char* xdg_dirs = std::getenv("XDG_DATA_DIRS"); xdg_dirs && *xdg_dirs) {
      std::string directories(xdg_dirs);
      std::size_t start = 0;
      while (start <= directories.size()) {
        const std::size_t end = directories.find(':', start);
        if (end != start) data_dirs.push_back(directories.substr(start, end - start));
        if (end == std::string::npos) break;
        start = end + 1;
      }
    } else {
      data_dirs.emplace_back("/usr/local/share");
      data_dirs.emplace_back("/usr/share");
    }

    for (const std::string& data_dir : data_dirs) {
      const std::string application_dir = data_dir + "/applications";
      DIR* directory = opendir(application_dir.c_str());
      if (!directory) continue;
      while (dirent* entry = readdir(directory)) {
        const std::string id(entry->d_name);
        if (id.size() <= 8 || id.compare(id.size() - 8, 8, ".desktop") != 0 || seen.count(id)) continue;

        std::ifstream file(application_dir + "/" + id);
        if (!file) continue;
        std::string name;
        std::string exec;
        std::string type;
        bool hidden = false;
        bool in_desktop_entry = false;
        for (std::string line; std::getline(file, line);) {
          line = trim(std::move(line));
          if (!line.empty() && line.front() == '[') {
            in_desktop_entry = line == "[Desktop Entry]";
            continue;
          }
          if (!in_desktop_entry || line.empty() || line.front() == '#' || line.front() == ';') continue;
          const std::size_t equals = line.find('=');
          if (equals == std::string::npos) continue;
          const std::string key = trim(line.substr(0, equals));
          const std::string value = trim(line.substr(equals + 1));
          if (key == "Name" && name.empty()) name = value;
          else if (key == "Exec" && exec.empty()) exec = value;
          else if (key == "Type") type = value;
          else if ((key == "Hidden" || key == "NoDisplay") && desktop_true(value)) hidden = true;
        }
        if (hidden) {
          seen.insert(id);
          continue;
        }
        if ((!type.empty() && type != "Application") || name.empty() || exec.empty()) continue;
        exec = strip_exec_field_codes(exec);
        if (!exec.empty()) {
          seen.insert(id);
          launcher_apps_.push_back({std::move(name), std::move(exec)});
        }
      }
      closedir(directory);
    }
    std::sort(launcher_apps_.begin(), launcher_apps_.end(), [](const LauncherApp& left, const LauncherApp& right) {
      std::string left_name = left.name;
      std::string right_name = right.name;
      std::transform(left_name.begin(), left_name.end(), left_name.begin(),
                     [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      std::transform(right_name.begin(), right_name.end(), right_name.begin(),
                     [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      return left_name < right_name;
    });
  }

  static int fuzzy_score(const std::string& query, const std::string& candidate) {
    if (query.empty()) return 0;
    std::size_t candidate_index = 0;
    int score = 0;
    int consecutive = 0;
    std::size_t last_match = std::string::npos;
    for (const unsigned char wanted : query) {
      const unsigned char lower_wanted = static_cast<unsigned char>(std::tolower(wanted));
      bool found = false;
      for (; candidate_index < candidate.size(); ++candidate_index) {
        const unsigned char character = static_cast<unsigned char>(candidate[candidate_index]);
        if (static_cast<unsigned char>(std::tolower(character)) != lower_wanted) continue;
        found = true;
        if (last_match != std::string::npos && candidate_index == last_match + 1) score += 5 + ++consecutive;
        else { consecutive = 0; ++score; }
        if (candidate_index == 0 || candidate[candidate_index - 1] == ' ' || candidate[candidate_index - 1] == '-' ||
            candidate[candidate_index - 1] == '_') score += 3;
        last_match = candidate_index++;
        break;
      }
      if (!found) return -1;
    }
    return std::max(0, score - static_cast<int>((candidate.size() - query.size()) / 8));
  }

  // Scoped to the current project's own 9 workspaces (mep-wm's per-project
  // workspace model), unlike mwm's single global tag list.
  void collect_project_windows(std::vector<Window>* windows) const {
    windows->clear();
    for (const Workspace& value : workspaces_) {
      windows->insert(windows->end(), value.stack_order.begin(), value.stack_order.end());
      windows->insert(windows->end(), value.floating.begin(), value.floating.end());
    }
  }

  void scan_windows() {
    window_candidates_.clear();
    std::vector<Window> windows;
    collect_project_windows(&windows);
    for (Window window : windows) {
      char* raw_title = nullptr;
      std::string title = "untitled";
      if (XFetchName(display_, window, &raw_title) && raw_title) { title = raw_title; XFree(raw_title); }
      window_candidates_.push_back({window, title});
    }
  }

  void filter_launcher_apps() {
    if (launcher_mode_ == LauncherMode::Windows) {
      std::vector<std::pair<int, std::size_t>> matches;
      for (std::size_t index = 0; index < window_candidates_.size(); ++index) {
        const int score = fuzzy_score(launcher_query_, window_candidates_[index].second);
        if (score >= 0) matches.emplace_back(score, index);
      }
      if (!launcher_query_.empty()) {
        std::sort(matches.begin(), matches.end(), [&](const auto& left, const auto& right) {
          return left.first != right.first ? left.first > right.first
                                           : window_candidates_[left.second].second < window_candidates_[right.second].second;
        });
      }
      window_matches_.clear();
      for (const auto& match : matches) window_matches_.push_back(match.second);
      launcher_selection_ = 0;
      launcher_scroll_ = 0;
      return;
    }
    if (launcher_mode_ != LauncherMode::Applications) {
      std::vector<std::pair<int, std::size_t>> matches;
      for (std::size_t index = 0; index < projects_.size(); ++index) {
        if (launcher_mode_ == LauncherMode::ActiveProjects && !project_has_clients(index)) continue;
        const int score = fuzzy_score(launcher_query_, projects_[index].path);
        if (score >= 0) matches.push_back({score, index});
      }
      if (!launcher_query_.empty()) {
        std::sort(matches.begin(), matches.end(), [&](const auto& left, const auto& right) {
          return left.first != right.first ? left.first > right.first
                                           : projects_[left.second].path < projects_[right.second].path;
        });
      }
      project_matches_.clear();
      for (const auto& match : matches) project_matches_.push_back(match.second);
      launcher_selection_ = 0;
      launcher_scroll_ = 0;
      return;
    }
    std::vector<std::pair<int, std::size_t>> matches;
    for (std::size_t index = 0; index < launcher_apps_.size(); ++index) {
      const int score = fuzzy_score(launcher_query_, launcher_apps_[index].name);
      if (score >= 0) matches.emplace_back(score, index);
    }
    if (!launcher_query_.empty()) {
      std::sort(matches.begin(), matches.end(), [&](const auto& left, const auto& right) {
        return left.first != right.first ? left.first > right.first
                                         : launcher_apps_[left.second].name < launcher_apps_[right.second].name;
      });
    }
    launcher_matches_.clear();
    for (const auto& match : matches) launcher_matches_.push_back(match.second);
    launcher_selection_ = 0;
    launcher_scroll_ = 0;
  }

  void draw_launcher_text(int x, int baseline, const std::string& value, const XftColor& color) {
    int cursor = x;
    for (std::size_t offset = 0; offset < value.size();) {
      FcChar32 codepoint;
      const std::size_t bytes = utf8_codepoint(value, offset, &codepoint);
      XftFont* font = XftCharExists(display_, bar_font_, codepoint) ? bar_font_ : fallback_font(codepoint);
      XftDrawStringUtf8(launcher_xft_draw_, &color, font, cursor, baseline,
                        reinterpret_cast<const FcChar8*>(value.data() + offset), static_cast<int>(bytes));
      XGlyphInfo extent{};
      XftTextExtentsUtf8(display_, font, reinterpret_cast<const FcChar8*>(value.data() + offset),
                          static_cast<int>(bytes), &extent);
      cursor += extent.xOff;
      offset += bytes;
    }
  }

  void draw_launcher() {
    if (launcher_window_ == None) return;
    const std::size_t match_count = launcher_match_count();
    const int shown = std::min<int>(kLauncherMaxRows, match_count - launcher_scroll_);
    const int height = kBarHeight * (std::max(1, shown) + 1);
    const Monitor& target_monitor = monitor(current_monitor_);
    const int width = std::max(1, std::min(kLauncherWidth, target_monitor.width - 20));
    const int x = target_monitor.x + (target_monitor.width - width) / 2;
    const int y = target_monitor.y + kBarHeight + 40;
    XMoveResizeWindow(display_, launcher_window_, x, y, width, height);
    if (launcher_pixmap_) XFreePixmap(display_, launcher_pixmap_);
    launcher_pixmap_ = XCreatePixmap(display_, launcher_window_, width, height, DefaultDepth(display_, screen_));
    if (launcher_xft_draw_) XftDrawDestroy(launcher_xft_draw_);
    launcher_xft_draw_ = XftDrawCreate(display_, launcher_pixmap_, DefaultVisual(display_, screen_),
                                        DefaultColormap(display_, screen_));
    XSetForeground(display_, bar_gc_, bar_background_.pixel);
    XFillRectangle(display_, launcher_pixmap_, bar_gc_, 0, 0, width, height);
    const char* title = launcher_mode_ == LauncherMode::Applications ? "Run: "
                      : launcher_mode_ == LauncherMode::Windows ? "Window: "
                      : launcher_mode_ == LauncherMode::Projects ? "Projects: " : "Active projects: ";
    draw_launcher_text(10, (kBarHeight + bar_font_->ascent - bar_font_->descent) / 2,
                       std::string(title) + launcher_query_, bar_foreground_);
    if (shown == 0) {
      draw_launcher_text(10, kBarHeight + (kBarHeight + bar_font_->ascent - bar_font_->descent) / 2,
                         launcher_mode_ == LauncherMode::Applications
                             ? (launcher_apps_.empty() ? "No applications found" : "No matching applications")
                         : launcher_mode_ == LauncherMode::Windows
                             ? (window_candidates_.empty() ? "No open windows" : "No matching windows")
                             : "No matching projects", bar_foreground_);
    }
    for (int row = 0; row < shown; ++row) {
      const int y_offset = (row + 1) * kBarHeight;
      if (launcher_scroll_ + static_cast<std::size_t>(row) == launcher_selection_) {
        XSetForeground(display_, bar_gc_, bar_selected_.pixel);
        XFillRectangle(display_, launcher_pixmap_, bar_gc_, 0, y_offset, width, kBarHeight);
      }
      const std::string label = launcher_mode_ == LauncherMode::Applications
                                    ? launcher_apps_[launcher_matches_[launcher_scroll_ + row]].name
                                : launcher_mode_ == LauncherMode::Windows
                                    ? window_candidates_[window_matches_[launcher_scroll_ + row]].second
                                    : project_label(projects_[project_matches_[launcher_scroll_ + row]].path);
      draw_launcher_text(10, y_offset + (kBarHeight + bar_font_->ascent - bar_font_->descent) / 2, label,
                         launcher_scroll_ + static_cast<std::size_t>(row) == launcher_selection_
                             ? bar_background_
                             : bar_foreground_);
    }
    XCopyArea(display_, launcher_pixmap_, launcher_window_, bar_gc_, 0, 0, width, height, 0, 0);
    XFlush(display_);
  }

  void close_launcher() {
    if (!launcher_visible_) return;
    launcher_visible_ = false;
    XUngrabKeyboard(display_, CurrentTime);
    XUnmapWindow(display_, launcher_window_);
  }

  void open_launcher(LauncherMode mode = LauncherMode::Applications) {
    launcher_mode_ = mode;
    if (mode == LauncherMode::Applications) scan_launcher_apps();
    if (mode == LauncherMode::Windows) scan_windows();
    launcher_query_.clear();
    filter_launcher_apps();
    if (launcher_window_ == None) {
      XSetWindowAttributes attributes{};
      attributes.override_redirect = True;
      attributes.background_pixel = bar_background_.pixel;
      attributes.border_pixel = bar_selected_.pixel;
      attributes.event_mask = ExposureMask | ButtonPressMask | KeyPressMask;
      launcher_window_ = XCreateWindow(display_, root_, 0, 0, kLauncherWidth, kBarHeight, 1,
                                        DefaultDepth(display_, screen_), CopyFromParent, DefaultVisual(display_, screen_),
                                        CWOverrideRedirect | CWBackPixel | CWBorderPixel | CWEventMask, &attributes);
      XStoreName(display_, launcher_window_, "mepwm-launcher");
      XDefineCursor(display_, launcher_window_, cursor_);
    }
    if (XGrabKeyboard(display_, root_, False, GrabModeAsync, GrabModeAsync, CurrentTime) != GrabSuccess) return;
    launcher_visible_ = true;
    draw_launcher();
    XMapRaised(display_, launcher_window_);
  }

  void toggle_launcher() {
    if (launcher_visible_) close_launcher();
    else open_launcher(LauncherMode::Applications);
  }

  void toggle_project_picker(bool active_only) {
    const LauncherMode mode = active_only ? LauncherMode::ActiveProjects : LauncherMode::Projects;
    if (launcher_visible_ && launcher_mode_ == mode) close_launcher();
    else {
      if (launcher_visible_) close_launcher();
      open_launcher(mode);
    }
  }

  void open_window_switcher() {
    if (launcher_visible_ && launcher_mode_ == LauncherMode::Windows) { close_launcher(); return; }
    if (launcher_visible_) close_launcher();
    open_launcher(LauncherMode::Windows);
  }

  std::size_t launcher_match_count() const {
    return launcher_mode_ == LauncherMode::Applications ? launcher_matches_.size()
         : launcher_mode_ == LauncherMode::Windows ? window_matches_.size()
                                                    : project_matches_.size();
  }

  void move_launcher_selection(int delta) {
    if (launcher_match_count() == 0) return;
    const int count = static_cast<int>(launcher_match_count());
    launcher_selection_ = static_cast<std::size_t>((static_cast<int>(launcher_selection_) + delta + count) % count);
    if (launcher_selection_ < launcher_scroll_) launcher_scroll_ = launcher_selection_;
    if (launcher_selection_ >= launcher_scroll_ + kLauncherMaxRows)
      launcher_scroll_ = launcher_selection_ - kLauncherMaxRows + 1;
  }

  void launch_selected_app(bool with_agent = false) {
    if (launcher_mode_ == LauncherMode::Windows) {
      if (window_matches_.empty()) return;
      const Window window = window_candidates_[window_matches_[launcher_selection_]].first;
      close_launcher();
      const int index = find_workspace(window);
      if (index < 0) return;
      if (index != current_workspace_) switch_workspace(index);
      focus(window);
      arrange();
      return;
    }
    if (launcher_mode_ != LauncherMode::Applications) {
      // A typed, valid directory takes precedence over fuzzy matches. Without
      // this, entering a path that happens to fuzzy-match an existing project
      // launches that match and makes it impossible to add the new directory.
      if (launcher_mode_ == LauncherMode::Projects && !launcher_query_.empty()) {
        std::string entered_path;
        if (normalize_project_path(launcher_query_, &entered_path)) {
          add_project(entered_path);
          const auto it = std::find_if(projects_.begin(), projects_.end(),
                                       [&](const Project& project) { return project.path == entered_path; });
          if (it == projects_.end()) return;
          const std::size_t project = static_cast<std::size_t>(it - projects_.begin());
          close_launcher();
          switch_project(project);
          spawn_terminal_in(entered_path);
          if (with_agent) spawn_terminal_running(entered_path, config_.agent_command);
          return;
        }
      }
      if (project_matches_.empty()) return;
      const std::size_t project = project_matches_[launcher_selection_];
      const bool open_terminal = launcher_mode_ == LauncherMode::Projects;
      const std::string path = projects_[project].path;
      close_launcher();
      switch_project(project);
      if (open_terminal) spawn_terminal_in(path);
      if (open_terminal && with_agent) spawn_terminal_running(path, config_.agent_command);
      return;
    }
    if (launcher_matches_.empty()) return;
    const std::string command = launcher_apps_[launcher_matches_[launcher_selection_]].exec;
    close_launcher();
    spawn_command(command);
  }

  void handle_launcher_key(const XKeyEvent& event) {
    char text[64];
    KeySym key = NoSymbol;
    const int length = XLookupString(const_cast<XKeyEvent*>(&event), text, sizeof(text), &key, nullptr);
    const unsigned int state = event.state & ~(LockMask | Mod2Mask);
    if (key == XK_Escape) { close_launcher(); return; }
    if (key == XK_Return || key == XK_KP_Enter) { launch_selected_app(state == ControlMask); return; }
    if (key == XK_BackSpace) {
      if (!launcher_query_.empty()) launcher_query_.pop_back();
      filter_launcher_apps();
      draw_launcher();
      return;
    }
    if (key == XK_Up || (state == ControlMask && key == XK_p)) { move_launcher_selection(-1); draw_launcher(); return; }
    if (key == XK_Down || key == XK_Tab || (state == ControlMask && key == XK_n)) {
      move_launcher_selection(1); draw_launcher(); return;
    }
    if (state == ControlMask && key == XK_u) {
      launcher_query_.clear();
      filter_launcher_apps();
      draw_launcher();
      return;
    }
    for (int index = 0; index < length && launcher_query_.size() < 255; ++index) {
      if (std::isprint(static_cast<unsigned char>(text[index]))) launcher_query_ += text[index];
    }
    if (length > 0) {
      filter_launcher_apps();
      draw_launcher();
    }
  }

  void grab_keys() {
    const unsigned int ignored_modifiers[] = {0, LockMask, Mod2Mask, LockMask | Mod2Mask};
    const KeySym plain_keys[] = {XK_Return, XK_q, XK_h, XK_i, XK_j, XK_k, XK_l, XK_o, XK_p, XK_v, XK_s, XK_Tab, XK_space,
                                 XK_a, XK_z, XK_m, XK_r, XK_minus, XK_comma, XK_period, XK_1, XK_2, XK_3,
                                 XK_4, XK_5, XK_6, XK_7, XK_8, XK_9, XK_w, XK_b, XK_d, XK_f};
    const KeySym shift_keys[] = {XK_q, XK_space, XK_c, XK_minus, XK_comma, XK_period, XK_h, XK_j, XK_k, XK_l,
                                 XK_r, XK_d, XK_t, XK_slash, XK_Tab};
    const KeySym ctrl_keys[] = {XK_h, XK_j, XK_k, XK_l};
    for (const unsigned int ignored : ignored_modifiers) {
      for (const KeySym key : plain_keys) {
        XGrabKey(display_, XKeysymToKeycode(display_, key), Mod4Mask | ignored, root_, True,
                 GrabModeAsync, GrabModeAsync);
      }
      for (const KeySym key : shift_keys) {
        XGrabKey(display_, XKeysymToKeycode(display_, key), Mod4Mask | ShiftMask | ignored, root_,
                 True, GrabModeAsync, GrabModeAsync);
      }
      for (const KeySym key : ctrl_keys) {
        XGrabKey(display_, XKeysymToKeycode(display_, key), Mod4Mask | ControlMask | ignored, root_,
                 True, GrabModeAsync, GrabModeAsync);
      }
    }
  }

  void adopt_existing_windows() {
    Window ignored_root;
    Window ignored_parent;
    Window* windows = nullptr;
    unsigned int count = 0;
    if (!XQueryTree(display_, root_, &ignored_root, &ignored_parent, &windows, &count)) return;
    for (unsigned int index = 0; index < count; ++index) {
      XWindowAttributes attributes;
      if (XGetWindowAttributes(display_, windows[index], &attributes) &&
          attributes.map_state == IsViewable && !attributes.override_redirect && windows[index] != bar_) {
        manage(windows[index]);
      }
    }
    if (windows) XFree(windows);
  }

  Node* find_leaf(Node* node, Window window) const {
    if (!node) return nullptr;
    if (node->is_leaf()) {
      return std::find(node->tabs.begin(), node->tabs.end(), window) != node->tabs.end() ? node
                                                                                              : nullptr;
    }
    for (const auto& child : node->children) {
      if (Node* found = find_leaf(child.get(), window)) return found;
    }
    return nullptr;
  }

  Node* first_leaf(Node* node) const {
    if (!node) return nullptr;
    return node->is_leaf() ? node : first_leaf(node->children.front().get());
  }

  void collect_windows(const Node* node, std::vector<Window>& windows) const {
    if (!node) return;
    if (node->is_leaf()) {
      windows.insert(windows.end(), node->tabs.begin(), node->tabs.end());
      return;
    }
    for (const auto& child : node->children) collect_windows(child.get(), windows);
  }

  void insert_tiled(Workspace& target, Window window) {
    if (!target.root) {
      target.root = make_leaf();
      target.selected_leaf = target.root.get();
    }
    if (!target.selected_leaf || !target.selected_leaf->is_leaf()) {
      target.selected_leaf = first_leaf(target.root.get());
    }
    target.selected_leaf->tabs.push_back(window);
    target.selected_leaf->active_tab = target.selected_leaf->tabs.size() - 1;
    target.stack_order.push_back(window);
  }

  void insert_floating(Workspace& target, Window window) { target.floating.push_back(window); }

  void remove_tiled(Workspace& target, Window window) {
    Node* leaf = find_leaf(target.root.get(), window);
    if (!leaf) return;
    const auto position = std::find(leaf->tabs.begin(), leaf->tabs.end(), window);
    leaf->tabs.erase(position);
    const auto stack_position = std::find(target.stack_order.begin(), target.stack_order.end(), window);
    if (stack_position != target.stack_order.end()) target.stack_order.erase(stack_position);
    if (leaf->tabs.empty()) {
      remove_empty_leaf(target, leaf);
    } else {
      leaf->active_tab %= leaf->tabs.size();
      target.selected_leaf = leaf;
    }
  }

  void center_floating(WindowState& state, int width, int height) {
    const Monitor& target = monitor(state.monitor);
    state.float_w = std::max(1, width);
    state.float_h = std::max(1, height);
    state.float_x = target.x + (target.width - state.float_w) / 2;
    state.float_y = target.y + kBarHeight +
                    std::max(0, (target.height - kBarHeight - state.float_h) / 2);
  }

  void apply_lua_rules(Window window, int* workspace_index, bool* should_float) {
    XClassHint hint{};
    std::string instance, class_name, title;
    if (XGetClassHint(display_, window, &hint)) {
      if (hint.res_name) instance = hint.res_name;
      if (hint.res_class) class_name = hint.res_class;
      if (hint.res_name) XFree(hint.res_name);
      if (hint.res_class) XFree(hint.res_class);
    }
    char* raw_title = nullptr;
    if (XFetchName(display_, window, &raw_title) && raw_title) { title = raw_title; XFree(raw_title); }
    auto matches = [](const std::string& pattern, const std::string& value) {
      return pattern.empty() || value.find(pattern) != std::string::npos;
    };
    for (const LuaRule& rule : lua_rules_) {
      if (!matches(rule.class_name, class_name) || !matches(rule.instance, instance) || !matches(rule.title, title)) continue;
      if (rule.workspace >= 0 && rule.workspace < kWorkspaceCount) *workspace_index = rule.workspace;
      if (rule.set_floating) *should_float = rule.floating;
    }
  }

  void manage(Window window) {
    if (find_workspace(window) >= 0) return;
    XWindowAttributes attributes;
    if (!XGetWindowAttributes(display_, window, &attributes) || attributes.override_redirect) return;

    int target_index = current_workspace_;
    WindowState& state = window_state_[window];
    state.monitor = current_monitor_;

    Window transient_for = None;
    const bool is_transient = XGetTransientForHint(display_, window, &transient_for) != 0;
    bool should_float = is_transient || is_dialog_window_type(window);
    apply_lua_rules(window, &target_index, &should_float);
    Workspace& target = workspaces_[target_index];

    XSetWindowBorderWidth(display_, window, config_.border_width);
    XSetWindowBorder(display_, window, border_normal_pixel_);

    if (should_float) {
      state.floating = true;
      center_floating(state, attributes.width, attributes.height);
      insert_floating(target, window);
    } else {
      insert_tiled(target, window);
    }

    XSelectInput(display_, window, EnterWindowMask | FocusChangeMask | PropertyChangeMask);
    XGrabButton(display_, AnyButton, AnyModifier, window, False, ButtonPressMask, GrabModeSync,
                GrabModeAsync, None, None);
    for (const LuaMousebind& binding : lua_mousebinds_) {
      if (binding.context == "client")
        XGrabButton(display_, binding.button, binding.modifiers, window, False, ButtonPressMask,
                    GrabModeAsync, GrabModeAsync, None, None);
    }
    add_to_client_list(window);
    // Focusing requires the window to already be viewable (XSetInputFocus is a
    // BadMatch otherwise); map it up front so focus() below always succeeds.
    XMapWindow(display_, window);
    if (target_index == current_workspace_) {
      focus(window);
    } else {
      hide_window(window);
    }
    arrange();
  }

  int find_workspace(Window window) const {
    for (int index = 0; index < kWorkspaceCount; ++index) {
      const Workspace& ws = workspaces_[index];
      if (find_leaf(ws.root.get(), window)) return index;
      if (std::find(ws.floating.begin(), ws.floating.end(), window) != ws.floating.end()) return index;
    }
    return -1;
  }

  Window pick_fallback_focus(const Workspace& target) const {
    if (target.selected_leaf && !target.selected_leaf->tabs.empty()) {
      return target.selected_leaf->tabs[target.selected_leaf->active_tab];
    }
    if (!target.floating.empty()) return target.floating.back();
    return None;
  }

  void forget_window(Workspace& target, Window window) {
    const auto floating_position = std::find(target.floating.begin(), target.floating.end(), window);
    if (floating_position != target.floating.end()) {
      target.floating.erase(floating_position);
    } else {
      remove_tiled(target, window);
    }
    if (target.focused == window) target.focused = pick_fallback_focus(target);
  }

  std::unique_ptr<Node>* slot_for(std::unique_ptr<Node>& root, Node* node) {
    if (root.get() == node) return &root;
    for (auto& child : root->children) {
      if (auto* slot = slot_for(child, node)) return slot;
    }
    return nullptr;
  }

  void remove_empty_leaf(Workspace& target, Node* leaf) {
    if (leaf == target.root.get()) {
      target.root.reset();
      target.selected_leaf = nullptr;
      return;
    }
    Node* parent = leaf->parent;
    auto& children = parent->children;
    const auto position = std::find_if(children.begin(), children.end(),
                                       [leaf](const std::unique_ptr<Node>& child) {
                                         return child.get() == leaf;
                                       });
    parent->weights.erase(parent->weights.begin() + (position - children.begin()));
    children.erase(position);
    if (children.size() == 1) {
      std::unique_ptr<Node>* parent_slot = slot_for(target.root, parent);
      std::unique_ptr<Node> survivor = std::move(children.front());
      survivor->parent = parent->parent;
      *parent_slot = std::move(survivor);
    }
    target.selected_leaf = first_leaf(target.root.get());
  }

  void unmanage(Window window) {
    const int index = find_workspace(window);
    if (index < 0) {
      for (Project& project : projects_) {
        for (Workspace& candidate : project.workspaces) {
          if (!find_leaf(candidate.root.get(), window) &&
              std::find(candidate.floating.begin(), candidate.floating.end(), window) == candidate.floating.end())
            continue;
          XUngrabButton(display_, AnyButton, AnyModifier, window);
          forget_window(candidate, window);
          window_state_.erase(window);
          rebuild_client_list();
          return;
        }
      }
      return;
    }
    XUngrabButton(display_, AnyButton, AnyModifier, window);
    if (previously_focused_ == window) previously_focused_ = None;
    const bool visible = index == current_workspace_;
    forget_window(workspaces_[index], window);
    window_state_.erase(window);
    rebuild_client_list();
    if (visible) {
      if (workspace().focused != None) {
        focus(workspace().focused);
      } else {
        XSetInputFocus(display_, root_, RevertToPointerRoot, CurrentTime);
        Window none = None;
        set_active_window(none);
      }
      arrange();
    }
  }

  void update_border(Window window, bool focused_state) {
    if (window == None) return;
    XSetWindowBorder(display_, window, focused_state ? border_focused_pixel_ : border_normal_pixel_);
  }

  void focus(Window window) {
    Workspace& target = workspace();
    if (window == None) {
      if (previously_focused_ != None) {
        update_border(previously_focused_, false);
        previously_focused_ = None;
      }
      target.focused = None;
      XSetInputFocus(display_, root_, RevertToPointerRoot, CurrentTime);
      Window none = None;
      set_active_window(none);
      return;
    }
    Node* leaf = find_leaf(target.root.get(), window);
    const bool is_floating_window =
        !leaf && std::find(target.floating.begin(), target.floating.end(), window) != target.floating.end();
    if (!leaf && !is_floating_window) return;
    if (leaf) {
      target.selected_leaf = leaf;
      leaf->active_tab = static_cast<std::size_t>(std::find(leaf->tabs.begin(), leaf->tabs.end(), window) -
                                                  leaf->tabs.begin());
    }
    if (previously_focused_ != None && previously_focused_ != window) update_border(previously_focused_, false);
    target.focused = window;
    current_monitor_ = window_state_[window].monitor;
    previously_focused_ = window;
    update_border(window, true);
    XSetInputFocus(display_, window, RevertToPointerRoot, CurrentTime);
    XRaiseWindow(display_, window);
    XRaiseWindow(display_, bar_);
    set_active_window(window);
  }

  // Window-geometry-based directional focus, used for the automatic layouts
  // (MasterStack/Monocle) where there is no pane tree to select against --
  // only real, mapped windows exist as candidates. Manual mode uses
  // select_pane_direction below instead, which can also land on an empty
  // pane that has no window yet.
  void focus_direction(KeySym key) {
    const Window selected = workspace().focused;
    if (selected == None) return;

    XWindowAttributes selected_attributes;
    if (!XGetWindowAttributes(display_, selected, &selected_attributes) ||
        selected_attributes.map_state != IsViewable) return;
    const int selected_x = selected_attributes.x + selected_attributes.width / 2;
    const int selected_y = selected_attributes.y + selected_attributes.height / 2;

    std::vector<Window> windows;
    collect_windows(workspace().root.get(), windows);
    windows.insert(windows.end(), workspace().floating.begin(), workspace().floating.end());

    Window best = None;
    long best_score = 0;
    for (Window candidate : windows) {
      if (candidate == selected || window_state_[candidate].monitor != current_monitor_) continue;
      XWindowAttributes attributes;
      if (!XGetWindowAttributes(display_, candidate, &attributes) || attributes.map_state != IsViewable) continue;

      const int candidate_x = attributes.x + attributes.width / 2;
      const int candidate_y = attributes.y + attributes.height / 2;
      int primary = 0;
      int secondary = 0;
      if (key == XK_h) {
        primary = selected_x - candidate_x;
        secondary = std::abs(selected_y - candidate_y);
      } else if (key == XK_j) {
        primary = candidate_y - selected_y;
        secondary = std::abs(selected_x - candidate_x);
      } else if (key == XK_k) {
        primary = selected_y - candidate_y;
        secondary = std::abs(selected_x - candidate_x);
      } else if (key == XK_l) {
        primary = candidate_x - selected_x;
        secondary = std::abs(selected_y - candidate_y);
      } else {
        return;
      }
      if (primary <= 0) continue;

      // Prioritize alignment with the requested axis, then distance along it,
      // so a directly-adjacent window (e.g. the other pane in the same
      // column) wins over a merely-closer window that sits off to the side.
      const long score = static_cast<long>(secondary) * 10000L + primary;
      if (best == None || score < best_score) {
        best = candidate;
        best_score = score;
      }
    }
    if (best != None) focus(best);
  }

  // Selects a pane outright: focuses its active tab if it has one, or -- for
  // a pane freshly created by split() and still empty -- just marks it as
  // the selected_leaf and clears real window focus, so the next spawned
  // client (Super+Return) or split (Super+v/s) lands there instead of
  // wherever focus last was.
  void select_pane(Node* leaf) {
    if (!leaf) return;
    Workspace& target = workspace();
    target.selected_leaf = leaf;
    if (!leaf->tabs.empty()) {
      focus(leaf->tabs[leaf->active_tab]);
    } else {
      if (previously_focused_ != None) {
        update_border(previously_focused_, false);
        previously_focused_ = None;
      }
      target.focused = None;
      XSetInputFocus(display_, root_, RevertToPointerRoot, CurrentTime);
      Window none = None;
      set_active_window(none);
    }
    // Either branch can change which pane is empty-and-selected, so refresh
    // the highlight overlay (and pane tab bars) either way -- focus() alone
    // doesn't touch them.
    arrange();
  }

  // Directional pane selection for manual mode: like focus_direction, but
  // the candidates are the panes in the tree (using their last-arranged
  // rects) rather than only real windows, so an empty pane can be selected
  // and later opened into without having to open something in it the
  // instant it's split off.
  void select_pane_direction(KeySym key) {
    Workspace& target = workspace();
    Node* current_leaf = target.selected_leaf;

    // selected_leaf is the source of truth for "where we are" -- it can
    // diverge from target.focused right after a split, since split() moves
    // selection to the new empty pane without touching real window focus
    // (there's nothing to focus yet). Preferring target.focused's geometry
    // here would silently navigate from the stale, pre-split position.
    int selected_x = 0, selected_y = 0;
    bool have_reference = false;
    if (current_leaf) {
      if (const Rect* rect = leaf_rect(current_leaf, current_monitor_)) {
        selected_x = rect->x + rect->w / 2;
        selected_y = rect->y + rect->h / 2;
        have_reference = true;
      }
    }
    if (!have_reference && target.focused != None) {
      XWindowAttributes attributes;
      if (XGetWindowAttributes(display_, target.focused, &attributes) &&
          attributes.map_state == IsViewable) {
        selected_x = attributes.x + attributes.width / 2;
        selected_y = attributes.y + attributes.height / 2;
        have_reference = true;
      }
    }
    if (!have_reference) { focus_direction(key); return; }

    struct Candidate {
      int x, y;
      Node* leaf;
      Window window;
    };
    std::vector<Candidate> candidates;

    std::vector<Node*> leaves;
    collect_leaves(target.root.get(), leaves);
    for (Node* leaf : leaves) {
      if (leaf == current_leaf) continue;
      const Rect* rect = leaf_rect(leaf, current_monitor_);
      if (!rect) continue;
      const Window window = leaf->tabs.empty() ? None : leaf->tabs[leaf->active_tab];
      candidates.push_back({rect->x + rect->w / 2, rect->y + rect->h / 2, leaf, window});
    }
    for (Window window : target.floating) {
      if (window == target.focused || window_state_[window].monitor != current_monitor_) continue;
      XWindowAttributes attributes;
      if (!XGetWindowAttributes(display_, window, &attributes) || attributes.map_state != IsViewable) continue;
      candidates.push_back({attributes.x + attributes.width / 2, attributes.y + attributes.height / 2,
                            nullptr, window});
    }

    bool found = false;
    long best_score = 0;
    Node* best_leaf = nullptr;
    Window best_window = None;
    for (const Candidate& candidate : candidates) {
      int primary = 0, secondary = 0;
      if (key == XK_h) {
        primary = selected_x - candidate.x;
        secondary = std::abs(selected_y - candidate.y);
      } else if (key == XK_j) {
        primary = candidate.y - selected_y;
        secondary = std::abs(selected_x - candidate.x);
      } else if (key == XK_k) {
        primary = selected_y - candidate.y;
        secondary = std::abs(selected_x - candidate.x);
      } else if (key == XK_l) {
        primary = candidate.x - selected_x;
        secondary = std::abs(selected_y - candidate.y);
      } else {
        return;
      }
      if (primary <= 0) continue;
      // Prioritize alignment with the requested axis, then distance along it
      // -- see the matching comment in focus_direction above.
      const long score = static_cast<long>(secondary) * 10000L + primary;
      if (!found || score < best_score) {
        found = true;
        best_score = score;
        best_leaf = candidate.leaf;
        best_window = candidate.window;
      }
    }
    if (!found) return;
    if (best_leaf) {
      select_pane(best_leaf);
    } else if (best_window != None) {
      focus(best_window);
    }
  }

  void next_tab(int direction = 1) {
    Node* leaf = workspace().selected_leaf;
    if (!leaf || leaf->tabs.size() < 2) return;
    const long count = static_cast<long>(leaf->tabs.size());
    long index = (static_cast<long>(leaf->active_tab) + direction) % count;
    if (index < 0) index += count;
    leaf->active_tab = static_cast<std::size_t>(index);
    focus(leaf->tabs[leaf->active_tab]);
    arrange();
  }

  void split(Orientation orientation) {
    Workspace& target = workspace();
    if (!target.root) {
      target.root = make_leaf();
      target.selected_leaf = target.root.get();
      return;
    }
    Node* leaf = target.selected_leaf ? target.selected_leaf : first_leaf(target.root.get());
    if (!leaf) return;
    std::unique_ptr<Node>* slot = slot_for(target.root, leaf);
    std::unique_ptr<Node> old_leaf = std::move(*slot);
    auto container = std::make_unique<Node>();
    container->parent = old_leaf->parent;
    container->orientation = orientation;
    old_leaf->parent = container.get();
    auto new_leaf = make_leaf(container.get());
    Node* new_leaf_ptr = new_leaf.get();
    container->children.push_back(std::move(old_leaf));
    container->children.push_back(std::move(new_leaf));
    container->weights = {1.0, 1.0};
    *slot = std::move(container);
    // select_pane, not a bare assignment: the new pane is empty, so this
    // also drops real window focus off the old pane's client (otherwise
    // target.focused would keep pointing at it, out of sync with
    // selected_leaf, until something is opened here).
    select_pane(new_leaf_ptr);
  }

  // Deletes the selected pane, folding its clients in as extra tabs of the
  // sibling pane (or, if that sibling is itself a split, its first leaf).
  void merge_pane() {
    Workspace& target = workspace();
    Node* leaf = target.selected_leaf;
    if (!leaf || !leaf->parent) return;
    Node* parent = leaf->parent;
    Node* sibling = nullptr;
    for (auto& child : parent->children) {
      if (child.get() != leaf) { sibling = child.get(); break; }
    }
    if (!sibling) return;
    Node* absorbing = sibling->is_leaf() ? sibling : first_leaf(sibling);
    if (!absorbing) return;
    absorbing->tabs.insert(absorbing->tabs.end(), leaf->tabs.begin(), leaf->tabs.end());
    absorbing->active_tab = absorbing->tabs.empty() ? 0 : absorbing->tabs.size() - 1;
    leaf->tabs.clear();
    remove_empty_leaf(target, leaf);
    target.selected_leaf = absorbing;
    if (!absorbing->tabs.empty()) focus(absorbing->tabs[absorbing->active_tab]);
    arrange();
  }

  // Resize across the nearest split boundary that has the requested axis.
  // h/l/j/k move that boundary left/right/down/up respectively, regardless
  // of whether the selected pane sits before or after it: l/j grow whichever
  // pane has the lower index and shrink the higher-index one (the boundary
  // moves toward the higher index), while h/k do the reverse. This keeps the
  // resize direction tied to screen position rather than to which pane
  // happens to be selected.
  void resize_pane(KeySym key) {
    Workspace& target = workspace();
    if (target.mode != LayoutMode::Manual) return;
    // selected_leaf rather than target.focused, so resizing works on a
    // selected-but-still-empty pane too.
    Node* child = target.selected_leaf;
    if (!child) return;

    const Orientation axis = (key == XK_h || key == XK_l) ? Orientation::Vertical
                                                           : Orientation::Horizontal;
    Node* split = child->parent;
    while (split && split->orientation != axis) {
      child = split;
      split = split->parent;
    }
    if (!split) return;
    const auto found = std::find_if(split->children.begin(), split->children.end(),
                                    [child](const std::unique_ptr<Node>& node) { return node.get() == child; });
    if (found == split->children.end()) return;
    const std::size_t index = static_cast<std::size_t>(found - split->children.begin());
    const bool has_before = index > 0;
    const bool has_after = index + 1 < split->children.size();
    if (!has_before && !has_after) return;
    if (key != XK_h && key != XK_l && key != XK_j && key != XK_k) return;

    // l/j prefer the boundary after the selected pane; h/k prefer the one
    // before it. At an outer edge only one boundary exists, so fall back.
    const bool towards_higher_index = (key == XK_l || key == XK_j);
    const std::size_t neighbour = towards_higher_index ? (has_after ? index + 1 : index - 1)
                                                        : (has_before ? index - 1 : index + 1);

    const std::size_t lower = std::min(index, neighbour);
    const std::size_t higher = std::max(index, neighbour);
    const std::size_t grower = towards_higher_index ? lower : higher;
    const std::size_t donor = towards_higher_index ? higher : lower;

    if (split->weights[donor] <= kMinSplitWeight) return;
    const double amount = std::min(kResizeStep, split->weights[donor] - kMinSplitWeight);
    split->weights[grower] += amount;
    split->weights[donor] -= amount;
    arrange();
  }

  void cycle_layout() {
    Workspace& target = workspace();
    target.mode = target.mode == LayoutMode::Manual
                      ? LayoutMode::MasterStack
                      : target.mode == LayoutMode::MasterStack ? LayoutMode::Monocle : LayoutMode::Manual;
    arrange();
  }

  void cycle_layout_reverse() {
    Workspace& target = workspace();
    target.mode = target.mode == LayoutMode::Manual
                      ? LayoutMode::Monocle
                      : target.mode == LayoutMode::Monocle ? LayoutMode::MasterStack : LayoutMode::Manual;
    arrange();
  }

  // Visual-only: unmaps the bar without reclaiming its screen space in the
  // tiling math (kBarHeight is baked into dozens of geometry calculations).
  void toggle_bar() {
    bar_visible_ = !bar_visible_;
    if (bar_visible_) { XMapRaised(display_, bar_); draw_bar(); } else { XUnmapWindow(display_, bar_); }
  }

  void adjust_nmaster(int delta) {
    config_.nmaster = static_cast<unsigned int>(std::max(0, static_cast<int>(config_.nmaster) + delta));
    arrange();
  }

  void adjust_mfact(double delta) {
    config_.mfact = std::clamp(config_.mfact + static_cast<float>(delta), 0.05f, 0.95f);
    arrange();
  }

  // Cycles through master-stack order, distinct from focus_direction's
  // spatial h/j/k/l focus.
  void focus_stack(int direction) {
    Workspace& target = workspace();
    if (target.stack_order.empty()) return;
    const auto it = std::find(target.stack_order.begin(), target.stack_order.end(), target.focused);
    int index = it == target.stack_order.end() ? 0 : static_cast<int>(it - target.stack_order.begin());
    const int count = static_cast<int>(target.stack_order.size());
    index = ((index + direction) % count + count) % count;
    focus(target.stack_order[static_cast<std::size_t>(index)]);
  }

  // Re-execs the running binary with its original argv, read back from
  // /proc/self/cmdline rather than plumbed through Backend::run -- avoids
  // threading argv through WindowManager/Config just for this.
  void restart() {
    std::ifstream cmdline("/proc/self/cmdline", std::ios::binary);
    const std::string data((std::istreambuf_iterator<char>(cmdline)), std::istreambuf_iterator<char>());
    std::vector<std::string> parts;
    for (std::size_t start = 0; start < data.size();) {
      std::size_t nul = data.find('\0', start);
      if (nul == std::string::npos) nul = data.size();
      parts.push_back(data.substr(start, nul - start));
      start = nul + 1;
    }
    if (parts.empty()) { std::cerr << "mepwm: restart failed: could not read /proc/self/cmdline\n"; return; }
    std::vector<char*> argv;
    for (std::string& part : parts) argv.push_back(part.data());
    argv.push_back(nullptr);
    execv("/proc/self/exe", argv.data());
    std::cerr << "mepwm: restart failed: " << std::strerror(errno) << '\n';
  }

  // Moves the focused tiled window to the front of master-stack order.
  void zoom() {
    Workspace& target = workspace();
    Window window = target.focused;
    if (window == None) return;
    const auto position = std::find(target.stack_order.begin(), target.stack_order.end(), window);
    if (position == target.stack_order.end() || position == target.stack_order.begin()) return;
    target.stack_order.erase(position);
    target.stack_order.insert(target.stack_order.begin(), window);
    arrange();
  }

  void toggle_floating() {
    Workspace& target = workspace();
    Window window = target.focused;
    if (window == None) return;
    WindowState& state = window_state_[window];
    if (state.fullscreen) return;
    if (state.floating) {
      const auto position = std::find(target.floating.begin(), target.floating.end(), window);
      if (position != target.floating.end()) target.floating.erase(position);
      state.floating = false;
      insert_tiled(target, window);
    } else {
      XWindowAttributes attributes;
      if (XGetWindowAttributes(display_, window, &attributes)) {
        state.float_x = attributes.x;
        state.float_y = attributes.y;
        state.float_w = std::max(1, attributes.width);
        state.float_h = std::max(1, attributes.height);
      }
      remove_tiled(target, window);
      state.floating = true;
      insert_floating(target, window);
    }
    target.focused = window;
    arrange();
    focus(window);
  }

  void toggle_maximize() {
    Workspace& target = workspace();
    Window window = target.focused;
    if (window == None) return;
    WindowState& state = window_state_[window];
    if (state.fullscreen || !state.floating) return;
    const int gap = static_cast<int>(config_.gap);
    const Monitor& target_monitor = monitor(state.monitor);
    const int area_x = target_monitor.x + gap;
    const int area_y = target_monitor.y + kBarHeight + gap;
    const int area_w = target_monitor.width - 2 * gap;
    const int area_h = target_monitor.height - kBarHeight - 2 * gap;
    if (state.maximized) {
      state.float_x = state.pre_maximize_x;
      state.float_y = state.pre_maximize_y;
      state.float_w = state.pre_maximize_w;
      state.float_h = state.pre_maximize_h;
      state.maximized = false;
    } else {
      state.pre_maximize_x = state.float_x;
      state.pre_maximize_y = state.float_y;
      state.pre_maximize_w = state.float_w;
      state.pre_maximize_h = state.float_h;
      state.float_x = area_x;
      state.float_y = area_y;
      state.float_w = area_w;
      state.float_h = area_h;
      state.maximized = true;
    }
    arrange();
  }

  void set_fullscreen(Window window, bool enable) {
    const int index = find_workspace(window);
    if (index < 0) return;
    Workspace& target = workspaces_[index];
    WindowState& state = window_state_[window];
    if (state.fullscreen == enable) return;

    if (enable) {
      state.pre_fullscreen_floating = state.floating;
      if (!state.floating) {
        XWindowAttributes attributes;
        if (XGetWindowAttributes(display_, window, &attributes)) {
          state.float_x = attributes.x;
          state.float_y = attributes.y;
          state.float_w = std::max(1, attributes.width);
          state.float_h = std::max(1, attributes.height);
        }
        remove_tiled(target, window);
        state.floating = true;
        insert_floating(target, window);
      }
      state.fullscreen = true;
      XSetWindowBorderWidth(display_, window, 0);
      set_net_wm_state_fullscreen(window, true);
    } else {
      state.fullscreen = false;
      XSetWindowBorderWidth(display_, window, config_.border_width);
      set_net_wm_state_fullscreen(window, false);
      if (!state.pre_fullscreen_floating) {
        const auto position = std::find(target.floating.begin(), target.floating.end(), window);
        if (position != target.floating.end()) target.floating.erase(position);
        state.floating = false;
        insert_tiled(target, window);
      }
    }
    if (index == current_workspace_) arrange();
  }

  void kill_focused() {
    Window window = workspace().focused;
    if (window == None) return;
    if (supports_protocol(window, wm_delete_window_atom_)) {
      XEvent event{};
      event.xclient.type = ClientMessage;
      event.xclient.window = window;
      event.xclient.message_type = wm_protocols_atom_;
      event.xclient.format = 32;
      event.xclient.data.l[0] = static_cast<long>(wm_delete_window_atom_);
      event.xclient.data.l[1] = CurrentTime;
      XSendEvent(display_, window, False, NoEventMask, &event);
    } else {
      XGrabServer(display_);
      XSetErrorHandler(dummy_x_error);
      XSetCloseDownMode(display_, DestroyAll);
      XKillClient(display_, window);
      XSync(display_, False);
      XSetErrorHandler(on_x_error);
      XUngrabServer(display_);
    }
  }

  void set_scratchpad() {
    scratchpad_ = workspace().focused;
    scratchpad_hidden_ = false;
  }

  void toggle_scratchpad() {
    if (scratchpad_ == None || find_workspace(scratchpad_) < 0) return;
    const int index = find_workspace(scratchpad_);
    if (scratchpad_hidden_) {
      scratchpad_hidden_ = false;
      if (index != current_workspace_) switch_workspace(index);
      show_window(scratchpad_);
      focus(scratchpad_);
      arrange();
    } else {
      scratchpad_hidden_ = true;
      hide_window(scratchpad_);
      if (workspace().focused == scratchpad_) focus(pick_fallback_focus(workspace()));
      arrange();
    }
  }

  void switch_workspace(int index) {
    if (index == current_workspace_ || index < 0 || index >= kWorkspaceCount) return;
    refresh_display_geometry();
    hide_workspace(workspace());
    current_workspace_ = index;
    arrange();
    if (workspace().focused != None) {
      focus(workspace().focused);
    } else {
      XSetInputFocus(display_, root_, RevertToPointerRoot, CurrentTime);
      Window none = None;
      set_active_window(none);
    }
    draw_bar();
  }

  void hide_workspace(const Workspace& target) {
    std::vector<Window> windows;
    collect_windows(target.root.get(), windows);
    windows.insert(windows.end(), target.floating.begin(), target.floating.end());
    for (Window window : windows) hide_window(window);
  }

  void hide_window(Window window) {
    XWindowAttributes attributes;
    if (XGetWindowAttributes(display_, window, &attributes) && attributes.map_state == IsViewable) {
      expected_unmaps_.insert(window);
      XUnmapWindow(display_, window);
    }
  }

  void show_window(Window window) { XMapWindow(display_, window); }

  void resize(Window window, int x, int y, int width, int height) {
    show_window(window);
    XMoveResizeWindow(display_, window, x, y, std::max(1, width), std::max(1, height));
  }

  void arrange_leaf(Node* leaf, int x, int y, int width, int height, std::size_t monitor_index) {
    // Recorded unconditionally, including for empty leaves, so a freshly
    // split pane with nothing open in it yet still has a rect to select,
    // highlight, and place a tab bar against.
    leaf->rect_by_monitor[monitor_index] = Rect{x, y, width, height};
    if (leaf->tabs.empty()) return;
    leaf->active_tab %= leaf->tabs.size();
    int content_y = y;
    int content_height = height;
    if (leaf->tabs.size() > 1) {
      content_y = y + kPaneTabBarHeight;
      content_height = std::max(1, height - kPaneTabBarHeight);
    }
    for (std::size_t index = 0; index < leaf->tabs.size(); ++index) {
      if (window_state_[leaf->tabs[index]].monitor != monitor_index) continue;
      if (index == leaf->active_tab) {
        resize(leaf->tabs[index], x, content_y, width, content_height);
      } else {
        hide_window(leaf->tabs[index]);
      }
    }
  }

  const Rect* leaf_rect(const Node* leaf, std::size_t monitor_index) const {
    if (!leaf) return nullptr;
    const auto it = leaf->rect_by_monitor.find(monitor_index);
    return it != leaf->rect_by_monitor.end() ? &it->second : nullptr;
  }

  void collect_leaves(Node* node, std::vector<Node*>& leaves) const {
    if (!node) return;
    if (node->is_leaf()) { leaves.push_back(node); return; }
    for (auto& child : node->children) collect_leaves(child.get(), leaves);
  }

  void arrange_manual(Node* node, int x, int y, int width, int height, std::size_t monitor_index) {
    if (!node) return;
    if (node->is_leaf()) {
      arrange_leaf(node, x, y, width, height, monitor_index);
      return;
    }
    const int count = static_cast<int>(node->children.size());
    if (count == 0) return;
    const int gap = static_cast<int>(config_.gap);
    const int available = std::max(1, (node->orientation == Orientation::Vertical ? width : height) -
                                      gap * (count - 1));
    const double total = std::accumulate(node->weights.begin(), node->weights.end(), 0.0);
    int offset = 0;
    for (int index = 0; index < count; ++index) {
      const int extent = index == count - 1
                             ? available - offset
                             : std::max(1, static_cast<int>(available * node->weights[index] / total));
      if (node->orientation == Orientation::Vertical) {
        arrange_manual(node->children[index].get(), x + offset, y, extent, height, monitor_index);
      } else {
        arrange_manual(node->children[index].get(), x, y + offset, width, extent, monitor_index);
      }
      offset += extent + gap;
    }
  }

  // Lays out windows[begin..end) as a single vertical column within the given rect.
  void stack_column(const std::vector<Window>& windows, int begin, int end, int x, int y, int width,
                     int height, int gap) {
    const int count = end - begin;
    if (count <= 0) return;
    const int cell_height = (height - gap * (count - 1)) / count;
    for (int index = 0; index < count; ++index) {
      const int h = (index == count - 1) ? (height - (cell_height + gap) * (count - 1)) : cell_height;
      resize(windows[begin + index], x, y + index * (cell_height + gap), width, h);
    }
  }

  void arrange_automatic(LayoutMode mode, int x, int y, int width, int height, std::size_t monitor_index) {
    Workspace& target = workspace();
    std::vector<Window> windows;
    for (Window window : target.stack_order) {
      if (!window_state_[window].floating && window_state_[window].monitor == monitor_index) windows.push_back(window);
    }
    if (windows.empty()) return;

    if (mode == LayoutMode::Monocle) {
      for (Window window : windows) hide_window(window);
      const bool focused_is_tiled =
          target.focused != None &&
          std::find(windows.begin(), windows.end(), target.focused) != windows.end();
      resize(focused_is_tiled ? target.focused : windows.front(), x, y, width, height);
      return;
    }

    const int gap = static_cast<int>(config_.gap);
    const int n = static_cast<int>(windows.size());
    const int nmaster = std::min(static_cast<int>(config_.nmaster), n);

    if (nmaster == 0 || nmaster == n) {
      stack_column(windows, 0, n, x, y, width, height, gap);
      return;
    }

    const int master_width = static_cast<int>(width * config_.mfact) - gap / 2;
    stack_column(windows, 0, nmaster, x, y, master_width, height, gap);
    const int stack_x = x + master_width + gap;
    const int stack_width = width - master_width - gap;
    stack_column(windows, nmaster, n, stack_x, y, stack_width, height, gap);
  }

  void arrange_floating(Workspace& target) {
    for (Window window : target.floating) {
      const WindowState& state = window_state_[window];
      if (state.fullscreen) {
        const Monitor& target_monitor = monitor(state.monitor);
        resize(window, target_monitor.x, target_monitor.y, target_monitor.width, target_monitor.height);
      } else {
        resize(window, state.float_x, state.float_y, state.float_w, state.float_h);
      }
    }
    for (Window window : target.floating) XRaiseWindow(display_, window);
  }

  void arrange() {
    Workspace& target = workspace();
    const int gap = static_cast<int>(config_.gap);
    for (std::size_t index = 0; index < monitors_.size(); ++index) {
      const Monitor& target_monitor = monitor(index);
      const int x = target_monitor.x + kDockWidth + gap;
      const int y = target_monitor.y + kBarHeight + gap;
      const int width = std::max(1, target_monitor.width - 2 * kDockWidth - 2 * gap);
      const int height = std::max(1, target_monitor.height - 2 * kBarHeight - 2 * gap);
      if (target.mode == LayoutMode::Manual) {
        arrange_manual(target.root.get(), x, y, width, height, index);
      } else {
        arrange_automatic(target.mode, x, y, width, height, index);
      }
    }
    arrange_floating(target);
    sync_pane_tab_bars();
    update_empty_pane_highlight();
    XRaiseWindow(display_, bar_);
    draw_bar();
    draw_docks();
    update_tray();
    XFlush(display_);
  }

  void ensure_empty_pane_highlight() {
    if (empty_pane_highlight_ != None) return;
    XSetWindowAttributes attributes{};
    attributes.override_redirect = True;
    attributes.background_pixel = bar_background_.pixel;
    attributes.event_mask = ExposureMask;
    empty_pane_highlight_ =
        XCreateWindow(display_, root_, 0, 0, 1, 1, config_.border_width, DefaultDepth(display_, screen_),
                     CopyFromParent, DefaultVisual(display_, screen_),
                     CWOverrideRedirect | CWBackPixel | CWEventMask, &attributes);
    XSetWindowBorder(display_, empty_pane_highlight_, border_focused_pixel_);
  }

  void draw_empty_pane_highlight() {
    if (empty_pane_highlight_ == None) return;
    Window root_return; int x, y; unsigned int width, height, border, depth;
    if (!XGetGeometry(display_, empty_pane_highlight_, &root_return, &x, &y, &width, &height, &border, &depth))
      return;
    XSetForeground(display_, bar_gc_, bar_background_.pixel);
    XFillRectangle(display_, empty_pane_highlight_, bar_gc_, 0, 0, width, height);
    const std::string hint = "Empty pane -- Super+Return to open a terminal here";
    draw_dock_text(empty_pane_highlight_, 12, (static_cast<int>(height) + bar_font_->ascent - bar_font_->descent) / 2,
                   hint, bar_foreground_);
  }

  // Shows/hides and positions the one highlight window that marks the
  // currently selected pane when it has no client in it yet -- otherwise
  // there would be nothing on screen to show where a split landed, and
  // Super+hjkl would have no visible effect when moving onto it.
  void update_empty_pane_highlight() {
    Workspace& target = workspace();
    Node* leaf = target.selected_leaf;
    const bool show = target.mode == LayoutMode::Manual && leaf && leaf->is_leaf() && leaf->tabs.empty();
    if (!show) {
      if (empty_pane_highlight_ != None) XUnmapWindow(display_, empty_pane_highlight_);
      return;
    }
    ensure_empty_pane_highlight();
    const Rect* rect = leaf_rect(leaf, current_monitor_);
    if (!rect) {
      XUnmapWindow(display_, empty_pane_highlight_);
      return;
    }
    XMoveResizeWindow(display_, empty_pane_highlight_, rect->x, rect->y, std::max(1, rect->w),
                      std::max(1, rect->h));
    draw_empty_pane_highlight();
    XMapRaised(display_, empty_pane_highlight_);
  }

  Node* pane_tab_bar_leaf_for(Window window) const {
    for (const auto& entry : pane_tab_bars_) {
      if (entry.second == window) return entry.first;
    }
    return nullptr;
  }

  void draw_pane_tab_bar(Node* leaf, Window window, int width) {
    XSetForeground(display_, bar_gc_, bar_background_.pixel);
    XFillRectangle(display_, window, bar_gc_, 0, 0, width, kPaneTabBarHeight);
    const int count = static_cast<int>(leaf->tabs.size());
    if (count == 0) return;
    const int cell_width = std::max(1, width / count);
    for (int index = 0; index < count; ++index) {
      char* raw_title = nullptr;
      std::string label =
          XFetchName(display_, leaf->tabs[index], &raw_title) && raw_title ? raw_title : "untitled";
      if (raw_title) XFree(raw_title);
      if (label.size() > 20) { label.resize(19); label += "\xe2\x80\xa6"; }
      const int x = index * cell_width;
      const int cell = (index == count - 1) ? (width - x) : cell_width;
      draw_dock_cell_h(window, x, cell, kPaneTabBarHeight, label, index == static_cast<int>(leaf->active_tab));
      if (index > 0) {
        XSetForeground(display_, bar_gc_, border_normal_pixel_);
        XFillRectangle(display_, window, bar_gc_, x, 0, 1, kPaneTabBarHeight);
      }
    }
  }

  // Creates/destroys/repositions the small tab-bar window sitting above each
  // manual-mode pane that currently holds more than one client, and redraws
  // each one. Run every arrange() so it always tracks the live tree: a pane
  // gets a bar the moment a second client lands in it, and loses it again
  // the moment it's back down to one (or zero).
  void sync_pane_tab_bars() {
    std::vector<Node*> wanted;
    if (workspace().mode == LayoutMode::Manual) {
      std::vector<Node*> leaves;
      collect_leaves(workspace().root.get(), leaves);
      for (Node* leaf : leaves) {
        if (leaf->tabs.size() > 1) wanted.push_back(leaf);
      }
    }
    for (auto it = pane_tab_bars_.begin(); it != pane_tab_bars_.end();) {
      if (std::find(wanted.begin(), wanted.end(), it->first) == wanted.end()) {
        XDestroyWindow(display_, it->second);
        it = pane_tab_bars_.erase(it);
      } else {
        ++it;
      }
    }
    for (Node* leaf : wanted) {
      auto existing = pane_tab_bars_.find(leaf);
      Window window;
      if (existing == pane_tab_bars_.end()) {
        XSetWindowAttributes attributes{};
        attributes.override_redirect = True;
        attributes.background_pixel = bar_background_.pixel;
        attributes.event_mask = ExposureMask | ButtonPressMask;
        window = XCreateWindow(display_, root_, 0, 0, 1, kPaneTabBarHeight, 0, DefaultDepth(display_, screen_),
                               CopyFromParent, DefaultVisual(display_, screen_),
                               CWOverrideRedirect | CWBackPixel | CWEventMask, &attributes);
        XDefineCursor(display_, window, cursor_);
        pane_tab_bars_.emplace(leaf, window);
      } else {
        window = existing->second;
      }
      const Rect* rect = leaf_rect(leaf, current_monitor_);
      if (!rect) {
        XUnmapWindow(display_, window);
        continue;
      }
      XMoveResizeWindow(display_, window, rect->x, rect->y, std::max(1, rect->w), kPaneTabBarHeight);
      draw_pane_tab_bar(leaf, window, rect->w);
      XMapRaised(display_, window);
    }
  }

  void handle_pane_tab_bar_button(Node* leaf, const XButtonEvent& event) {
    if (leaf->tabs.empty()) return;
    const Rect* rect = leaf_rect(leaf, current_monitor_);
    const int width = rect ? rect->w : 1;
    const int count = static_cast<int>(leaf->tabs.size());
    const int cell_width = std::max(1, width / count);
    const int index = std::clamp(event.x / cell_width, 0, count - 1);
    leaf->active_tab = static_cast<std::size_t>(index);
    workspace().selected_leaf = leaf;
    focus(leaf->tabs[static_cast<std::size_t>(index)]);
    arrange();
  }

  void spawn_command(const std::string& command) const {
    const pid_t child = fork();
    if (child < 0) {
      std::cerr << "mepwm: could not start command: " << std::strerror(errno) << '\n';
      return;
    }
    if (child == 0) {
      setsid();
      execl("/bin/sh", "sh", "-c", command.c_str(), static_cast<char*>(nullptr));
      _exit(127);
    }
    signal(SIGCHLD, SIG_IGN);
  }

  void spawn_terminal() const { spawn_command(config_.terminal); }

  // Independent of config_.terminal (which is a full launch expression, not
  // just a binary), so the agent command is run in its own terminal rather
  // than trying to splice it into an arbitrary user-configured launcher.
  void spawn_terminal_running(const std::string& directory, const std::string& command) const {
    const pid_t child = fork();
    if (child < 0) {
      std::cerr << "mepwm: could not start agent: " << std::strerror(errno) << '\n';
      return;
    }
    if (child == 0) {
      setsid();
      if (chdir(directory.c_str()) != 0) _exit(127);
      setenv("MEPWM_AGENT_COMMAND", command.c_str(), 1);
      const char* wrapped =
          "command -v kitty >/dev/null 2>&1 && exec kitty -e sh -c \"$MEPWM_AGENT_COMMAND\" "
          "|| exec xterm -e sh -c \"$MEPWM_AGENT_COMMAND\"";
      execl("/bin/sh", "sh", "-c", wrapped, static_cast<char*>(nullptr));
      _exit(127);
    }
    signal(SIGCHLD, SIG_IGN);
  }

  void spawn_terminal_in(const std::string& directory) const {
    const pid_t child = fork();
    if (child < 0) {
      std::cerr << "mepwm: could not start terminal: " << std::strerror(errno) << '\n';
      return;
    }
    if (child == 0) {
      setsid();
      if (chdir(directory.c_str()) != 0) _exit(127);
      execl("/bin/sh", "sh", "-c", config_.terminal.c_str(), static_cast<char*>(nullptr));
      _exit(127);
    }
    signal(SIGCHLD, SIG_IGN);
  }

  // Makes a tiled window floating mid-drag, at the given live geometry, and
  // re-tiles everything else immediately (mirrors dwm's drag-to-float).
  void make_floating_in_place(Window window, int x, int y, int width, int height) {
    Workspace& target = workspace();
    WindowState& state = window_state_[window];
    if (state.floating) return;
    remove_tiled(target, window);
    state.floating = true;
    state.float_x = x;
    state.float_y = y;
    state.float_w = width;
    state.float_h = height;
    insert_floating(target, window);
    arrange();
  }

  void move_window(Window window, int start_x_root, int start_y_root) {
    WindowState* state = &window_state_[window];
    if (state->fullscreen) return;
    XWindowAttributes attributes;
    if (!XGetWindowAttributes(display_, window, &attributes)) return;
    const int origin_x = attributes.x;
    const int origin_y = attributes.y;
    bool floated_mid_drag = state->floating;

    if (XGrabPointer(display_, root_, False, PointerMotionMask | ButtonReleaseMask, GrabModeAsync,
                     GrabModeAsync, None, cursor_, CurrentTime) != GrabSuccess) {
      return;
    }

    const int gap = static_cast<int>(config_.gap);
    const Monitor& source_monitor = monitor(state->monitor);
    const int area_x = source_monitor.x + gap;
    const int area_y = source_monitor.y + kBarHeight + gap;
    const int area_w = source_monitor.width - 2 * gap;
    const int area_h = source_monitor.height - kBarHeight - 2 * gap;
    const int snap = static_cast<int>(config_.snap);

    bool dragging = true;
    while (dragging) {
      XEvent event;
      XMaskEvent(display_, PointerMotionMask | ButtonReleaseMask, &event);
      if (event.type == MotionNotify) {
        int new_x = origin_x + (event.xmotion.x_root - start_x_root);
        int new_y = origin_y + (event.xmotion.y_root - start_y_root);
        if (std::abs(new_x - area_x) < snap) new_x = area_x;
        if (std::abs(new_y - area_y) < snap) new_y = area_y;
        if (std::abs((new_x + attributes.width) - (area_x + area_w)) < snap) {
          new_x = area_x + area_w - attributes.width;
        }
        if (std::abs((new_y + attributes.height) - (area_y + area_h)) < snap) {
          new_y = area_y + area_h - attributes.height;
        }

        if (!floated_mid_drag) {
          if (std::abs(new_x - origin_x) > snap || std::abs(new_y - origin_y) > snap) {
            floated_mid_drag = true;
            make_floating_in_place(window, new_x, new_y, attributes.width, attributes.height);
          }
        } else {
          window_state_[window].float_x = new_x;
          window_state_[window].float_y = new_y;
          XMoveWindow(display_, window, new_x, new_y);
        }
      } else if (event.type == ButtonRelease) {
        if (floated_mid_drag) send_to_monitor(window, monitor_at(event.xbutton.x_root, event.xbutton.y_root));
        dragging = false;
      }
    }
    XUngrabPointer(display_, CurrentTime);
  }

  void resize_window(Window window) {
    WindowState* state = &window_state_[window];
    if (state->fullscreen) return;
    XWindowAttributes attributes;
    if (!XGetWindowAttributes(display_, window, &attributes)) return;
    bool floated_mid_drag = state->floating;
    const int origin_w = attributes.width;
    const int origin_h = attributes.height;

    if (XGrabPointer(display_, root_, False, PointerMotionMask | ButtonReleaseMask, GrabModeAsync,
                     GrabModeAsync, None, cursor_, CurrentTime) != GrabSuccess) {
      return;
    }
    XWarpPointer(display_, None, window, 0, 0, 0, 0, attributes.width, attributes.height);

    const int snap = static_cast<int>(config_.snap);
    bool dragging = true;
    while (dragging) {
      XEvent event;
      XMaskEvent(display_, PointerMotionMask | ButtonReleaseMask, &event);
      if (event.type == MotionNotify) {
        const int new_w = std::max(kMinWindowSize, event.xmotion.x_root - attributes.x);
        const int new_h = std::max(kMinWindowSize, event.xmotion.y_root - attributes.y);

        if (!floated_mid_drag) {
          if (std::abs(new_w - origin_w) > snap || std::abs(new_h - origin_h) > snap) {
            floated_mid_drag = true;
            make_floating_in_place(window, attributes.x, attributes.y, new_w, new_h);
          }
        } else {
          window_state_[window].float_w = new_w;
          window_state_[window].float_h = new_h;
          XResizeWindow(display_, window, new_w, new_h);
        }
      } else if (event.type == ButtonRelease) {
        if (floated_mid_drag) send_to_monitor(window, monitor_at(event.xbutton.x_root, event.xbutton.y_root));
        dragging = false;
      }
    }
    XUngrabPointer(display_, CurrentTime);
  }

  void configure_notify_echo(Window window) {
    XWindowAttributes attributes;
    if (!XGetWindowAttributes(display_, window, &attributes)) return;
    XConfigureEvent event{};
    event.type = ConfigureNotify;
    event.display = display_;
    event.event = window;
    event.window = window;
    event.x = attributes.x;
    event.y = attributes.y;
    event.width = attributes.width;
    event.height = attributes.height;
    event.border_width = attributes.border_width;
    event.above = None;
    event.override_redirect = False;
    XSendEvent(display_, window, False, StructureNotifyMask, reinterpret_cast<XEvent*>(&event));
  }

  void dispatch(const XEvent& event) {
    switch (event.type) {
      case MapRequest:
        if (is_tray_icon(event.xmaprequest.window)) {
          update_tray();
        } else {
          manage(event.xmaprequest.window);
          XMapWindow(display_, event.xmaprequest.window);
          arrange();
        }
        break;
      case ConfigureRequest: {
        const XConfigureRequestEvent& request = event.xconfigurerequest;
        const int index = find_workspace(request.window);
        if (index >= 0) {
          const WindowState& state = window_state_[request.window];
          if (state.floating && !state.fullscreen) {
            WindowState& mutable_state = window_state_[request.window];
            if (request.value_mask & CWX) mutable_state.float_x = request.x;
            if (request.value_mask & CWY) mutable_state.float_y = request.y;
            if (request.value_mask & CWWidth) mutable_state.float_w = std::max(1, request.width);
            if (request.value_mask & CWHeight) mutable_state.float_h = std::max(1, request.height);
            if (index == current_workspace_) {
              resize(request.window, mutable_state.float_x, mutable_state.float_y,
                     mutable_state.float_w, mutable_state.float_h);
            }
          } else {
            configure_notify_echo(request.window);
          }
          break;
        }
        XWindowChanges changes{request.x, request.y, request.width, request.height,
                               request.border_width, request.above, request.detail};
        XConfigureWindow(display_, request.window, request.value_mask, &changes);
        break;
      }
      case DestroyNotify:
        if (is_tray_icon(event.xdestroywindow.window)) {
          remove_tray_icon(event.xdestroywindow.window);
          break;
        }
        expected_unmaps_.erase(event.xdestroywindow.window);
        unmanage(event.xdestroywindow.window);
        break;
      case UnmapNotify:
        if (is_tray_icon(event.xunmap.window)) {
          update_tray();
        } else if (expected_unmaps_.erase(event.xunmap.window) == 0) {
          unmanage(event.xunmap.window);
        }
        break;
      case EnterNotify:
        if (event.xcrossing.window != bar_ && event.xcrossing.window != launcher_window_ &&
            dock_monitor(event.xcrossing.window) < 0) focus(event.xcrossing.window);
        break;
      case ButtonPress:
        // Any click outside a popup's own window closes that popup first, then
        // falls through to normal click routing below so the click still does
        // whatever it would otherwise do (focus a client, hit a dock icon, ...).
        // Dock clicks are exempt: handle_dock_button/handle_bottom_widget already
        // toggle/switch popups correctly based on the pre-click state, so
        // force-closing here first would make re-clicking a widget always
        // reopen it instead of closing it.
        if (hints_visible_) close_hints();
        if (dock_monitor(event.xbutton.window) < 0) {
          if (side_panel_ != SidePanel::Closed && event.xbutton.window != side_panel_window_) close_side_panel();
          if (launcher_visible_ && event.xbutton.window != launcher_window_) close_launcher();
          if (slider_visible_ && event.xbutton.window != slider_window_) close_slider_popup();
        }

        if (side_panel_ != SidePanel::Closed && event.xbutton.window == side_panel_window_) {
          handle_side_panel_button(event.xbutton);
        } else if (slider_visible_ && event.xbutton.window == slider_window_) {
          if (event.xbutton.button == Button1) {
            slider_dragging_ = true;
            handle_slider_position(event.xbutton.x);
          }
        } else if (event.xbutton.window == launcher_window_) {
          if (event.xbutton.button == Button4) move_launcher_selection(-1);
          else if (event.xbutton.button == Button5) move_launcher_selection(1);
          else if (event.xbutton.button == Button1 && event.xbutton.y >= kBarHeight) {
            const std::size_t row = static_cast<std::size_t>(event.xbutton.y / kBarHeight - 1);
            if (row < launcher_match_count() - launcher_scroll_ && row < kLauncherMaxRows) {
              launcher_selection_ = launcher_scroll_ + row;
              launch_selected_app();
              break;
            }
          }
          if (launcher_visible_) draw_launcher();
        } else if (dock_monitor(event.xbutton.window) >= 0) {
          handle_dock_button(event.xbutton);
        } else if (Node* tab_bar_leaf = pane_tab_bar_leaf_for(event.xbutton.window)) {
          handle_pane_tab_bar_button(tab_bar_leaf, event.xbutton);
        } else {
          handle_button(event.xbutton);
        }
        break;
      case ButtonRelease:
        if (slider_visible_ && event.xbutton.window == slider_window_ && event.xbutton.button == Button1)
          slider_dragging_ = false;
        break;
      case MotionNotify:
        if (slider_visible_ && slider_dragging_ && event.xmotion.window == slider_window_)
          handle_slider_position(event.xmotion.x);
        break;
      case Expose:
        if (event.xexpose.window == bar_ && event.xexpose.count == 0) draw_bar();
        if (event.xexpose.window == launcher_window_ && event.xexpose.count == 0) draw_launcher();
        if (event.xexpose.window == slider_window_ && event.xexpose.count == 0) draw_slider_popup();
        if (event.xexpose.window == side_panel_window_ && event.xexpose.count == 0) draw_side_panel();
        if (event.xexpose.window == empty_pane_highlight_ && event.xexpose.count == 0) draw_empty_pane_highlight();
        if (event.xexpose.count == 0) {
          if (Node* leaf = pane_tab_bar_leaf_for(event.xexpose.window)) {
            const Rect* rect = leaf_rect(leaf, current_monitor_);
            draw_pane_tab_bar(leaf, event.xexpose.window, rect ? rect->w : 1);
          }
        }
        if (dock_monitor(event.xexpose.window) >= 0 && event.xexpose.count == 0) draw_docks();
        break;
      case ClientMessage:
        handle_client_message(event.xclient);
        break;
      case ConfigureNotify:
        if (is_tray_icon(event.xconfigure.window)) {
          for (TrayIcon& icon : tray_icons_) {
            if (icon.window != event.xconfigure.window) continue;
            icon.width = std::clamp(event.xconfigure.height > 0
                                        ? event.xconfigure.width * kBarHeight / event.xconfigure.height
                                        : kBarHeight,
                                    1, kBarHeight * 2);
            icon.height = kBarHeight;
            break;
          }
          update_tray();
          break;
        }
        if (event.xconfigure.window == root_) {
          refresh_display_geometry();
          arrange();
        }
        break;
      case KeyPress:
        if (hints_visible_) handle_hint_key(event.xkey);
        else if (launcher_visible_) handle_launcher_key(event.xkey);
        else if (side_panel_ == SidePanel::Todos) handle_todo_key(event.xkey);
        else handle_key(event.xkey);
        break;
      case PropertyNotify:
        if (is_tray_icon(event.xproperty.window) && event.xproperty.atom == xembed_info_atom_) {
          for (TrayIcon& icon : tray_icons_) {
            if (icon.window == event.xproperty.window) update_tray_icon_state(icon);
          }
          update_tray();
        }
        if (find_workspace(event.xproperty.window) == current_workspace_) draw_bar();
        break;
      default:
        break;
    }
  }

  void handle_key(const XKeyEvent& event) {
    const KeySym key = XkbKeycodeToKeysym(display_, event.keycode, 0, 0);
    const unsigned int state = event.state & ~(LockMask | Mod2Mask);
    for (const LuaKeybind& binding : lua_keybinds_) {
      if (binding.keycode != event.keycode || binding.modifiers != state) continue;
      lua_rawgeti(lua_, LUA_REGISTRYINDEX, binding.callback);
      if (lua_pcall(lua_, 0, 0, 0) != LUA_OK) {
        std::cerr << "mepwm: Lua keybind error: " << lua_tostring(lua_, -1) << '\n';
        lua_pop(lua_, 1);
      }
    }
    if (state == (Mod4Mask | ShiftMask)) {
      if (key == XK_q) running_ = false;
      if (key == XK_space) toggle_floating();
      if (key == XK_c) kill_focused();
      if (key == XK_minus) set_scratchpad();
      if (key == XK_comma) send_to_monitor(workspace().focused,
                                           (current_monitor_ + monitors_.size() - 1) % monitors_.size());
      if (key == XK_period) send_to_monitor(workspace().focused,
                                            (current_monitor_ + 1) % monitors_.size());
      if (key == XK_h || key == XK_j || key == XK_k || key == XK_l) resize_pane(key);
      if (key == XK_Tab) next_tab(-1);
      if (key == XK_r) restart();
      if (key == XK_d) adjust_nmaster(1);
      if (key == XK_t) open_theme_panel();
      if (key == XK_slash) toggle_side_panel(SidePanel::Help);
      return;
    }
    if (state == (Mod4Mask | ControlMask)) {
      if (key == XK_h) adjust_mfact(-kResizeStep);
      if (key == XK_l) adjust_mfact(kResizeStep);
      if (key == XK_j) focus_stack(1);
      if (key == XK_k) focus_stack(-1);
      return;
    }
    if (state != Mod4Mask) return;
    if (key == XK_Return) spawn_terminal();
    if (key == XK_i) toggle_project_picker(false);
    if (key == XK_o) toggle_project_picker(true);
    if (key == XK_p) toggle_launcher();
    if (key == XK_w) open_window_switcher();
    if (key == XK_f) toggle_hints();
    if (key == XK_b) toggle_bar();
    if (key == XK_d) { if (workspace().mode == LayoutMode::Manual) merge_pane(); else adjust_nmaster(-1); }
    if (key == XK_h || key == XK_j || key == XK_k || key == XK_l) {
      if (workspace().mode == LayoutMode::Manual) select_pane_direction(key);
      else focus_direction(key);
    }
    if (key == XK_v) split(Orientation::Vertical);
    if (key == XK_s) split(Orientation::Horizontal);
    if (key == XK_Tab) next_tab();
    if (key == XK_space || key == XK_a) cycle_layout();
    if (key == XK_z) zoom();
    if (key == XK_m) toggle_maximize();
    if (key == XK_minus) toggle_scratchpad();
    if (key == XK_r) reload_lua();
    if (key == XK_comma) focus_monitor(-1);
    if (key == XK_period) focus_monitor(1);
    if (key >= XK_1 && key <= XK_9) switch_workspace(static_cast<int>(key - XK_1));
  }

  void handle_button(const XButtonEvent& event) {
    const unsigned int state = event.state & ~(LockMask | Mod2Mask);
    const std::string context = event.window == root_ ? "root" : event.window == bar_ ? "tagbar" : "client";
    for (const LuaMousebind& binding : lua_mousebinds_) {
      if (binding.context != context || binding.button != event.button || binding.modifiers != state) continue;
      lua_rawgeti(lua_, LUA_REGISTRYINDEX, binding.callback);
      if (lua_pcall(lua_, 0, 0, 0) != LUA_OK) {
        std::cerr << "mepwm: Lua mousebind error: " << lua_tostring(lua_, -1) << '\n';
        lua_pop(lua_, 1);
      }
    }
    if (event.window == bar_) {
      // Launcher and layout-mode toggle live in the outer kDockWidth corners
      // now that the bar spans the full display width.
      if (event.x < kDockWidth) {
        if (event.button == Button1) toggle_launcher();
        return;
      }
      if (event.x >= DisplayWidth(display_, screen_) - kDockWidth) {
        if (event.button == Button1) cycle_layout();
        if (event.button == Button3) { cycle_layout(); cycle_layout(); }
        return;
      }
      const int workspace_index = (event.x - kDockWidth) / 84;
      if (workspace_index >= 0 && workspace_index < kWorkspaceCount) {
        switch_workspace(workspace_index);
      } else if (event.x >= kDockWidth + 756 && event.x < bar_task_list_x_) {
        if (event.button == Button1) cycle_layout();
        if (event.button == Button3) {
          workspace().mode = LayoutMode::Monocle;
          arrange();
        }
      } else {
        for (const BarHit& hit : task_hits_) {
          if (event.x >= hit.left && event.x < hit.right) {
            focus(hit.window);
            arrange();
            break;
          }
        }
      }
      return;
    }
    if (find_workspace(event.window) != current_workspace_) return;
    focus(event.window);
    if (state == Mod4Mask && event.button == Button1) {
      XAllowEvents(display_, AsyncPointer, CurrentTime);
      move_window(event.window, event.x_root, event.y_root);
      arrange();
      return;
    }
    if (state == Mod4Mask && event.button == Button3) {
      XAllowEvents(display_, AsyncPointer, CurrentTime);
      resize_window(event.window);
      arrange();
      return;
    }
    // The passive grab is synchronous only long enough to focus the window;
    // replaying it lets the application receive its original click.
    XAllowEvents(display_, ReplayPointer, CurrentTime);
  }

  Config config_;
  Display* display_ = nullptr;
  int screen_ = 0;
  Window root_ = None;
  Window bar_ = None;
  Window tray_ = None;
  std::vector<DockWindows> docks_;
  StartupTimer* startup_timer_ = nullptr;
  bool defer_widget_refresh_ = true;
  std::vector<WidgetHit> bottom_widget_hits_;
  std::time_t widgets_refreshed_ = 0;
  std::string battery_capacity_path_, battery_status_path_, backlight_path_, backlight_max_path_;
  long battery_percent_ = -1;
  bool battery_charging_ = false;
  int backlight_percent_ = -1;
  long cpu_previous_total_ = -1, cpu_previous_idle_ = -1;
  int cpu_percent_ = -1, mem_percent_ = -1, disk_percent_ = -1;
  std::string network_interface_, wifi_interface_, media_title_, clock_text_;
  std::string git_status_;
  bool network_up_ = false;
  int volume_percent_ = -1, mic_percent_ = -1;
  bool volume_muted_ = false, mic_muted_ = false;
  Window slider_window_ = None;
  bool slider_visible_ = false;
  bool slider_dragging_ = false;
  SliderKind slider_kind_ = SliderKind::Backlight;
  Window empty_pane_highlight_ = None;
  std::unordered_map<Node*, Window> pane_tab_bars_;
  Window side_panel_window_ = None;
  SidePanel side_panel_ = SidePanel::Closed;
  std::size_t side_panel_monitor_ = 0;
  std::vector<int> side_panel_rows_;
  int side_panel_scroll_offset_ = 0;
  std::vector<Notification> notifications_;
  unsigned int next_notification_id_ = 1;
  std::vector<TodoItem> todos_;
  int todo_selected_ = -1;
  bool todo_input_active_ = false;
  std::string todo_input_text_;
  std::string info_panel_title_;
  std::string info_panel_body_;
  InfoAction info_action_ = InfoAction::NoneAction;
  std::vector<WifiNetwork> wifi_networks_;
  std::vector<BluetoothDevice> bluetooth_devices_;
  DBusConnection* notification_dbus_ = nullptr;
  bool notification_dbus_owned_ = false;
  std::string wifi_ssid_;
  std::string bluetooth_status_;
  bool wifi_blocked_ = false;
  bool bluetooth_blocked_ = false;
  std::string keyboard_layout_;
  std::vector<std::string> keyboard_layouts_;
  int theme_index_ = 0;
  std::vector<std::array<std::string, 3>> theme_palettes_ = {
      {{"#f8f8f2", "#202124", "#5294e2"}}, {{"#202124", "#f4f4f4", "#3971ed"}}, {{"#d8dee9", "#2e3440", "#88c0d0"}}};
  std::vector<std::string> theme_names_ = {"dark", "blue", "nord"};
  std::optional<bool> wallpaper_is_light_;
  std::mt19937 wallpaper_rng_{std::random_device{}()};
  std::vector<AgentStatus> agents_;
  int agent_needs_input_ = 0;
  GC bar_gc_ = nullptr;
  Pixmap bar_pixmap_ = None;
  int bar_pixmap_width_ = 0;
  XftDraw* bar_xft_draw_ = nullptr;
  Window launcher_window_ = None;
  Pixmap launcher_pixmap_ = None;
  XftDraw* launcher_xft_draw_ = nullptr;
  bool launcher_visible_ = false;
  std::string launcher_query_;
  std::vector<LauncherApp> launcher_apps_;
  std::vector<std::size_t> launcher_matches_;
  LauncherMode launcher_mode_ = LauncherMode::Applications;
  std::vector<std::size_t> project_matches_;
  std::vector<std::pair<Window, std::string>> window_candidates_;
  std::vector<std::size_t> window_matches_;
  std::size_t launcher_selection_ = 0;
  std::size_t launcher_scroll_ = 0;
  XftFont* bar_font_ = nullptr;
  XftFont* icon_font_ = nullptr;
  XftColor bar_foreground_{};
  XftColor bar_background_{};
  XftColor bar_selected_{};
  std::unordered_map<FcChar32, XftFont*> fallback_fonts_;
  std::vector<BarHit> task_hits_;
  int bar_task_list_x_ = 960;
  bool hints_visible_ = false;
  std::string hint_query_;
  std::vector<Hint> hints_;
  std::vector<Window> hint_windows_;
  XftFont* hint_font_ = nullptr;
  XftColor hint_background_{};
  XftColor hint_foreground_{};
  XftColor hint_matched_{};
  Cursor cursor_ = None;
  std::array<Workspace, kWorkspaceCount> workspaces_;
  std::vector<Project> projects_;
  std::size_t active_project_index_ = 0;
  int current_workspace_ = 0;
  std::vector<Monitor> monitors_;
  std::size_t current_monitor_ = 0;
  std::unordered_set<Window> expected_unmaps_;
  std::unordered_map<Window, WindowState> window_state_;
  Window previously_focused_ = None;
  Window scratchpad_ = None;
  bool scratchpad_hidden_ = false;
  bool bar_visible_ = true;
  unsigned long border_normal_pixel_ = 0;
  unsigned long border_focused_pixel_ = 0;
  bool running_ = true;
  lua_State* lua_ = nullptr;
  int ipc_fd_ = -1;
  std::string ipc_path_;
  std::vector<LuaRule> lua_rules_;
  std::vector<LuaKeybind> lua_keybinds_;
  std::vector<LuaMousebind> lua_mousebinds_;
  std::vector<LuaWidget> lua_widgets_;

  // EWMH
  Window wm_check_window_ = None;
  Atom net_supported_atom_ = None;
  Atom net_supporting_wm_check_atom_ = None;
  Atom net_wm_name_atom_ = None;
  Atom utf8_string_atom_ = None;
  Atom net_active_window_atom_ = None;
  Atom net_client_list_atom_ = None;
  Atom net_wm_state_atom_ = None;
  Atom net_wm_state_fullscreen_atom_ = None;
  Atom net_wm_window_type_atom_ = None;
  Atom net_wm_window_type_dialog_atom_ = None;
  Atom net_wm_pid_atom_ = None;
  Atom wm_protocols_atom_ = None;
  Atom wm_delete_window_atom_ = None;
  Atom manager_atom_ = None;
  Atom net_system_tray_atom_ = None;
  Atom net_system_tray_opcode_atom_ = None;
  Atom net_system_tray_orientation_atom_ = None;
  Atom xembed_atom_ = None;
  Atom xembed_info_atom_ = None;
  std::vector<TrayIcon> tray_icons_;
};

}  // namespace

std::unique_ptr<Backend> make_x11_backend() { return std::make_unique<X11Backend>(); }

}  // namespace mepwm
