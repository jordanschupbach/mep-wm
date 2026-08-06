#include "backend.hpp"

#include <X11/XKBlib.h>
#include <X11/Xlib.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <sys/types.h>
#include <unistd.h>
#include <unordered_set>
#include <utility>
#include <vector>

namespace mepwm {
namespace {

constexpr int kWorkspaceCount = 9;
constexpr int kBarHeight = 24;
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
};

int on_x_error(Display*, XErrorEvent* error) {
  if (error->error_code == BadAccess) another_window_manager = 1;
  return 0;
}

// Xlib requires an I/O error handler not to return. The nested X server may be
// closed independently of the manager, so exit quietly instead of printing an
// alarming connection-broken diagnostic in the launching terminal.
int on_x_io_error(Display*) { _exit(0); }

class X11Backend final : public Backend {
 public:
  ~X11Backend() override {
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

    create_bar();
    grab_keys();
    adopt_existing_windows();
    arrange();
    std::cerr << "mepwm: managing X display " << DisplayString(display_) << '\n';

    while (running_) {
      XEvent event;
      XNextEvent(display_, &event);
      dispatch(event);
    }
    return 0;
  }

 private:
  Workspace& workspace() { return workspaces_[current_workspace_]; }

  static std::unique_ptr<Node> make_leaf(Node* parent = nullptr) {
    auto leaf = std::make_unique<Node>();
    leaf->parent = parent;
    return leaf;
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
    XMapRaised(display_, bar_);
    draw_bar();
  }

  void draw_bar() {
    if (!bar_) return;
    const int width = DisplayWidth(display_, screen_);
    XSetForeground(display_, bar_gc_, BlackPixel(display_, screen_));
    XFillRectangle(display_, bar_, bar_gc_, 0, 0, width, kBarHeight);

    for (int index = 0; index < kWorkspaceCount; ++index) {
      const int x = index * 42;
      if (index == current_workspace_) {
        XSetForeground(display_, bar_gc_, WhitePixel(display_, screen_));
        XFillRectangle(display_, bar_, bar_gc_, x, 0, 38, kBarHeight);
        XSetForeground(display_, bar_gc_, BlackPixel(display_, screen_));
      } else {
        XSetForeground(display_, bar_gc_, WhitePixel(display_, screen_));
      }
      const std::string label = " " + std::to_string(index + 1) + " ";
      XDrawString(display_, bar_, bar_gc_, x + 8, 16, label.c_str(), static_cast<int>(label.size()));
    }

    XSetForeground(display_, bar_gc_, WhitePixel(display_, screen_));
    const char* mode = workspace().mode == LayoutMode::Manual
                           ? "manual"
                           : workspace().mode == LayoutMode::MasterStack ? "master-stack" : "monocle";
    const std::string right = "mepwm | workspace " + std::to_string(current_workspace_ + 1) +
                              " | " + mode;
    XDrawString(display_, bar_, bar_gc_, 400, 16, right.c_str(), static_cast<int>(right.size()));
    XFlush(display_);
  }

  void grab_keys() {
    const unsigned int ignored_modifiers[] = {0, LockMask, Mod2Mask, LockMask | Mod2Mask};
    const KeySym keys[] = {XK_Return, XK_q, XK_j, XK_k, XK_v, XK_s, XK_Tab, XK_space, XK_a,
                           XK_1,      XK_2, XK_3, XK_4, XK_5, XK_6, XK_7, XK_8, XK_9};
    for (const unsigned int ignored : ignored_modifiers) {
      for (const KeySym key : keys) {
        XGrabKey(display_, XKeysymToKeycode(display_, key), Mod4Mask | ignored, root_, True,
                 GrabModeAsync, GrabModeAsync);
      }
      XGrabKey(display_, XKeysymToKeycode(display_, XK_q), Mod4Mask | ShiftMask | ignored, root_,
               True, GrabModeAsync, GrabModeAsync);
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

  void manage(Window window) {
    if (find_workspace(window) >= 0) return;
    XWindowAttributes attributes;
    if (!XGetWindowAttributes(display_, window, &attributes) || attributes.override_redirect) return;

    Workspace& target = workspace();
    if (!target.root) {
      target.root = make_leaf();
      target.selected_leaf = target.root.get();
    }
    if (!target.selected_leaf || !target.selected_leaf->is_leaf()) {
      target.selected_leaf = first_leaf(target.root.get());
    }
    target.selected_leaf->tabs.push_back(window);
    target.selected_leaf->active_tab = target.selected_leaf->tabs.size() - 1;
    XSelectInput(display_, window, EnterWindowMask | FocusChangeMask | PropertyChangeMask);
    XGrabButton(display_, AnyButton, AnyModifier, window, False, ButtonPressMask, GrabModeSync,
                GrabModeAsync, None, None);
    focus(window);
    arrange();
  }

  int find_workspace(Window window) const {
    for (int index = 0; index < kWorkspaceCount; ++index) {
      if (find_leaf(workspaces_[index].root.get(), window)) return index;
    }
    return -1;
  }

  void forget_window(Workspace& target, Window window) {
    Node* leaf = find_leaf(target.root.get(), window);
    if (!leaf) return;
    const auto position = std::find(leaf->tabs.begin(), leaf->tabs.end(), window);
    leaf->tabs.erase(position);
    if (leaf->tabs.empty()) {
      remove_empty_leaf(target, leaf);
    } else {
      leaf->active_tab %= leaf->tabs.size();
      target.selected_leaf = leaf;
      target.focused = leaf->tabs[leaf->active_tab];
    }
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
      target.focused = None;
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
    target.focused = target.selected_leaf && !target.selected_leaf->tabs.empty()
                         ? target.selected_leaf->tabs[target.selected_leaf->active_tab]
                         : None;
  }

  void unmanage(Window window) {
    const int index = find_workspace(window);
    if (index < 0) return;
    XUngrabButton(display_, AnyButton, AnyModifier, window);
    const bool visible = index == current_workspace_;
    forget_window(workspaces_[index], window);
    if (visible) arrange();
  }

  void focus(Window window) {
    Workspace& target = workspace();
    Node* leaf = find_leaf(target.root.get(), window);
    if (!leaf) return;
    target.selected_leaf = leaf;
    target.focused = window;
    leaf->active_tab = static_cast<std::size_t>(std::find(leaf->tabs.begin(), leaf->tabs.end(), window) -
                                                leaf->tabs.begin());
    XSetInputFocus(display_, window, RevertToPointerRoot, CurrentTime);
    XRaiseWindow(display_, window);
  }

  void focus_relative(int offset) {
    std::vector<Window> windows;
    collect_windows(workspace().root.get(), windows);
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

  void switch_workspace(int index) {
    if (index == current_workspace_ || index < 0 || index >= kWorkspaceCount) return;
    hide_workspace(workspace());
    current_workspace_ = index;
    arrange();
    if (workspace().focused != None) focus(workspace().focused);
    draw_bar();
  }

  void hide_workspace(const Workspace& target) {
    std::vector<Window> windows;
    collect_windows(target.root.get(), windows);
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

  void arrange_leaf(Node* leaf, int x, int y, int width, int height) {
    if (leaf->tabs.empty()) return;
    leaf->active_tab %= leaf->tabs.size();
    for (std::size_t index = 0; index < leaf->tabs.size(); ++index) {
      if (index == leaf->active_tab) {
        resize(leaf->tabs[index], x, y, width, height);
      } else {
        hide_window(leaf->tabs[index]);
      }
    }
  }

  void arrange_manual(Node* node, int x, int y, int width, int height) {
    if (!node) return;
    if (node->is_leaf()) {
      arrange_leaf(node, x, y, width, height);
      return;
    }
    const int count = static_cast<int>(node->children.size());
    const int gap = static_cast<int>(config_.gap);
    if (node->orientation == Orientation::Vertical) {
      const int child_width = (width - gap * (count - 1)) / count;
      for (int index = 0; index < count; ++index) {
        arrange_manual(node->children[index].get(), x + index * (child_width + gap), y, child_width,
                       height);
      }
    } else {
      const int child_height = (height - gap * (count - 1)) / count;
      for (int index = 0; index < count; ++index) {
        arrange_manual(node->children[index].get(), x, y + index * (child_height + gap), width,
                       child_height);
      }
    }
  }

  void arrange_automatic(LayoutMode mode, int x, int y, int width, int height) {
    std::vector<Window> windows;
    collect_windows(workspace().root.get(), windows);
    if (windows.empty()) return;
    if (mode == LayoutMode::Monocle) {
      for (Window window : windows) hide_window(window);
      resize(workspace().focused == None ? windows.front() : workspace().focused, x, y, width, height);
      return;
    }
    const int gap = static_cast<int>(config_.gap);
    if (windows.size() == 1) {
      resize(windows.front(), x, y, width, height);
      return;
    }
    const int master_width = width / 2 - gap / 2;
    resize(windows.front(), x, y, master_width, height);
    const int stack_x = x + master_width + gap;
    const int stack_width = width - master_width - gap;
    const int stack_height = (height - gap * (static_cast<int>(windows.size()) - 2)) /
                             static_cast<int>(windows.size() - 1);
    for (std::size_t index = 1; index < windows.size(); ++index) {
      resize(windows[index], stack_x, y + static_cast<int>(index - 1) * (stack_height + gap),
             stack_width, stack_height);
    }
  }

  void arrange() {
    const int gap = static_cast<int>(config_.gap);
    const int width = DisplayWidth(display_, screen_);
    const int height = DisplayHeight(display_, screen_) - kBarHeight;
    Workspace& target = workspace();
    if (target.mode == LayoutMode::Manual) {
      arrange_manual(target.root.get(), gap, kBarHeight + gap, width - 2 * gap, height - 2 * gap);
    } else {
      arrange_automatic(target.mode, gap, kBarHeight + gap, width - 2 * gap, height - 2 * gap);
    }
    draw_bar();
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

  void dispatch(const XEvent& event) {
    switch (event.type) {
      case MapRequest:
        manage(event.xmaprequest.window);
        XMapWindow(display_, event.xmaprequest.window);
        arrange();
        break;
      case ConfigureRequest: {
        const XConfigureRequestEvent& request = event.xconfigurerequest;
        if (find_workspace(request.window) >= 0) {
          arrange();
          break;
        }
        XWindowChanges changes{request.x, request.y, request.width, request.height,
                               request.border_width, request.above, request.detail};
        XConfigureWindow(display_, request.window, request.value_mask, &changes);
        break;
      }
      case DestroyNotify:
        expected_unmaps_.erase(event.xdestroywindow.window);
        unmanage(event.xdestroywindow.window);
        break;
      case UnmapNotify:
        if (expected_unmaps_.erase(event.xunmap.window) == 0) unmanage(event.xunmap.window);
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
      case ConfigureNotify:
        if (event.xconfigure.window == root_) {
          XResizeWindow(display_, bar_, event.xconfigure.width, kBarHeight);
          arrange();
        }
        break;
      case KeyPress:
        handle_key(event.xkey);
        break;
      default:
        break;
    }
  }

  void handle_key(const XKeyEvent& event) {
    const KeySym key = XkbKeycodeToKeysym(display_, event.keycode, 0, 0);
    const unsigned int state = event.state & ~(LockMask | Mod2Mask);
    if (state == (Mod4Mask | ShiftMask) && key == XK_q) {
      running_ = false;
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
    if (key >= XK_1 && key <= XK_9) switch_workspace(static_cast<int>(key - XK_1));
  }

  void handle_button(const XButtonEvent& event) {
    if (event.window == bar_) {
      const int workspace_index = event.x / 42;
      if (workspace_index >= 0 && workspace_index < kWorkspaceCount) {
        switch_workspace(workspace_index);
      }
      return;
    }
    if (find_workspace(event.window) == current_workspace_) {
      focus(event.window);
      // The passive grab is synchronous only long enough to focus the window;
      // replaying it lets the application receive its original click.
      XAllowEvents(display_, ReplayPointer, CurrentTime);
    }
  }

  Config config_;
  Display* display_ = nullptr;
  int screen_ = 0;
  Window root_ = None;
  Window bar_ = None;
  GC bar_gc_ = nullptr;
  Cursor cursor_ = None;
  std::array<Workspace, kWorkspaceCount> workspaces_;
  int current_workspace_ = 0;
  std::unordered_set<Window> expected_unmaps_;
  bool running_ = true;
};

}  // namespace

std::unique_ptr<Backend> make_x11_backend() { return std::make_unique<X11Backend>(); }

}  // namespace mepwm
