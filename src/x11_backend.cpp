#include "backend.hpp"

#include <X11/XKBlib.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xft/Xft.h>
#include <X11/extensions/Xinerama.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>
#include <lua.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <ctime>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace mepwm {
namespace {

constexpr int kWorkspaceCount = 9;
constexpr int kBarHeight = 24;
constexpr int kMinWindowSize = 20;
constexpr int kTraySpacing = 4;
constexpr long kSystemTrayRequestDock = 0;
constexpr long kXEmbedEmbeddedNotify = 0;
constexpr long kXEmbedMapped = 1 << 0;
int another_window_manager = 0;

enum class Orientation { Vertical, Horizontal };
enum class LayoutMode { Manual, MasterStack, Monocle };

struct Node {
  Node* parent = nullptr;
  Orientation orientation = Orientation::Vertical;
  std::vector<std::unique_ptr<Node>> children;
  std::vector<Window> tabs;
  std::size_t active_tab = 0;

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
};
struct LuaMousebind { std::string context; unsigned int button = 0, modifiers = 0; int callback = LUA_NOREF; };

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
    if (lua_) lua_close(lua_);
    destroy_tray();
    if (bar_font_) {
      XftColorFree(display_, DefaultVisual(display_, screen_), DefaultColormap(display_, screen_), &bar_foreground_);
      XftColorFree(display_, DefaultVisual(display_, screen_), DefaultColormap(display_, screen_), &bar_background_);
      XftColorFree(display_, DefaultVisual(display_, screen_), DefaultColormap(display_, screen_), &bar_selected_);
    }
    for (auto& entry : fallback_fonts_) if (entry.second != bar_font_) XftFontClose(display_, entry.second);
    if (bar_xft_draw_) XftDrawDestroy(bar_xft_draw_);
    if (bar_font_) XftFontClose(display_, bar_font_);
    if (bar_pixmap_) XFreePixmap(display_, bar_pixmap_);
    if (bar_gc_) XFreeGC(display_, bar_gc_);
    if (cursor_) XFreeCursor(display_, cursor_);
    if (display_) XCloseDisplay(display_);
  }

  int run(const Config& config) override {
    config_ = config;
    display_ = XOpenDisplay(nullptr);
    if (!display_) throw std::runtime_error("could not open the X display");

    screen_ = DefaultScreen(display_);
    root_ = RootWindow(display_, screen_);
    XSetIOErrorHandler(on_x_io_error);
    cursor_ = XCreateFontCursor(display_, XC_left_ptr);
    XDefineCursor(display_, root_, cursor_);
    another_window_manager = 0;
    XSetErrorHandler(on_x_error);
    XSelectInput(display_, root_, SubstructureRedirectMask | SubstructureNotifyMask |
                                      StructureNotifyMask);
    XSync(display_, False);
    if (another_window_manager) {
      throw std::runtime_error("another window manager is already running on this display");
    }

    border_normal_pixel_ = alloc_color(config_.border_color_normal);
    border_focused_pixel_ = alloc_color(config_.border_color_focused);
    update_monitors();

    create_bar();
    setup_ewmh();
    create_tray();
    initialize_lua();
    create_ipc();
    grab_keys();
    adopt_existing_windows();
    arrange();
    std::cerr << "mepwm: managing X display " << DisplayString(display_) << '\n';

    while (running_) {
      pollfd fds[] = {{ConnectionNumber(display_), POLLIN, 0}, {ipc_fd_, POLLIN, 0}};
      const int count = ipc_fd_ >= 0 ? 2 : 1;
      const int ready = poll(fds, count, 1000);
      if (ready < 0 && errno != EINTR) break;
      if (ready == 0) draw_bar();
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
    backend->lua_keybinds_.push_back({keycode, modifiers, callback});
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

  void initialize_lua() {
    if (lua_) lua_close(lua_);
    lua_ = luaL_newstate();
    if (!lua_) throw std::runtime_error("could not create Lua state");
    luaL_openlibs(lua_);
    lua_pushlightuserdata(lua_, this);
    lua_setfield(lua_, LUA_REGISTRYINDEX, "mepwm.backend");
    lua_newtable(lua_);
    lua_pushcfunction(lua_, lua_set_terminal); lua_setfield(lua_, -2, "set_terminal");
    lua_pushcfunction(lua_, lua_set_mfact); lua_setfield(lua_, -2, "set_mfact");
    lua_pushcfunction(lua_, lua_set_nmaster); lua_setfield(lua_, -2, "set_nmaster");
    lua_pushcfunction(lua_, lua_set_snap); lua_setfield(lua_, -2, "set_snap");
    lua_pushcfunction(lua_, lua_set_gaps); lua_setfield(lua_, -2, "set_gaps");
    lua_pushcfunction(lua_, lua_set_border_width); lua_setfield(lua_, -2, "set_border_width");
    lua_pushcfunction(lua_, lua_rule); lua_setfield(lua_, -2, "rule");
    lua_pushcfunction(lua_, lua_keybind); lua_setfield(lua_, -2, "keybind");
    lua_pushcfunction(lua_, lua_mousebind); lua_setfield(lua_, -2, "mousebind");
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
    lua_rules_.clear();
    lua_keybinds_.clear();
    lua_mousebinds_.clear();
    initialize_lua();
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
    XMoveResizeWindow(display_, tray_, DisplayWidth(display_, screen_) - width, 0, width, kBarHeight);
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

  void create_bar() {
    bar_ = XCreateSimpleWindow(display_, root_, 0, 0, DisplayWidth(display_, screen_), kBarHeight,
                               0, BlackPixel(display_, screen_), BlackPixel(display_, screen_));
    XSetWindowAttributes attributes{};
    attributes.override_redirect = True;
    XChangeWindowAttributes(display_, bar_, CWOverrideRedirect, &attributes);
    XSelectInput(display_, bar_, ExposureMask | ButtonPressMask);
    XStoreName(display_, bar_, "mepwm-bar");
    XDefineCursor(display_, bar_, cursor_);
    bar_gc_ = XCreateGC(display_, bar_, 0, nullptr);
    bar_font_ = XftFontOpenName(display_, screen_, "sans-10");
    if (!bar_font_) throw std::runtime_error("could not open an Xft bar font");
    XftColorAllocName(display_, DefaultVisual(display_, screen_), DefaultColormap(display_, screen_),
                      "#f8f8f2", &bar_foreground_);
    XftColorAllocName(display_, DefaultVisual(display_, screen_), DefaultColormap(display_, screen_),
                      "#202124", &bar_background_);
    XftColorAllocName(display_, DefaultVisual(display_, screen_), DefaultColormap(display_, screen_),
                      "#5294e2", &bar_selected_);
    XMapRaised(display_, bar_);
    draw_bar();
  }

  XftFont* fallback_font(FcChar32 codepoint) {
    const auto cached = fallback_fonts_.find(codepoint);
    if (cached != fallback_fonts_.end()) return cached->second;
    FcPattern* pattern = FcNameParse(reinterpret_cast<const FcChar8*>("sans-10"));
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
    const int width = DisplayWidth(display_, screen_);
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

    for (int index = 0; index < kWorkspaceCount; ++index) {
      const int x = index * 42;
      std::vector<Window> occupied;
      collect_windows(workspaces_[index].root.get(), occupied);
      occupied.insert(occupied.end(), workspaces_[index].floating.begin(), workspaces_[index].floating.end());
      if (index == current_workspace_) {
        XSetForeground(display_, bar_gc_, bar_selected_.pixel);
        XFillRectangle(display_, bar_pixmap_, bar_gc_, x, 0, 38, kBarHeight);
      }
      if (!occupied.empty()) {
        XSetForeground(display_, bar_gc_, index == current_workspace_ ? bar_background_.pixel : bar_foreground_.pixel);
        XFillRectangle(display_, bar_pixmap_, bar_gc_, x + 3, 9, 5, 5);
      }
      text(x + 12, std::to_string(index + 1), index == current_workspace_ ? bar_background_ : bar_foreground_);
    }

    const char* mode = workspace().mode == LayoutMode::Manual
                           ? "manual"
                           : workspace().mode == LayoutMode::MasterStack ? "master-stack" : "monocle";
    text(390, mode, bar_foreground_);
    const std::time_t now = std::time(nullptr);
    char clock[16];
    std::strftime(clock, sizeof(clock), "%H:%M", std::localtime(&now));
    const int clock_width = text_width(clock) + 16;
    text(width - clock_width, clock, bar_foreground_);
    task_hits_.clear();
    std::vector<Window> windows;
    collect_windows(workspace().root.get(), windows);
    windows.insert(windows.end(), workspace().floating.begin(), workspace().floating.end());
    int x = 480;
    const int end = tray_ == None ? width - clock_width : std::max(480, width - 140 - clock_width);
    for (Window window : windows) {
      char* title = nullptr;
      std::string label = XFetchName(display_, window, &title) && title ? title : "untitled";
      if (title) XFree(title);
      if (label.size() > 24) label.resize(23), label += "…";
      const int item_width = text_width(label) + 16;
      if (x + item_width > end) break;
      if (window == workspace().focused) {
        XSetForeground(display_, bar_gc_, bar_selected_.pixel);
        XFillRectangle(display_, bar_pixmap_, bar_gc_, x, 2, item_width, kBarHeight - 4);
      }
      text(x + 8, label, window == workspace().focused ? bar_background_ : bar_foreground_);
      task_hits_.push_back({x, x + item_width, window});
      x += item_width + 1;
    }
    XCopyArea(display_, bar_pixmap_, bar_, bar_gc_, 0, 0, width, kBarHeight, 0, 0);
    XFlush(display_);
  }

  void grab_keys() {
    const unsigned int ignored_modifiers[] = {0, LockMask, Mod2Mask, LockMask | Mod2Mask};
    const KeySym plain_keys[] = {XK_Return, XK_q, XK_j, XK_k, XK_v, XK_s, XK_Tab, XK_space, XK_a,
                                 XK_z, XK_m, XK_r, XK_minus, XK_comma, XK_period, XK_1, XK_2, XK_3, XK_4, XK_5,
                                 XK_6, XK_7, XK_8, XK_9};
    const KeySym shift_keys[] = {XK_q, XK_space, XK_c, XK_minus, XK_comma, XK_period};
    for (const unsigned int ignored : ignored_modifiers) {
      for (const KeySym key : plain_keys) {
        XGrabKey(display_, XKeysymToKeycode(display_, key), Mod4Mask | ignored, root_, True,
                 GrabModeAsync, GrabModeAsync);
      }
      for (const KeySym key : shift_keys) {
        XGrabKey(display_, XKeysymToKeycode(display_, key), Mod4Mask | ShiftMask | ignored, root_,
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
    children.erase(std::remove_if(children.begin(), children.end(),
                                  [leaf](const std::unique_ptr<Node>& child) {
                                    return child.get() == leaf;
                                  }),
                   children.end());
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
    if (index < 0) return;
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

  void focus_relative(int offset) {
    std::vector<Window> windows;
    collect_windows(workspace().root.get(), windows);
    windows.insert(windows.end(), workspace().floating.begin(), workspace().floating.end());
    if (windows.empty()) return;
    const auto found = std::find(windows.begin(), windows.end(), workspace().focused);
    const int current = found == windows.end() ? 0 : static_cast<int>(found - windows.begin());
    const int next = (current + offset + static_cast<int>(windows.size())) % static_cast<int>(windows.size());
    focus(windows[next]);
    arrange();
  }

  void next_tab() {
    Node* leaf = workspace().selected_leaf;
    if (!leaf || leaf->tabs.size() < 2) return;
    leaf->active_tab = (leaf->active_tab + 1) % leaf->tabs.size();
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
    target.selected_leaf = new_leaf.get();
    container->children.push_back(std::move(old_leaf));
    container->children.push_back(std::move(new_leaf));
    *slot = std::move(container);
    arrange();
  }

  void cycle_layout() {
    Workspace& target = workspace();
    target.mode = target.mode == LayoutMode::Manual
                      ? LayoutMode::MasterStack
                      : target.mode == LayoutMode::MasterStack ? LayoutMode::Monocle : LayoutMode::Manual;
    arrange();
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
    if (leaf->tabs.empty()) return;
    leaf->active_tab %= leaf->tabs.size();
    for (std::size_t index = 0; index < leaf->tabs.size(); ++index) {
      if (window_state_[leaf->tabs[index]].monitor != monitor_index) continue;
      if (index == leaf->active_tab) {
        resize(leaf->tabs[index], x, y, width, height);
      } else {
        hide_window(leaf->tabs[index]);
      }
    }
  }

  void arrange_manual(Node* node, int x, int y, int width, int height, std::size_t monitor_index) {
    if (!node) return;
    if (node->is_leaf()) {
      arrange_leaf(node, x, y, width, height, monitor_index);
      return;
    }
    const int count = static_cast<int>(node->children.size());
    const int gap = static_cast<int>(config_.gap);
    if (node->orientation == Orientation::Vertical) {
      const int child_width = (width - gap * (count - 1)) / count;
      for (int index = 0; index < count; ++index) {
        arrange_manual(node->children[index].get(), x + index * (child_width + gap), y, child_width,
                       height, monitor_index);
      }
    } else {
      const int child_height = (height - gap * (count - 1)) / count;
      for (int index = 0; index < count; ++index) {
        arrange_manual(node->children[index].get(), x, y + index * (child_height + gap), width,
                       child_height, monitor_index);
      }
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
      const int x = target_monitor.x + gap;
      const int y = target_monitor.y + kBarHeight + gap;
      const int width = target_monitor.width - 2 * gap;
      const int height = target_monitor.height - kBarHeight - 2 * gap;
      if (target.mode == LayoutMode::Manual) {
        arrange_manual(target.root.get(), x, y, width, height, index);
      } else {
        arrange_automatic(target.mode, x, y, width, height, index);
      }
    }
    arrange_floating(target);
    XRaiseWindow(display_, bar_);
    draw_bar();
    update_tray();
    XFlush(display_);
  }

  void spawn_terminal() const {
    const pid_t child = fork();
    if (child < 0) {
      std::cerr << "mepwm: could not start terminal: " << std::strerror(errno) << '\n';
      return;
    }
    if (child == 0) {
      setsid();
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
        if (event.xcrossing.window != bar_) focus(event.xcrossing.window);
        break;
      case ButtonPress:
        handle_button(event.xbutton);
        break;
      case Expose:
        if (event.xexpose.window == bar_ && event.xexpose.count == 0) draw_bar();
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
          update_monitors();
          XResizeWindow(display_, bar_, event.xconfigure.width, kBarHeight);
          arrange();
        }
        break;
      case KeyPress:
        handle_key(event.xkey);
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
      return;
    }
    if (state != Mod4Mask) return;
    if (key == XK_Return) spawn_terminal();
    if (key == XK_j) focus_relative(1);
    if (key == XK_k) focus_relative(-1);
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
      const int workspace_index = event.x / 42;
      if (workspace_index >= 0 && workspace_index < kWorkspaceCount) {
        switch_workspace(workspace_index);
      } else if (event.x >= 378 && event.x < 480) {
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
  GC bar_gc_ = nullptr;
  Pixmap bar_pixmap_ = None;
  int bar_pixmap_width_ = 0;
  XftDraw* bar_xft_draw_ = nullptr;
  XftFont* bar_font_ = nullptr;
  XftColor bar_foreground_{};
  XftColor bar_background_{};
  XftColor bar_selected_{};
  std::unordered_map<FcChar32, XftFont*> fallback_fonts_;
  std::vector<BarHit> task_hits_;
  Cursor cursor_ = None;
  std::array<Workspace, kWorkspaceCount> workspaces_;
  int current_workspace_ = 0;
  std::vector<Monitor> monitors_;
  std::size_t current_monitor_ = 0;
  std::unordered_set<Window> expected_unmaps_;
  std::unordered_map<Window, WindowState> window_state_;
  Window previously_focused_ = None;
  Window scratchpad_ = None;
  bool scratchpad_hidden_ = false;
  unsigned long border_normal_pixel_ = 0;
  unsigned long border_focused_pixel_ = 0;
  bool running_ = true;
  lua_State* lua_ = nullptr;
  int ipc_fd_ = -1;
  std::string ipc_path_;
  std::vector<LuaRule> lua_rules_;
  std::vector<LuaKeybind> lua_keybinds_;
  std::vector<LuaMousebind> lua_mousebinds_;

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
