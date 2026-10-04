// macOS implementation of core::Platform. mepwm cannot replace the macOS
// window server, so this backend works as an overlay (AeroSpace/Amethyst
// style): it observes other applications' windows through the Accessibility
// API and arranges them, while global Option-key hotkeys drive the shared
// core::TilingEngine.

#include "backend.hpp"

#import <AppKit/AppKit.h>
#import <Carbon/Carbon.h>
#import <CoreWLAN/CoreWLAN.h>
#import <IOBluetooth/IOBluetooth.h>
#import <IOKit/ps/IOPSKeys.h>
#import <IOKit/ps/IOPowerSources.h>
#import <QuartzCore/QuartzCore.h>

#include <mach/mach.h>
#include <sys/mount.h>
#include <sys/sysctl.h>
#include <sys/wait.h>

#include <algorithm>
#include <cctype>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

#include "core/engine.hpp"
#include "core/platform.hpp"

// Content view for chrome panels that reports left clicks (in view
// coordinates) without activating mepwm or the panel.
@interface MEPWMClickView : NSView
@property(nonatomic, copy) void (^onMouseDown)(NSPoint point);
@end

@implementation MEPWMClickView
// The chrome panels never become key (non-activating), so every click is a
// "first mouse"; without this AppKit would swallow it.
- (BOOL)acceptsFirstMouse:(NSEvent*)event {
  return YES;
}
// Keep the label subviews out of hit testing so clicks always land here.
- (NSView*)hitTest:(NSPoint)point {
  NSView* view = [super hitTest:point];
  return view == nil ? nil : self;
}
- (void)mouseDown:(NSEvent*)event {
  if (self.onMouseDown != nil) self.onMouseDown([self convertPoint:event.locationInWindow fromView:nil]);
}
@end

// Borderless panel that can take keyboard focus without activating mepwm --
// the Spotlight/Alfred technique (NSWindowStyleMaskNonactivatingPanel plus
// canBecomeKeyWindow): the frontmost app stays active while typed keys reach
// the picker.
@interface MEPWMPickerPanel : NSPanel
@end

@implementation MEPWMPickerPanel
- (BOOL)canBecomeKeyWindow {
  return YES;
}
@end

// Picker content view: forwards raw key presses to the platform.
@interface MEPWMPickerView : NSView
@property(nonatomic, copy) void (^onKeyDown)(NSEvent* event);
@end

@implementation MEPWMPickerView
- (BOOL)acceptsFirstResponder {
  return YES;
}
- (void)keyDown:(NSEvent*)event {
  if (self.onKeyDown != nil) self.onKeyDown(event);
}
@end

// Side-panel content view: reports clicks like MEPWMClickView and also
// forwards raw key presses for panels the engine marks wants_keys (todo
// navigation, the '?' help overlay).
@interface MEPWMPanelView : MEPWMClickView
@property(nonatomic, copy) void (^onKeyDown)(NSEvent* event);
@end

@implementation MEPWMPanelView
- (BOOL)acceptsFirstResponder {
  return YES;
}
- (void)keyDown:(NSEvent*)event {
  if (self.onKeyDown != nil) self.onKeyDown(event);
}
@end

namespace mepwm {
namespace {

constexpr OSType kHotkeySignature = 0x4D455057;  // 'MEPW'

std::string to_std_string(NSString* value) {
  if (value == nil) return {};
  const char* utf8 = value.UTF8String;
  return utf8 != nullptr ? std::string(utf8) : std::string();
}

std::string copy_string_attribute(AXUIElementRef element, CFStringRef attribute) {
  CFTypeRef value = nullptr;
  if (AXUIElementCopyAttributeValue(element, attribute, &value) != kAXErrorSuccess || value == nullptr) {
    return {};
  }
  std::string result;
  if (CFGetTypeID(value) == CFStringGetTypeID()) {
    result = to_std_string((__bridge NSString*)value);
  }
  CFRelease(value);
  return result;
}

bool boolean_attribute(AXUIElementRef element, CFStringRef attribute) {
  CFTypeRef value = nullptr;
  if (AXUIElementCopyAttributeValue(element, attribute, &value) != kAXErrorSuccess || value == nullptr) {
    return false;
  }
  const bool result = CFGetTypeID(value) == CFBooleanGetTypeID() && CFBooleanGetValue((CFBooleanRef)value);
  CFRelease(value);
  return result;
}

core::Rect frame_attribute(AXUIElementRef window) {
  core::Rect frame;
  CFTypeRef value = nullptr;
  if (AXUIElementCopyAttributeValue(window, kAXPositionAttribute, &value) == kAXErrorSuccess &&
      value != nullptr) {
    CGPoint point = CGPointZero;
    AXValueGetValue((AXValueRef)value, kAXValueTypeCGPoint, &point);
    frame.x = static_cast<int>(point.x);
    frame.y = static_cast<int>(point.y);
    CFRelease(value);
  }
  value = nullptr;
  if (AXUIElementCopyAttributeValue(window, kAXSizeAttribute, &value) == kAXErrorSuccess &&
      value != nullptr) {
    CGSize size = CGSizeZero;
    AXValueGetValue((AXValueRef)value, kAXValueTypeCGSize, &size);
    frame.width = static_cast<int>(size.width);
    frame.height = static_cast<int>(size.height);
    CFRelease(value);
  }
  return frame;
}

// A window the engine should tile: a standard, non-minimized document
// window, as opposed to panels, sheets, and popovers.
bool is_manageable(AXUIElementRef window) {
  if (copy_string_attribute(window, kAXSubroleAttribute) !=
      to_std_string((__bridge NSString*)kAXStandardWindowSubrole)) {
    return false;
  }
  return !boolean_attribute(window, kAXMinimizedAttribute);
}

NSColor* color_from_hex(const std::string& spec) {
  unsigned int red = 68;
  unsigned int green = 68;
  unsigned int blue = 68;
  if (spec.size() == 7 && spec[0] == '#') {
    std::sscanf(spec.c_str() + 1, "%02x%02x%02x", &red, &green, &blue);
  }
  return [NSColor colorWithSRGBRed:red / 255.0 green:green / 255.0 blue:blue / 255.0 alpha:1.0];
}

CGFloat primary_screen_height() {
  NSScreen* primary = NSScreen.screens.firstObject;
  return primary != nil ? primary.frame.size.height : 0;
}

// Blocks until the process is trusted for Accessibility. Never fails hard:
// launched as an app (Spotlight, LaunchAgent) there is no terminal to read
// an error from, and macOS applies the grant to the running process, so
// mepwm comes alive the moment it lands -- no relaunch needed. Quitting
// from the dialog exits 0 so a LaunchAgent does not treat it as a crash
// and restart-loop the alert.
void wait_for_accessibility_permission() {
  NSDictionary* options = @{(__bridge NSString*)kAXTrustedCheckOptionPrompt : @YES};
  if (AXIsProcessTrustedWithOptions((__bridge CFDictionaryRef)options)) return;
  if (isatty(STDERR_FILENO) != 0) {
    std::fprintf(stderr,
                 "mepwm: waiting for the Accessibility permission (System Settings > Privacy & "
                 "Security > Accessibility).\n"
                 "If mepwm is already listed and checked, that entry belongs to a previous "
                 "build: toggle it off and back on.\n");
    while (!AXIsProcessTrusted()) [NSThread sleepForTimeInterval:1.0];
    return;
  }
  // App launch: a modal alert keeps the process servicing events (a bare
  // sleep loop would leave the app "not responding" to Finder) and explains
  // the stale-entry case, where the permission shows as already checked for
  // a previous build's binary and the system prompt never appears.
  while (!AXIsProcessTrusted()) {
    NSAlert* alert = [[NSAlert alloc] init];
    alert.messageText = @"MEP-wm needs the Accessibility permission";
    alert.informativeText =
        @"Enable MEP-wm under System Settings > Privacy & Security > Accessibility, then choose "
        @"Continue.\n\nIf MEP-wm is already listed and checked, that entry belongs to a previous "
        @"build: toggle it off and back on (or remove it with the minus button).";
    [alert addButtonWithTitle:@"Continue"];
    [alert addButtonWithTitle:@"Open System Settings"];
    [alert addButtonWithTitle:@"Quit"];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    // activateIgnoringOtherApps: is soft-deprecated in favor of -activate,
    // which older SDKs lack; keep the portable spelling.
    [NSApp activateIgnoringOtherApps:YES];
#pragma clang diagnostic pop
    // Auto-dismiss the moment the grant lands, so the user never has to
    // click Continue at the right time.
    NSTimer* poll = [NSTimer timerWithTimeInterval:1.0
                                           repeats:YES
                                             block:^(NSTimer*) {
                                               if (AXIsProcessTrusted()) {
                                                 [NSApp stopModalWithCode:NSAlertFirstButtonReturn];
                                               }
                                             }];
    [[NSRunLoop currentRunLoop] addTimer:poll forMode:NSModalPanelRunLoopMode];
    const NSModalResponse response = [alert runModal];
    [poll invalidate];
    if (response == NSAlertSecondButtonReturn) {
      [NSWorkspace.sharedWorkspace
          openURL:[NSURL URLWithString:@"x-apple.systempreferences:com.apple.preference.security"
                                       @"?Privacy_Accessibility"]];
    } else if (response == NSAlertThirdButtonReturn) {
      std::exit(0);
    }
  }
}

// A click-through overlay window whose layer draws one border ring. Stands
// in for the X11 backend's window borders; lives in the gap around each
// tiled window.
NSWindow* make_border_window() {
  NSWindow* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 16, 16)
                                                 styleMask:NSWindowStyleMaskBorderless
                                                   backing:NSBackingStoreBuffered
                                                     defer:NO];
  window.opaque = NO;
  window.backgroundColor = NSColor.clearColor;
  window.hasShadow = NO;
  window.ignoresMouseEvents = YES;
  window.level = NSFloatingWindowLevel;
  window.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces |
                              NSWindowCollectionBehaviorStationary |
                              NSWindowCollectionBehaviorIgnoresCycle |
                              NSWindowCollectionBehaviorFullScreenAuxiliary;
  window.releasedWhenClosed = NO;
  window.contentView.wantsLayer = YES;
  window.contentView.layer.backgroundColor = NSColor.clearColor.CGColor;
  return window;
}

// Opaque screen-edge panel (top bar, docks, bottom bar, sidebars). A
// non-activating NSPanel with a click-reporting content view: clicks reach
// the chrome without stealing focus from the frontmost app.
NSPanel* make_chrome_window() {
  NSPanel* window = [[NSPanel alloc]
      initWithContentRect:NSMakeRect(0, 0, 16, 16)
                styleMask:NSWindowStyleMaskBorderless | NSWindowStyleMaskNonactivatingPanel
                  backing:NSBackingStoreBuffered
                    defer:NO];
  window.opaque = NO;
  window.backgroundColor = NSColor.clearColor;
  window.hasShadow = NO;
  window.level = NSFloatingWindowLevel;
  window.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces |
                              NSWindowCollectionBehaviorStationary |
                              NSWindowCollectionBehaviorIgnoresCycle |
                              NSWindowCollectionBehaviorFullScreenAuxiliary;
  window.releasedWhenClosed = NO;
  window.contentView = [[MEPWMClickView alloc] initWithFrame:NSMakeRect(0, 0, 16, 16)];
  window.contentView.wantsLayer = YES;
  return window;
}

// The side panel is the one chrome window that can take keyboard focus
// (todo navigation, the '?' help overlay): an MEPWMPickerPanel
// (canBecomeKeyWindow) configured like make_chrome_window() but with a
// click+key content view. Non-activating, so the frontmost app keeps its
// focused look while typed keys reach the panel -- the picker's technique.
NSPanel* make_side_panel_window() {
  NSPanel* window = [[MEPWMPickerPanel alloc]
      initWithContentRect:NSMakeRect(0, 0, 16, 16)
                styleMask:NSWindowStyleMaskBorderless | NSWindowStyleMaskNonactivatingPanel
                  backing:NSBackingStoreBuffered
                    defer:NO];
  window.opaque = NO;
  window.backgroundColor = NSColor.clearColor;
  window.hasShadow = NO;
  window.level = NSFloatingWindowLevel;
  window.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces |
                              NSWindowCollectionBehaviorStationary |
                              NSWindowCollectionBehaviorIgnoresCycle |
                              NSWindowCollectionBehaviorFullScreenAuxiliary;
  window.releasedWhenClosed = NO;
  window.contentView = [[MEPWMPanelView alloc] initWithFrame:NSMakeRect(0, 0, 16, 16)];
  window.contentView.wantsLayer = YES;
  return window;
}

// One-pixel horizontal separator (under the panel title, above the footer).
NSView* make_panel_rule(NSWindow* window) {
  NSView* rule = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 16, 1)];
  rule.wantsLayer = YES;
  [window.contentView addSubview:rule];
  return rule;
}

NSTextField* make_chrome_label(NSWindow* window, NSTextAlignment alignment, NSFont* font) {
  NSTextField* label = [NSTextField labelWithString:@""];
  label.font = font;
  label.alignment = alignment;
  label.lineBreakMode = NSLineBreakByTruncatingTail;
  label.wantsLayer = YES;
  [window.contentView addSubview:label];
  return label;
}

// Picks an installed Nerd Font family for the chrome so widget icon glyphs
// render. MEPWM_ICON_FONT overrides ("none" disables icons entirely).
std::string detect_icon_font() {
  if (const char* env = std::getenv("MEPWM_ICON_FONT")) {
    return std::string(env) == "none" ? std::string() : std::string(env);
  }
  NSString* best = nil;
  for (NSString* family in NSFontManager.sharedFontManager.availableFontFamilies) {
    if ([family rangeOfString:@"Nerd Font" options:NSCaseInsensitiveSearch].location ==
        NSNotFound) {
      continue;
    }
    // Prefer the base variant over "... Mono" / "... Propo" siblings.
    if (best == nil || family.length < best.length) best = family;
  }
  return best != nil ? to_std_string(best) : std::string();
}

// Footer strip at the bottom of the side panel ("?  Toggle help"). The
// engine mirrors this in its kPanelChromeHeight when windowing todo rows.
constexpr CGFloat kSidePanelFooterHeight = 26;

// Application picker (Mod+p): geometry and the scanned .app inventory.
constexpr CGFloat kPickerWidth = 560;
constexpr CGFloat kPickerRowHeight = 26;
constexpr CGFloat kPickerQueryHeight = 40;
constexpr std::size_t kPickerMaxRows = 10;

// Both picker flavors share core::PickerItem rows; for applications the id
// is the absolute bundle path and the label the bundle name without ".app".
void scan_app_directory(const std::filesystem::path& directory, int depth,
                        std::vector<core::PickerItem>& apps) {
  std::error_code error;
  for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
    const std::filesystem::path& path = entry.path();
    if (path.extension() == ".app") {
      apps.push_back({path.string(), path.stem().string(), {}});
    } else if (depth > 0 && entry.is_directory(error)) {
      // One level of subfolders covers /Applications/Utilities and the
      // folders installers create, without walking inside bundles.
      scan_app_directory(path, depth - 1, apps);
    }
  }
}

// Deliberately a filesystem scan, not Spotlight/NSMetadataQuery: it works
// with indexing disabled and has no query latency.
std::vector<core::PickerItem> scan_applications() {
  std::vector<core::PickerItem> apps;
  std::vector<std::string> roots = {"/Applications", "/System/Applications",
                                    "/System/Library/CoreServices/Applications"};
  if (const char* home = std::getenv("HOME")) roots.push_back(std::string(home) + "/Applications");
  for (const std::string& root : roots) scan_app_directory(root, 1, apps);
  std::sort(apps.begin(), apps.end(), [](const core::PickerItem& left, const core::PickerItem& right) {
    return left.label < right.label;
  });
  return apps;
}

// Case-insensitive subsequence match, like the X11 launcher's: -1 when the
// query is not a subsequence of the candidate, otherwise a score favoring
// word-start and consecutive hits. An empty query matches everything.
int fuzzy_score(const std::string& query, const std::string& candidate) {
  if (query.empty()) return 0;
  int score = 0;
  std::size_t next = 0;
  for (const char query_char : query) {
    const int lowered = std::tolower(static_cast<unsigned char>(query_char));
    bool matched = false;
    for (std::size_t index = next; index < candidate.size(); ++index) {
      if (std::tolower(static_cast<unsigned char>(candidate[index])) != lowered) continue;
      score += 1;
      if (index == 0 || candidate[index - 1] == ' ') score += 8;
      if (index == next && next != 0) score += 4;
      next = index + 1;
      matched = true;
      break;
    }
    if (!matched) return -1;
  }
  return score;
}

void place_label(NSTextField* label, NSRect frame, const std::string& text, NSColor* text_color) {
  label.frame = frame;
  NSString* value = [NSString stringWithUTF8String:text.c_str()];
  label.stringValue = value != nil ? value : @"";
  label.textColor = text_color;
}

// Up to `limit` trimmed lines of a shell command's output.
std::vector<std::string> capture_command_lines(const std::string& command, std::size_t limit) {
  std::vector<std::string> lines;
  FILE* pipe = popen(command.c_str(), "r");
  if (pipe == nullptr) return lines;
  char buffer[256];
  while (lines.size() < limit && std::fgets(buffer, sizeof buffer, pipe) != nullptr) {
    std::string line = buffer;
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
    lines.push_back(std::move(line));
  }
  pclose(pipe);
  return lines;
}

// First line of a shell command's output, trimmed; empty on failure.
std::string capture_command(const std::string& command) {
  FILE* pipe = popen(command.c_str(), "r");
  if (pipe == nullptr) return {};
  char buffer[256] = {0};
  std::string result;
  if (std::fgets(buffer, sizeof buffer, pipe) != nullptr) result = buffer;
  pclose(pipe);
  while (!result.empty() && (result.back() == '\n' || result.back() == '\r' || result.back() == ' ')) {
    result.pop_back();
  }
  return result;
}

void read_battery(core::SystemStatus& status) {
  CFTypeRef info = IOPSCopyPowerSourcesInfo();
  if (info == nullptr) return;
  CFArrayRef sources = IOPSCopyPowerSourcesList(info);
  if (sources != nullptr) {
    for (CFIndex index = 0; index < CFArrayGetCount(sources); ++index) {
      CFDictionaryRef description =
          IOPSGetPowerSourceDescription(info, CFArrayGetValueAtIndex(sources, index));
      if (description == nullptr) continue;
      auto* current = (CFNumberRef)CFDictionaryGetValue(description, CFSTR(kIOPSCurrentCapacityKey));
      auto* max = (CFNumberRef)CFDictionaryGetValue(description, CFSTR(kIOPSMaxCapacityKey));
      auto* charging = (CFBooleanRef)CFDictionaryGetValue(description, CFSTR(kIOPSIsChargingKey));
      int current_value = 0;
      int max_value = 0;
      if (current != nullptr && max != nullptr &&
          CFNumberGetValue(current, kCFNumberIntType, &current_value) &&
          CFNumberGetValue(max, kCFNumberIntType, &max_value) && max_value > 0) {
        status.battery_percent = current_value * 100 / max_value;
        status.battery_charging = charging != nullptr && CFBooleanGetValue(charging);
        const CFStringRef minutes_key = status.battery_charging
                                            ? CFSTR(kIOPSTimeToFullChargeKey)
                                            : CFSTR(kIOPSTimeToEmptyKey);
        auto* minutes = (CFNumberRef)CFDictionaryGetValue(description, minutes_key);
        int minutes_value = -1;
        if (minutes != nullptr && CFNumberGetValue(minutes, kCFNumberIntType, &minutes_value)) {
          status.battery_minutes_remaining = minutes_value;
        }
        break;
      }
    }
    CFRelease(sources);
  }
  CFRelease(info);
}

void read_memory(core::SystemStatus& status) {
  std::uint64_t total = 0;
  std::size_t size = sizeof total;
  if (sysctlbyname("hw.memsize", &total, &size, nullptr, 0) != 0 || total == 0) return;
  vm_statistics64_data_t stats;
  mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
  if (host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t)&stats, &count) !=
      KERN_SUCCESS) {
    return;
  }
  const auto page = static_cast<std::uint64_t>(getpagesize());
  const std::uint64_t used =
      (static_cast<std::uint64_t>(stats.active_count) + stats.wire_count + stats.compressor_page_count) * page;
  status.memory_used_percent = static_cast<int>(used * 100 / total);
}

// CPU usage percent from host_statistics tick deltas -- the macOS analogue
// of the X11 backend's /proc/stat sampling. The first call only records the
// baseline, so the field keeps its -1 sentinel until the second sample.
void read_cpu(core::SystemStatus& status) {
  host_cpu_load_info_data_t info;
  mach_msg_type_number_t count = HOST_CPU_LOAD_INFO_COUNT;
  if (host_statistics(mach_host_self(), HOST_CPU_LOAD_INFO, (host_info_t)&info, &count) !=
      KERN_SUCCESS) {
    return;
  }
  static std::uint64_t previous_total = 0, previous_idle = 0;
  std::uint64_t total = 0;
  for (const natural_t ticks : info.cpu_ticks) total += ticks;
  const std::uint64_t idle = info.cpu_ticks[CPU_STATE_IDLE];
  if (previous_total > 0 && total > previous_total) {
    status.cpu_used_percent = static_cast<int>(
        100 * ((total - previous_total) - (idle - previous_idle)) / (total - previous_total));
  }
  previous_total = total;
  previous_idle = idle;
}

void read_disk(core::SystemStatus& status) {
  struct statfs stats;
  if (statfs("/", &stats) != 0 || stats.f_blocks == 0) return;
  // df semantics: used / (used + available). On APFS, f_blocks spans the
  // whole container, so 1 - bavail/blocks would wildly overstate usage.
  const std::uint64_t used = stats.f_blocks - stats.f_bfree;
  const std::uint64_t reachable = used + stats.f_bavail;
  if (reachable == 0) return;
  status.disk_used_percent = static_cast<int>(used * 100 / reachable);
}

UInt32 carbon_key_code(core::Key key) {
  switch (key) {
    case core::Key::B: return kVK_ANSI_B;
    case core::Key::D: return kVK_ANSI_D;
    case core::Key::F: return kVK_ANSI_F;
    case core::Key::H: return kVK_ANSI_H;
    case core::Key::I: return kVK_ANSI_I;
    case core::Key::J: return kVK_ANSI_J;
    case core::Key::K: return kVK_ANSI_K;
    case core::Key::L: return kVK_ANSI_L;
    case core::Key::M: return kVK_ANSI_M;
    case core::Key::N: return kVK_ANSI_N;
    case core::Key::O: return kVK_ANSI_O;
    case core::Key::P: return kVK_ANSI_P;
    case core::Key::R: return kVK_ANSI_R;
    case core::Key::S: return kVK_ANSI_S;
    case core::Key::T: return kVK_ANSI_T;
    case core::Key::U: return kVK_ANSI_U;
    case core::Key::V: return kVK_ANSI_V;
    case core::Key::W: return kVK_ANSI_W;
    case core::Key::Return: return kVK_Return;
    case core::Key::Tab: return kVK_Tab;
    case core::Key::Q: return kVK_ANSI_Q;
    case core::Key::Minus: return kVK_ANSI_Minus;
    case core::Key::Equal: return kVK_ANSI_Equal;
    case core::Key::Slash: return kVK_ANSI_Slash;
    case core::Key::N1: return kVK_ANSI_1;
    case core::Key::N2: return kVK_ANSI_2;
    case core::Key::N3: return kVK_ANSI_3;
    case core::Key::N4: return kVK_ANSI_4;
    case core::Key::N5: return kVK_ANSI_5;
    case core::Key::N6: return kVK_ANSI_6;
    case core::Key::N7: return kVK_ANSI_7;
    case core::Key::N8: return kVK_ANSI_8;
    case core::Key::N9: return kVK_ANSI_9;
  }
  return kVK_ANSI_H;
}

class MacosPlatform final : public core::Platform {
 public:
  MacosPlatform() = default;
  ~MacosPlatform() override {
    for (NSWindow* window : border_windows_) [window close];
    for (NSWindow* window : tab_bar_windows_) [window close];
    for (NSWindow* window : {top_bar_, bottom_bar_, left_dock_, right_dock_, side_panel_window_,
                             (NSWindow*)picker_panel_, (NSWindow*)hint_panel_}) {
      if (window != nil) [window close];
    }
    for (const HotkeyEntry& entry : hotkeys_) {
      if (entry.reference != nullptr) UnregisterEventHotKey(entry.reference);
    }
    if (hotkey_handler_ != nullptr) RemoveEventHandler(hotkey_handler_);
    if (event_tap_source_ != nullptr) {
      CFRunLoopRemoveSource(CFRunLoopGetMain(), event_tap_source_, kCFRunLoopCommonModes);
      CFRelease(event_tap_source_);
    }
    if (event_tap_ != nullptr) {
      CGEventTapEnable(event_tap_, false);
      CFRelease(event_tap_);
    }
    for (const AxWindow& window : registry_) CFRelease(window.element);
  }

  bool initialize(core::Modifier primary, std::string* error) override {
    switch (primary) {
      case core::Modifier::Cmd:
      case core::Modifier::Super:  // Command is the Mac's Super key
        primary_modifier_ = cmdKey;
        break;
      case core::Modifier::Alt:
        primary_modifier_ = optionKey;
        break;
      case core::Modifier::Ctrl:
        primary_modifier_ = controlKey;
        break;
      case core::Modifier::Fn:
        // The Globe/fn key has no Carbon hotkey mask; RegisterEventHotKey
        // rejects it, so fn hotkeys are matched via a CGEventTap instead.
        use_fn_tap_ = true;
        break;
      case core::Modifier::FnCmd:
        use_fn_tap_ = true;
        fn_requires_cmd_ = true;
        break;
    }
    (void)error;  // no fatal initialization paths remain on macOS
    // Set up NSApplication before the permission wait (not in run()) so the
    // wait can show UI, and so border overlay windows can be created as soon
    // as the engine's first retile happens.
    [NSApplication sharedApplication];
    // Accessory: no Dock icon, no menu bar takeover -- mepwm is an overlay.
    [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
    wait_for_accessibility_permission();
    // SIGCHLD must keep its default disposition: with SIGCHLD ignored, POSIX
    // makes pclose()'s wait4 block until ALL children exit, so one long-lived
    // spawned program (a directly exec'd terminal) wedges every
    // capture_command() call -- and with it the poll loop and event tap.
    // spawn() double-forks instead, so no child outlives its exec anyway.

    icon_font_name_ = detect_icon_font();
    if (!icon_font_name_.empty()) {
      NSString* family = [NSString stringWithUTF8String:icon_font_name_.c_str()];
      chrome_font_ = [NSFontManager.sharedFontManager fontWithFamily:family
                                                              traits:0
                                                              weight:5
                                                                size:11.0];
      chrome_title_font_ = [NSFontManager.sharedFontManager fontWithFamily:family
                                                                    traits:NSBoldFontMask
                                                                    weight:9
                                                                      size:13.0];
      if (chrome_font_ == nil) icon_font_name_.clear();
    }
    if (chrome_font_ == nil) {
      chrome_font_ = [NSFont monospacedSystemFontOfSize:11.0 weight:NSFontWeightMedium];
    }
    if (chrome_title_font_ == nil) {
      chrome_title_font_ = [NSFont systemFontOfSize:13.0 weight:NSFontWeightBold];
    }
    if (debug_) {
      std::fprintf(stderr, "mepwm: icon font: %s\n",
                   icon_font_name_.empty() ? "(none, text fallback)" : icon_font_name_.c_str());
    }
    return true;
  }

  bool has_icon_font() override { return !icon_font_name_.empty(); }

  core::Rect work_area() override {
    NSArray<NSScreen*>* screens = NSScreen.screens;
    if (screens.count == 0) return {0, 0, 1280, 800};
    NSScreen* primary = screens.firstObject;
    const NSRect visible = primary.visibleFrame;
    // AppKit uses a bottom-left origin; the Accessibility API (and therefore
    // core::Rect here) uses top-left origin global coordinates.
    const CGFloat primary_height = primary.frame.size.height;
    core::Rect area;
    area.x = static_cast<int>(visible.origin.x);
    area.y = static_cast<int>(primary_height - (visible.origin.y + visible.size.height));
    area.width = static_cast<int>(visible.size.width);
    area.height = static_cast<int>(visible.size.height);
    return area;
  }

  std::vector<core::WindowInfo> list_windows() override {
    std::vector<core::WindowInfo> windows;
    @autoreleasepool {
      for (NSRunningApplication* app in NSWorkspace.sharedWorkspace.runningApplications) {
        if (app.activationPolicy != NSApplicationActivationPolicyRegular) continue;
        if (app.hidden || app.terminated) continue;
        if (app.processIdentifier == getpid()) continue;
        AXUIElementRef ax_app = AXUIElementCreateApplication(app.processIdentifier);
        CFArrayRef ax_windows = nullptr;
        if (AXUIElementCopyAttributeValue(ax_app, kAXWindowsAttribute, (CFTypeRef*)&ax_windows) ==
                kAXErrorSuccess &&
            ax_windows != nullptr) {
          for (CFIndex index = 0; index < CFArrayGetCount(ax_windows); ++index) {
            auto window = (AXUIElementRef)CFArrayGetValueAtIndex(ax_windows, index);
            if (!is_manageable(window)) continue;
            core::WindowInfo info;
            info.id = intern(window, app.processIdentifier);
            info.application = to_std_string(app.localizedName);
            info.title = copy_string_attribute(window, kAXTitleAttribute);
            info.frame = frame_attribute(window);
            windows.push_back(std::move(info));
          }
          CFRelease(ax_windows);
        }
        CFRelease(ax_app);
      }
    }
    return windows;
  }

  core::WindowId focused_window() override {
    @autoreleasepool {
      NSRunningApplication* app = NSWorkspace.sharedWorkspace.frontmostApplication;
      if (app == nil) return core::kNoWindow;
      AXUIElementRef ax_app = AXUIElementCreateApplication(app.processIdentifier);
      CFTypeRef value = nullptr;
      core::WindowId result = core::kNoWindow;
      if (AXUIElementCopyAttributeValue(ax_app, kAXFocusedWindowAttribute, &value) ==
              kAXErrorSuccess &&
          value != nullptr) {
        result = intern((AXUIElementRef)value, app.processIdentifier);
        CFRelease(value);
      }
      CFRelease(ax_app);
      return result;
    }
  }

  void set_window_frame(core::WindowId window_id, const core::Rect& frame) override {
    AXUIElementRef window = element(window_id);
    if (window == nullptr) return;
    // Position, size, then position again: apps that clamp their size while
    // off-screen would otherwise end up displaced after the resize.
    set_position(window, frame);
    CGSize size = CGSizeMake(frame.width, frame.height);
    AXValueRef size_value = AXValueCreate(kAXValueTypeCGSize, &size);
    AXUIElementSetAttributeValue(window, kAXSizeAttribute, size_value);
    CFRelease(size_value);
    set_position(window, frame);
  }

  void focus_window(core::WindowId window_id) override {
    AXUIElementRef window = element(window_id);
    if (window == nullptr) return;
    @autoreleasepool {
      NSRunningApplication* app = [NSRunningApplication
          runningApplicationWithProcessIdentifier:registry_[window_id - 1].owner];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
      // activateWithOptions: is soft-deprecated in favor of -activate, which
      // older SDKs lack; keep the portable spelling.
      [app activateWithOptions:0];
#pragma clang diagnostic pop
    }
    AXUIElementSetAttributeValue(window, kAXMainAttribute, kCFBooleanTrue);
    AXUIElementPerformAction(window, kAXRaiseAction);
  }

  void raise_window(core::WindowId window_id) override {
    AXUIElementRef window = element(window_id);
    if (window == nullptr) return;
    // Raise without activating the owning app: within-app z-order only,
    // which is enough to keep a pane's active tab on top of its siblings.
    AXUIElementPerformAction(window, kAXRaiseAction);
  }

  bool register_hotkey(const core::Hotkey& hotkey, core::Action action, int argument) override {
    if (use_fn_tap_) {
      if (!ensure_event_tap()) return false;
      HotkeyEntry entry;
      entry.action = action;
      entry.argument = argument;
      entry.key_code = carbon_key_code(hotkey.key);
      entry.shift = (hotkey.modifiers & core::kModShift) != 0U;
      entry.ctrl = (hotkey.modifiers & core::kModCtrl) != 0U;
      hotkeys_.push_back(entry);
      return true;
    }
    if (!ensure_hotkey_handler()) return false;
    UInt32 modifiers = 0;
    if ((hotkey.modifiers & core::kModPrimary) != 0U) modifiers |= primary_modifier_;
    if ((hotkey.modifiers & core::kModShift) != 0U) modifiers |= shiftKey;
    if ((hotkey.modifiers & core::kModCtrl) != 0U) modifiers |= controlKey;
    EventHotKeyID identifier;
    identifier.signature = kHotkeySignature;
    identifier.id = static_cast<UInt32>(hotkeys_.size());
    EventHotKeyRef reference = nullptr;
    // Dispatcher target, not application target: for accessory/background
    // apps the application target can silently drop hotkey events.
    const OSStatus status = RegisterEventHotKey(carbon_key_code(hotkey.key), modifiers, identifier,
                                                GetEventDispatcherTarget(), 0, &reference);
    if (debug_) {
      std::fprintf(stderr, "mepwm: register hotkey id=%u status=%d\n",
                   static_cast<unsigned int>(identifier.id), static_cast<int>(status));
    }
    if (status != noErr || reference == nullptr) return false;
    hotkeys_.push_back({reference, action, argument});
    return true;
  }

  void update_borders(const std::vector<core::Border>& borders) override {
    @autoreleasepool {
      while (border_windows_.size() > borders.size()) {
        [border_windows_.back() close];
        border_windows_.pop_back();
      }
      while (border_windows_.size() < borders.size()) {
        border_windows_.push_back(make_border_window());
      }
      const CGFloat screen_height = primary_screen_height();
      for (std::size_t index = 0; index < borders.size(); ++index) {
        const core::Border& border = borders[index];
        const int width = static_cast<int>(border.width);
        // The ring sits just outside the managed window's frame, in the gap.
        const core::Rect ring{border.frame.x - width, border.frame.y - width,
                              border.frame.width + 2 * width, border.frame.height + 2 * width};
        const NSRect cocoa_frame = NSMakeRect(
            ring.x, screen_height - (ring.y + ring.height), ring.width, ring.height);
        NSWindow* window = border_windows_[index];
        [window setFrame:cocoa_frame display:NO];
        CALayer* layer = window.contentView.layer;
        layer.borderWidth = border.width;
        layer.cornerRadius = 0.0;
        layer.borderColor = color_from_hex(border.color).CGColor;
        [window orderFrontRegardless];
      }
    }
  }

  void update_chrome(const core::Chrome& chrome) override {
    @autoreleasepool {
      if (top_bar_ == nil) {
        top_bar_ = make_chrome_window();
        bottom_bar_ = make_chrome_window();
        left_dock_ = make_chrome_window();
        right_dock_ = make_chrome_window();
        top_left_label_ = make_chrome_label(top_bar_, NSTextAlignmentLeft, chrome_font_);
        top_center_label_ = make_chrome_label(top_bar_, NSTextAlignmentCenter, chrome_font_);
        top_right_label_ = make_chrome_label(top_bar_, NSTextAlignmentRight, chrome_font_);
        bottom_label_ = make_chrome_label(bottom_bar_, NSTextAlignmentCenter, chrome_font_);
        MacosPlatform* platform = this;
        ((MEPWMClickView*)bottom_bar_.contentView).onMouseDown = ^(NSPoint point) {
          platform->chrome_clicked(core::ChromeArea::BottomBar, point);
        };
        ((MEPWMClickView*)left_dock_.contentView).onMouseDown = ^(NSPoint point) {
          platform->chrome_clicked(core::ChromeArea::LeftDock, point);
        };
        ((MEPWMClickView*)top_bar_.contentView).onMouseDown = ^(NSPoint point) {
          platform->chrome_clicked(core::ChromeArea::TopBar, point);
        };
        ((MEPWMClickView*)right_dock_.contentView).onMouseDown = ^(NSPoint point) {
          platform->chrome_clicked(core::ChromeArea::RightDock, point);
        };
      }
      last_chrome_ = chrome;
      // mod+b hid the border chrome: order the edge panels out and stop.
      // (Hints skip ordered-out windows, so hidden cells get no chips.)
      if (!chrome.visible) {
        for (NSWindow* window : {top_bar_, bottom_bar_, left_dock_, right_dock_}) {
          [window orderOut:nil];
        }
        return;
      }
      NSColor* background = color_from_hex(chrome.background_color);
      NSColor* foreground = color_from_hex(chrome.text_color);
      NSColor* accent = color_from_hex(chrome.accent_color);
      const CGFloat screen_height = primary_screen_height();

      const auto place_panel = [&](NSWindow* window, const core::Rect& frame) {
        const NSRect cocoa = NSMakeRect(frame.x, screen_height - (frame.y + frame.height),
                                        frame.width, frame.height);
        [window setFrame:cocoa display:NO];
        window.contentView.layer.backgroundColor =
            [background colorWithAlphaComponent:0.94].CGColor;
        [window orderFrontRegardless];
      };
      place_panel(top_bar_, chrome.top);
      place_panel(bottom_bar_, chrome.bottom);
      place_panel(left_dock_, chrome.left);
      place_panel(right_dock_, chrome.right);

      // Top bar: "mepwm", workspace cells, centered title, clock.
      const CGFloat bar_width = chrome.top.width;
      const CGFloat label_y = (chrome.top.height - 16.0) / 2.0;
      place_label(top_left_label_, NSMakeRect(10, label_y, 52, 16), chrome.top_left, accent);
      while (top_cell_labels_.size() > chrome.top_cells.size()) {
        [top_cell_labels_.back() removeFromSuperview];
        top_cell_labels_.pop_back();
      }
      while (top_cell_labels_.size() < chrome.top_cells.size()) {
        top_cell_labels_.push_back(make_chrome_label(top_bar_, NSTextAlignmentCenter, chrome_font_));
      }
      // Cells size to their text (workspace numbers stay narrow, task-list
      // titles get what they need); whatever would run under the clock is
      // hidden rather than clipped mid-bar.
      constexpr CGFloat kCellMinWidth = 22;
      const CGFloat cells_end = bar_width - 180;
      CGFloat cell_x = 70;
      for (std::size_t index = 0; index < chrome.top_cells.size(); ++index) {
        const core::BarCell& cell = chrome.top_cells[index];
        NSTextField* label = top_cell_labels_[index];
        place_label(label, NSMakeRect(0, label_y - 2, 10, 20), cell.text,
                    cell.highlight ? NSColor.whiteColor : foreground);
        [label sizeToFit];
        const CGFloat width = std::max(kCellMinWidth, label.frame.size.width + 8);
        if (cell_x + width > cells_end) {
          label.frame = NSZeroRect;  // keeps hit-testing from matching it
          continue;
        }
        label.frame = NSMakeRect(cell_x, label_y - 2, width, 20);
        label.alignment = NSTextAlignmentCenter;
        label.layer.backgroundColor =
            cell.highlight ? accent.CGColor : NSColor.clearColor.CGColor;
        cell_x += width + 4;
      }
      const CGFloat center_start = std::max<CGFloat>(cell_x + 12, 180);
      place_label(top_center_label_,
                  NSMakeRect(center_start, label_y, std::max<CGFloat>(bar_width - center_start - 180, 50), 16),
                  chrome.top_center, foreground);
      place_label(top_right_label_, NSMakeRect(bar_width - 170, label_y, 160, 16),
                  chrome.top_right, foreground);
      // Bottom bar: system widgets right-aligned, help text in the rest.
      while (bottom_widget_labels_.size() > chrome.bottom_widgets.size()) {
        [bottom_widget_labels_.back() removeFromSuperview];
        bottom_widget_labels_.pop_back();
      }
      while (bottom_widget_labels_.size() < chrome.bottom_widgets.size()) {
        bottom_widget_labels_.push_back(make_chrome_label(bottom_bar_, NSTextAlignmentCenter, chrome_font_));
      }
      const CGFloat widget_y = (chrome.bottom.height - 16.0) / 2.0;
      CGFloat right_edge = chrome.bottom.width - 10;
      for (std::size_t index = chrome.bottom_widgets.size(); index-- > 0;) {
        const core::BarCell& cell = chrome.bottom_widgets[index];
        NSTextField* label = bottom_widget_labels_[index];
        place_label(label, NSMakeRect(0, widget_y, 10, 16), cell.text,
                    cell.highlight ? NSColor.whiteColor : foreground);
        [label sizeToFit];
        const CGFloat width = label.frame.size.width + 8;
        label.frame = NSMakeRect(right_edge - width, widget_y, width, 16);
        label.alignment = NSTextAlignmentCenter;
        label.layer.backgroundColor =
            cell.highlight ? accent.CGColor : NSColor.clearColor.CGColor;
        right_edge -= width + 10;
      }
      // Left-aligned pills (the active-TODO pill): filled with the cell's
      // own color, text in the bar background color -- the X11 pill style.
      while (bottom_left_labels_.size() > chrome.bottom_left_widgets.size()) {
        [bottom_left_labels_.back() removeFromSuperview];
        bottom_left_labels_.pop_back();
      }
      while (bottom_left_labels_.size() < chrome.bottom_left_widgets.size()) {
        bottom_left_labels_.push_back(
            make_chrome_label(bottom_bar_, NSTextAlignmentCenter, chrome_font_));
      }
      CGFloat left_edge = 10;
      for (std::size_t index = 0; index < chrome.bottom_left_widgets.size(); ++index) {
        const core::BarCell& cell = chrome.bottom_left_widgets[index];
        NSTextField* label = bottom_left_labels_[index];
        // A cell with a color is a filled pill (text in the bar background
        // color); a colorless cell is plain bar text (the OS icon).
        NSColor* pill = cell.color.empty() ? nil : color_from_hex(cell.color);
        place_label(label, NSMakeRect(0, widget_y, 10, 16), cell.text,
                    pill != nil ? background : foreground);
        [label sizeToFit];
        const CGFloat width = label.frame.size.width + 12;
        label.frame = NSMakeRect(left_edge, widget_y, width, 16);
        label.alignment = NSTextAlignmentCenter;
        label.layer.backgroundColor = pill != nil ? pill.CGColor : NSColor.clearColor.CGColor;
        label.layer.cornerRadius = 4;
        left_edge += width + 10;
      }
      place_label(bottom_label_,
                  NSMakeRect(left_edge, widget_y,
                             std::max<CGFloat>(right_edge - left_edge - 10, 50), 16),
                  chrome.bottom_text, foreground);

      update_dock_cells(left_dock_, left_cell_labels_, chrome.left_cells, chrome.left, foreground,
                        accent, /*center_vertically=*/true);
      update_dock_cells(right_dock_, right_cell_labels_, chrome.right_cells, chrome.right,
                        foreground, accent);
    }
  }

  core::SystemStatus system_status() override {
    core::SystemStatus status;
    @autoreleasepool {
      read_battery(status);
      read_cpu(status);
      read_memory(status);
      read_disk(status);

      double load[1];
      if (getloadavg(load, 1) == 1) status.load_average = load[0];

      CWInterface* wifi = CWWiFiClient.sharedWiFiClient.interface;
      if (wifi != nil) {
        status.wifi_on = wifi.powerOn;
        // ssid is nil without the Location permission; the widget then
        // shows just "wifi on".
        if (wifi.ssid != nil) status.wifi_ssid = to_std_string(wifi.ssid);
      }

      IOBluetoothHostController* bluetooth = IOBluetoothHostController.defaultController;
      status.bluetooth_on =
          bluetooth != nil && bluetooth.powerState == kBluetoothHCIPowerStateON;
      if (status.bluetooth_on) {
        for (IOBluetoothDevice* device in IOBluetoothDevice.pairedDevices) {
          const std::string name = to_std_string(device.name);
          if (name.empty()) continue;
          status.bluetooth_devices.push_back({name, device.isConnected == TRUE, ""});
          if (device.isConnected && status.bluetooth_device.empty()) {
            status.bluetooth_device = name;
          }
        }
      }

      // One osascript round-trip for output volume, input (mic) volume, and
      // mute state: "63 50 false".
      const std::string volume = capture_command(
          "osascript -e 'set s to get volume settings' "
          "-e '(output volume of s as string) & \" \" & (input volume of s as string) & \" \" & "
          "(output muted of s as string)' 2>/dev/null");
      if (!volume.empty()) {
        int output = -1;
        int input = -1;
        char muted[8] = {0};
        if (std::sscanf(volume.c_str(), "%d %d %7s", &output, &input, muted) >= 1) {
          status.volume_percent = output;
          status.input_volume_percent = input;
          status.volume_muted = std::string(muted) == "true";
        }
      }

      // Now-playing from Spotify or Music. The pgrep gate matters twice
      // over: osascript would LAUNCH the player otherwise, and the script
      // only compiles when the app (and its scripting dictionary) exists.
      // First use triggers macOS's one-time Automation prompt per app.
      const auto now_playing = [](const char* app) {
        return capture_command(
            std::string("pgrep -xq '") + app + "' && osascript -e 'tell application \"" + app +
            "\"' -e 'if player state is playing then' -e '(artist of current track) & \" - \" & "
            "(name of current track)' -e 'end if' -e 'end tell' 2>/dev/null");
      };
      status.media_title = now_playing("Spotify");
      if (status.media_title.empty()) status.media_title = now_playing("Music");

      TISInputSourceRef source = TISCopyCurrentKeyboardInputSource();
      if (source != nullptr) {
        auto layout = (CFStringRef)TISGetInputSourceProperty(source, kTISPropertyLocalizedName);
        if (layout != nullptr) status.keyboard_layout = to_std_string((__bridge NSString*)layout);
        CFRelease(source);
      }
      for (const std::string& name : selectable_keyboard_layouts()) {
        status.keyboard_layouts.push_back({name, name == status.keyboard_layout, ""});
      }

      NSString* appearance = [NSApp.effectiveAppearance
          bestMatchFromAppearancesWithNames:@[ NSAppearanceNameAqua, NSAppearanceNameDarkAqua ]];
      status.appearance =
          [appearance isEqualToString:NSAppearanceNameDarkAqua] ? "dark" : "light";

      // AI agent processes (status-file agents are read by the engine; this
      // is the process-scan half, /proc-walk equivalent of the X11 backend).
      for (const char* kind : {"claude", "codex"}) {
        // ps comm (argv[0]) rather than pgrep: agent CLIs are typically
        // node binaries whose executable name pgrep would match instead.
        const std::string counted = capture_command(std::string("ps -axo comm= | grep -xc -e ") +
                                                    kind + " -e '.*/'" + kind + " 2>/dev/null");
        const int count = counted.empty() ? 0 : std::atoi(counted.c_str());
        if (count <= 0) continue;
        core::AgentInfo agent;
        agent.kind = kind;
        agent.status = "running";
        if (count > 1) agent.label = std::to_string(count) + " processes";
        status.agents.push_back(std::move(agent));
      }

      // Git state of the directory mepwm was launched from.
      status.git_branch =
          capture_command("git rev-parse --abbrev-ref HEAD 2>/dev/null");
      if (!status.git_branch.empty()) {
        status.git_status_lines =
            capture_command_lines("git status --porcelain 2>/dev/null", 20);
        status.git_dirty = static_cast<int>(status.git_status_lines.size());
      }
    }
    return status;
  }

  void perform(core::SystemAction action, int value, const std::string& argument) override {
    if (debug_) std::fprintf(stderr, "mepwm: perform action=%d value=%d\n", static_cast<int>(action), value);
    switch (action) {
      case core::SystemAction::SetOutputVolume:
        capture_command("osascript -e 'set volume output volume " + std::to_string(value) +
                        "' 2>/dev/null");
        break;
      case core::SystemAction::SetInputVolume:
        capture_command("osascript -e 'set volume input volume " + std::to_string(value) +
                        "' 2>/dev/null");
        break;
      case core::SystemAction::SetOutputMuted:
        capture_command(std::string("osascript -e 'set volume output muted ") +
                        (value != 0 ? "true" : "false") + "' 2>/dev/null");
        break;
      case core::SystemAction::ToggleAppearance:
        // Needs the System Events automation permission (one-time prompt).
        capture_command(
            "osascript -e 'tell application \"System Events\" to tell appearance preferences to "
            "set dark mode to not dark mode' 2>/dev/null");
        break;
      case core::SystemAction::SelectKeyboardLayout:
        select_keyboard_layout(argument);
        break;
      case core::SystemAction::SetWallpaper: {
        @autoreleasepool {
          NSString* path = [NSString stringWithUTF8String:argument.c_str()];
          NSURL* url = [NSURL fileURLWithPath:path];
          for (NSScreen* screen in NSScreen.screens) {
            NSError* error = nil;
            if (![NSWorkspace.sharedWorkspace setDesktopImageURL:url
                                                       forScreen:screen
                                                         options:@{}
                                                           error:&error] &&
                debug_) {
              std::fprintf(stderr, "mepwm: could not set wallpaper: %s\n",
                           error.localizedDescription.UTF8String);
            }
          }
        }
        break;
      }
      case core::SystemAction::MediaPrevious:
        media_command("previous track");
        break;
      case core::SystemAction::MediaPlayPause:
        media_command("playpause");
        break;
      case core::SystemAction::MediaNext:
        media_command("next track");
        break;
    }
  }

  void update_tab_bars(const std::vector<core::TabBar>& bars) override {
    @autoreleasepool {
      last_tab_bars_ = bars;
      while (tab_bar_windows_.size() > bars.size()) {
        [tab_bar_windows_.back() close];
        tab_bar_windows_.pop_back();
        tab_bar_labels_.pop_back();
      }
      while (tab_bar_windows_.size() < bars.size()) {
        NSPanel* window = make_chrome_window();
        const std::size_t bar_index = tab_bar_windows_.size();
        MacosPlatform* platform = this;
        ((MEPWMClickView*)window.contentView).onMouseDown = ^(NSPoint point) {
          platform->tab_bar_clicked(bar_index, point);
        };
        tab_bar_windows_.push_back(window);
        tab_bar_labels_.emplace_back();
      }
      const CGFloat screen_height = primary_screen_height();
      // Follow the engine's chrome palette so tab bars track the theme.
      NSColor* background = color_from_hex(last_chrome_.background_color);
      NSColor* foreground = color_from_hex(last_chrome_.text_color);
      NSColor* accent = color_from_hex(last_chrome_.accent_color);
      for (std::size_t index = 0; index < bars.size(); ++index) {
        const core::TabBar& bar = bars[index];
        NSWindow* window = tab_bar_windows_[index];
        const NSRect cocoa = NSMakeRect(bar.frame.x,
                                        screen_height - (bar.frame.y + bar.frame.height),
                                        bar.frame.width, bar.frame.height);
        [window setFrame:cocoa display:NO];
        window.contentView.layer.backgroundColor =
            [background colorWithAlphaComponent:0.94].CGColor;

        std::vector<NSTextField*>& labels = tab_bar_labels_[index];
        while (labels.size() > bar.tabs.size()) {
          [labels.back() removeFromSuperview];
          labels.pop_back();
        }
        while (labels.size() < bar.tabs.size()) {
          labels.push_back(make_chrome_label(window, NSTextAlignmentCenter, chrome_font_));
        }
        const CGFloat count = std::max<CGFloat>(1, static_cast<CGFloat>(bar.tabs.size()));
        const CGFloat gap = 2;
        const CGFloat inset = 4;
        const CGFloat cell_width =
            (bar.frame.width - 2 * inset - gap * (count - 1)) / count;
        for (std::size_t tab = 0; tab < bar.tabs.size(); ++tab) {
          const core::BarCell& cell = bar.tabs[tab];
          NSTextField* label = labels[tab];
          place_label(label,
                      NSMakeRect(inset + static_cast<CGFloat>(tab) * (cell_width + gap),
                                 (bar.frame.height - 18.0) / 2.0, cell_width, 18),
                      cell.text, cell.highlight ? NSColor.whiteColor : foreground);
          label.alignment = NSTextAlignmentCenter;
          label.layer.backgroundColor =
              cell.highlight ? accent.CGColor : NSColor.clearColor.CGColor;
        }
        [window orderFrontRegardless];
      }
    }
  }

  void tab_bar_clicked(std::size_t bar_index, NSPoint point) {
    if (handler_ == nullptr || bar_index >= tab_bar_labels_.size() ||
        bar_index >= last_tab_bars_.size()) {
      return;
    }
    const std::vector<NSTextField*>& labels = tab_bar_labels_[bar_index];
    const core::TabBar& bar = last_tab_bars_[bar_index];
    for (std::size_t index = 0; index < labels.size() && index < bar.tabs.size(); ++index) {
      if (!NSPointInRect(point, labels[index].frame)) continue;
      if (debug_) std::fprintf(stderr, "mepwm: tab click: %s\n", bar.tabs[index].id.c_str());
      core::Event event;
      event.type = core::Event::Type::ChromeClicked;
      event.area = core::ChromeArea::TabBar;
      event.cell = bar.tabs[index].id;
      handler_(event);
      return;
    }
  }

  // ---- Vimium-style click hints (the X11 backend's Super+f overlay) ----
  // A letter chip on every clickable chrome cell; typing a chip's label
  // replays the same ChromeClicked event the cell's real click handler
  // produces, so hint behavior can never drift from click behavior.

  struct HintTarget {
    std::string label;       // letters to type
    NSRect frame;            // target cell, screen coordinates
    core::ChromeArea area;
    std::string cell;        // BarCell id to report
  };

  static std::string hint_label(std::size_t index, std::size_t total) {
    static constexpr char kCharset[] = "asdfghjklqwertyuiopzxcvbnm";
    constexpr std::size_t kBase = 26;
    // (count, ch) constructor on purpose; see the X11 backend's note about
    // the initializer_list overload.
    if (total <= kBase) return std::string(1, kCharset[index]);  // NOLINT(modernize-return-braced-init-list)
    const std::size_t first = std::min(index / kBase, kBase - 1);
    return std::string(1, kCharset[first]) + kCharset[index % kBase];
  }

  void toggle_hints() override {
    @autoreleasepool {
      if (hints_visible_) {
        close_hints();
        return;
      }
      if (picker_visible_) return;
      build_hints();
      if (hints_.empty()) return;
      ensure_hint_panel();
      NSScreen* screen = NSScreen.screens.firstObject;
      if (screen == nil) return;
      [hint_panel_ setFrame:screen.frame display:NO];
      layout_hint_chips();
      hint_query_.clear();
      hints_visible_ = true;
      [hint_panel_ makeKeyAndOrderFront:nil];
      [hint_panel_ makeFirstResponder:hint_panel_.contentView];
    }
  }

  void build_hints() {
    hints_.clear();
    struct Candidate {
      NSRect frame;
      core::ChromeArea area;
      std::string cell;
    };
    std::vector<Candidate> candidates;
    const auto add_cells = [&](NSWindow* window, const std::vector<NSTextField*>& labels,
                               const std::vector<core::BarCell>& cells, core::ChromeArea area) {
      if (window == nil || !window.visible) return;
      for (std::size_t index = 0; index < labels.size() && index < cells.size(); ++index) {
        if (cells[index].id.empty()) continue;  // decorative, not clickable
        const NSRect frame = labels[index].frame;
        if (NSIsEmptyRect(frame)) continue;  // hidden overflow cells
        candidates.push_back({[window convertRectToScreen:frame], area, cells[index].id});
      }
    };
    add_cells(top_bar_, top_cell_labels_, last_chrome_.top_cells, core::ChromeArea::TopBar);
    add_cells(left_dock_, left_cell_labels_, last_chrome_.left_cells, core::ChromeArea::LeftDock);
    add_cells(right_dock_, right_cell_labels_, last_chrome_.right_cells,
              core::ChromeArea::RightDock);
    add_cells(bottom_bar_, bottom_widget_labels_, last_chrome_.bottom_widgets,
              core::ChromeArea::BottomBar);
    add_cells(bottom_bar_, bottom_left_labels_, last_chrome_.bottom_left_widgets,
              core::ChromeArea::BottomBar);
    for (std::size_t bar = 0; bar < tab_bar_windows_.size() && bar < tab_bar_labels_.size() &&
                              bar < last_tab_bars_.size();
         ++bar) {
      add_cells(tab_bar_windows_[bar], tab_bar_labels_[bar], last_tab_bars_[bar].tabs,
                core::ChromeArea::TabBar);
    }
    if (last_panel_.visible) {
      add_cells(side_panel_window_, side_panel_labels_, last_panel_.lines,
                core::ChromeArea::SidePanel);
    }
    // Reading order, top-left first: screen coordinates are bottom-left
    // origin, so larger y sorts earlier.
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& left, const Candidate& right) {
                return left.frame.origin.y != right.frame.origin.y
                           ? left.frame.origin.y > right.frame.origin.y
                           : left.frame.origin.x < right.frame.origin.x;
              });
    hints_.reserve(candidates.size());
    for (std::size_t index = 0; index < candidates.size(); ++index) {
      hints_.push_back({hint_label(index, candidates.size()), candidates[index].frame,
                        candidates[index].area, candidates[index].cell});
    }
  }

  void ensure_hint_panel() {
    if (hint_panel_ != nil) return;
    hint_panel_ = [[MEPWMPickerPanel alloc]
        initWithContentRect:NSMakeRect(0, 0, 16, 16)
                  styleMask:NSWindowStyleMaskBorderless | NSWindowStyleMaskNonactivatingPanel
                    backing:NSBackingStoreBuffered
                      defer:NO];
    hint_panel_.opaque = NO;
    hint_panel_.backgroundColor = NSColor.clearColor;
    hint_panel_.hasShadow = NO;
    // Above the chrome panels and border rings (NSFloatingWindowLevel).
    hint_panel_.level = NSPopUpMenuWindowLevel;
    hint_panel_.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces |
                                     NSWindowCollectionBehaviorStationary |
                                     NSWindowCollectionBehaviorIgnoresCycle |
                                     NSWindowCollectionBehaviorFullScreenAuxiliary;
    hint_panel_.releasedWhenClosed = NO;
    // Keys reach the panel (it is key, non-activating -- the picker's
    // technique) while clicks fall through to whatever is underneath.
    hint_panel_.ignoresMouseEvents = YES;
    MEPWMPickerView* view = [[MEPWMPickerView alloc] initWithFrame:NSMakeRect(0, 0, 16, 16)];
    view.wantsLayer = YES;
    MacosPlatform* platform = this;
    view.onKeyDown = ^(NSEvent* event) {
      platform->hint_key_down(event);
    };
    hint_panel_.contentView = view;
  }

  void layout_hint_chips() {
    while (hint_labels_.size() > hints_.size()) {
      [hint_labels_.back() removeFromSuperview];
      hint_labels_.pop_back();
    }
    while (hint_labels_.size() < hints_.size()) {
      hint_labels_.push_back(make_chrome_label(hint_panel_, NSTextAlignmentCenter, chrome_font_));
    }
    NSColor* chip = color_from_hex(last_chrome_.accent_color);
    NSColor* text = color_from_hex(last_chrome_.background_color);
    const NSPoint origin = hint_panel_.frame.origin;
    for (std::size_t index = 0; index < hints_.size(); ++index) {
      const HintTarget& hint = hints_[index];
      NSTextField* label = hint_labels_[index];
      label.stringValue = [NSString stringWithUTF8String:hint.label.c_str()];
      label.textColor = text;
      [label sizeToFit];
      const CGFloat width = std::max<CGFloat>(16, label.frame.size.width + 8);
      constexpr CGFloat kChipHeight = 16;
      // Chip over the target's top-left corner (where X11 places its chips),
      // converted from screen to panel coordinates.
      label.frame = NSMakeRect(hint.frame.origin.x - origin.x,
                               NSMaxY(hint.frame) - kChipHeight - origin.y, width, kChipHeight);
      label.alignment = NSTextAlignmentCenter;
      label.layer.backgroundColor = chip.CGColor;
      label.layer.cornerRadius = 3;
      label.hidden = NO;
    }
  }

  void refresh_hint_matches() {
    const HintTarget* exact = nullptr;
    for (std::size_t index = 0; index < hints_.size() && index < hint_labels_.size(); ++index) {
      const bool matches =
          hints_[index].label.compare(0, hint_query_.size(), hint_query_) == 0;
      hint_labels_[index].hidden = !matches;
      if (matches && hints_[index].label == hint_query_) exact = &hints_[index];
    }
    if (exact != nullptr) trigger_hint(*exact);
  }

  void trigger_hint(const HintTarget& hint) {
    // Copy before close_hints() clears the vector the reference points into.
    const core::ChromeArea area = hint.area;
    const std::string cell = hint.cell;
    close_hints();
    if (handler_ == nullptr) return;
    if (debug_) std::fprintf(stderr, "mepwm: hint click: %s\n", cell.c_str());
    core::Event event;
    event.type = core::Event::Type::ChromeClicked;
    event.area = area;
    event.cell = cell;
    handler_(event);
  }

  void hint_key_down(NSEvent* event) {
    if (!hints_visible_) return;
    const unsigned short code = event.keyCode;
    if (code == kVK_Escape) {
      close_hints();
      return;
    }
    if (code == kVK_Delete) {
      if (!hint_query_.empty()) {
        hint_query_.pop_back();
        refresh_hint_matches();
      }
      return;
    }
    // Command/Control chords are hotkeys (including the Mod+f that toggles
    // this overlay); they never contribute to the query.
    if ((event.modifierFlags & (NSEventModifierFlagCommand | NSEventModifierFlagControl)) != 0) {
      return;
    }
    bool changed = false;
    for (const char raw : to_std_string(event.charactersIgnoringModifiers)) {
      const char typed = static_cast<char>(std::tolower(static_cast<unsigned char>(raw)));
      if (typed < 'a' || typed > 'z') continue;
      const std::string candidate = hint_query_ + typed;
      const bool any_match =
          std::any_of(hints_.begin(), hints_.end(), [&](const HintTarget& hint) {
            return hint.label.compare(0, candidate.size(), candidate) == 0;
          });
      if (any_match) {
        hint_query_ = candidate;
        changed = true;
      }
    }
    if (changed) refresh_hint_matches();
  }

  void close_hints() {
    hints_visible_ = false;
    hint_query_.clear();
    hints_.clear();
    if (hint_panel_ != nil) [hint_panel_ orderOut:nil];
    // Hand the keyboard back to a side panel that had it before the overlay.
    if (last_panel_.visible && last_panel_.wants_keys && side_panel_window_ != nil) {
      [side_panel_window_ makeKeyAndOrderFront:nil];
    }
  }

  void update_side_panel(const core::SidePanel& panel) override {
    @autoreleasepool {
      last_panel_ = panel;
      if (!panel.visible) {
        if (side_panel_window_ != nil) [side_panel_window_ orderOut:nil];
        return;
      }
      if (side_panel_window_ == nil) {
        side_panel_window_ = make_side_panel_window();
        side_panel_title_ = make_chrome_label(side_panel_window_, NSTextAlignmentLeft, chrome_title_font_);
        side_panel_header_rule_ = make_panel_rule(side_panel_window_);
        side_panel_footer_rule_ = make_panel_rule(side_panel_window_);
        side_panel_footer_label_ =
            make_chrome_label(side_panel_window_, NSTextAlignmentCenter, chrome_font_);
        MacosPlatform* platform = this;
        MEPWMPanelView* view = (MEPWMPanelView*)side_panel_window_.contentView;
        view.onMouseDown = ^(NSPoint point) {
          platform->side_panel_clicked(point);
        };
        view.onKeyDown = ^(NSEvent* event) {
          platform->side_panel_key_down(event);
        };
      }
      const CGFloat screen_height = primary_screen_height();
      const NSRect cocoa = NSMakeRect(panel.frame.x,
                                      screen_height - (panel.frame.y + panel.frame.height),
                                      panel.frame.width, panel.frame.height);
      [side_panel_window_ setFrame:cocoa display:NO];
      side_panel_window_.contentView.layer.backgroundColor =
          [color_from_hex(last_chrome_.background_color) colorWithAlphaComponent:0.96].CGColor;

      NSColor* foreground = color_from_hex(last_chrome_.text_color);
      NSColor* accent = color_from_hex(last_chrome_.accent_color);
      NSColor* rule = [foreground colorWithAlphaComponent:0.25];

      place_label(side_panel_title_,
                  NSMakeRect(12, panel.frame.height - 30, panel.frame.width - 24, 18), panel.title,
                  accent);
      // Hairline under the title, matching the X11 panel's header separator.
      side_panel_header_rule_.frame = NSMakeRect(0, panel.frame.height - 36, panel.frame.width, 1);
      side_panel_header_rule_.layer.backgroundColor = rule.CGColor;

      // Footer: separator plus the centered "?  Toggle help" hint (the
      // engine only sets footer text for keyboard-grabbing panels).
      const CGFloat footer_height = panel.footer.empty() ? 0 : kSidePanelFooterHeight;
      side_panel_footer_rule_.hidden = footer_height == 0;
      side_panel_footer_label_.hidden = footer_height == 0;
      if (footer_height > 0) {
        side_panel_footer_rule_.frame = NSMakeRect(0, footer_height, panel.frame.width, 1);
        side_panel_footer_rule_.layer.backgroundColor = rule.CGColor;
        place_label(side_panel_footer_label_,
                    NSMakeRect(12, (footer_height - 16) / 2, panel.frame.width - 24, 16),
                    panel.footer, foreground);
      }

      while (side_panel_labels_.size() > panel.lines.size()) {
        [side_panel_labels_.back() removeFromSuperview];
        side_panel_labels_.pop_back();
      }
      while (side_panel_labels_.size() < panel.lines.size()) {
        side_panel_labels_.push_back(make_chrome_label(side_panel_window_, NSTextAlignmentLeft, chrome_font_));
      }
      constexpr CGFloat kLineHeight = 18;
      for (std::size_t index = 0; index < panel.lines.size(); ++index) {
        const core::BarCell& line = panel.lines[index];
        const CGFloat top = 40 + static_cast<CGFloat>(index) * kLineHeight;
        const CGFloat bottom = panel.frame.height - top - kLineHeight;
        place_label(side_panel_labels_[index],
                    NSMakeRect(12, bottom, panel.frame.width - 24, kLineHeight),
                    line.text, line.highlight ? NSColor.whiteColor : foreground);
        // Rows that would run into the footer strip are clipped out.
        side_panel_labels_[index].hidden = bottom < footer_height + 2;
        side_panel_labels_[index].layer.backgroundColor =
            line.highlight ? accent.CGColor : NSColor.clearColor.CGColor;
      }
      if (panel.wants_keys) {
        // The Spotlight technique again: the panel becomes key without
        // activating mepwm, so bare keys (?, a, ^n/^p) reach the engine
        // while the frontmost app keeps its focused look.
        [side_panel_window_ makeKeyAndOrderFront:nil];
        [side_panel_window_ makeFirstResponder:side_panel_window_.contentView];
      } else {
        [side_panel_window_ orderFrontRegardless];
      }
    }
  }

  void spawn(const std::string& shell_command) override {
    if (debug_) std::fprintf(stderr, "mepwm: spawn: %s\n", shell_command.c_str());
    // Double fork: the grandchild runs the command and is adopted by init, so
    // a long-lived program (a spawned terminal) is never mepwm's child. The
    // intermediate
    // exits immediately and is reaped right here, leaving popen/pclose as the
    // only wait() users in the process.
    const pid_t child = fork();
    if (child == 0) {
      setsid();
      const pid_t grandchild = fork();
      if (grandchild == 0) {
        execl("/bin/sh", "sh", "-c", shell_command.c_str(), static_cast<char*>(nullptr));
        _exit(127);
      }
      _exit(grandchild > 0 ? 0 : 127);
    }
    if (child > 0) {
      int status = 0;
      waitpid(child, &status, 0);
    }
  }

  void toggle_app_picker() override {
    @autoreleasepool {
      if (picker_visible_ && picker_kind_ == PickerKind::Apps) {
        close_app_picker();
        return;
      }
      // Replacing a showing list picker counts as cancelling it.
      if (picker_visible_) notify_picker_event(core::Event::Type::PickerCancelled);
      picker_kind_ = PickerKind::Apps;
      picker_title_ = "Run";
      // Rescan on every open: the scan is a handful of directory listings,
      // and it keeps freshly installed apps visible without a cache.
      picker_items_ = scan_applications();
      open_picker();
    }
  }

  void show_list_picker(const std::string& title, const std::vector<core::PickerItem>& items,
                        const std::string& selected_id = {}) override {
    @autoreleasepool {
      if (picker_visible_ && picker_kind_ == PickerKind::List && picker_title_ == title) {
        close_app_picker();
        notify_picker_event(core::Event::Type::PickerCancelled);
        return;
      }
      picker_kind_ = PickerKind::List;
      picker_title_ = title;
      picker_items_ = items;
      picker_initial_id_ = selected_id;
      open_picker();
    }
  }

 private:
  enum class PickerKind { Apps, List };

  void open_picker() {
    picker_query_.clear();
    picker_selection_ = 0;
    picker_scroll_ = 0;
    ensure_picker_panel();
    picker_visible_ = true;
    filter_picker_items();
    // Start highlighted on the caller-requested row (the theme picker's
    // committed theme) instead of the first one.
    if (!picker_initial_id_.empty()) {
      for (std::size_t row = 0; row < picker_filtered_.size(); ++row) {
        if (picker_items_[picker_filtered_[row]].id == picker_initial_id_) {
          picker_selection_ = row;
          if (row >= kPickerMaxRows) picker_scroll_ = row - kPickerMaxRows + 1;
          break;
        }
      }
      picker_initial_id_.clear();
    }
    layout_picker();
    [picker_panel_ makeKeyAndOrderFront:nil];
    [picker_panel_ makeFirstResponder:picker_panel_.contentView];
    notify_picker_event(core::Event::Type::PickerHighlighted);
  }

  // Reports list-picker state to the engine: highlight changes drive live
  // previews (theme picker), cancels revert them. App-picker state stays
  // platform-internal.
  void notify_picker_event(core::Event::Type type) {
    if (handler_ == nullptr || picker_kind_ != PickerKind::List) return;
    core::Event event;
    event.type = type;
    if (type == core::Event::Type::PickerHighlighted &&
        picker_selection_ < picker_filtered_.size()) {
      event.cell = picker_items_[picker_filtered_[picker_selection_]].id;
    }
    handler_(event);
  }
  void ensure_picker_panel() {
    if (picker_panel_ != nil) return;
    picker_panel_ = [[MEPWMPickerPanel alloc]
        initWithContentRect:NSMakeRect(0, 0, kPickerWidth, 120)
                  styleMask:NSWindowStyleMaskBorderless | NSWindowStyleMaskNonactivatingPanel
                    backing:NSBackingStoreBuffered
                      defer:NO];
    picker_panel_.opaque = NO;
    picker_panel_.backgroundColor = NSColor.clearColor;
    picker_panel_.hasShadow = YES;
    // Above the chrome panels and border rings (NSFloatingWindowLevel).
    picker_panel_.level = NSPopUpMenuWindowLevel;
    picker_panel_.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces |
                                       NSWindowCollectionBehaviorStationary |
                                       NSWindowCollectionBehaviorIgnoresCycle |
                                       NSWindowCollectionBehaviorFullScreenAuxiliary;
    picker_panel_.releasedWhenClosed = NO;
    MEPWMPickerView* view =
        [[MEPWMPickerView alloc] initWithFrame:NSMakeRect(0, 0, kPickerWidth, 120)];
    view.wantsLayer = YES;
    view.layer.cornerRadius = 10.0;
    view.layer.masksToBounds = YES;
    MacosPlatform* platform = this;
    view.onKeyDown = ^(NSEvent* event) {
      platform->picker_key_down(event);
    };
    picker_panel_.contentView = view;
    picker_query_label_ = make_chrome_label(
        picker_panel_, NSTextAlignmentLeft,
        [NSFont monospacedSystemFontOfSize:15.0 weight:NSFontWeightSemibold]);
    picker_row_font_ = [NSFont monospacedSystemFontOfSize:13.0 weight:NSFontWeightRegular];
  }

  void close_app_picker() {
    picker_visible_ = false;
    if (picker_panel_ != nil) [picker_panel_ orderOut:nil];
  }

  void picker_key_down(NSEvent* event) {
    if (!picker_visible_) return;
    const unsigned short code = event.keyCode;
    const bool ctrl = (event.modifierFlags & NSEventModifierFlagControl) != 0;
    if (code == kVK_Escape) {
      close_app_picker();
      notify_picker_event(core::Event::Type::PickerCancelled);
      return;
    }
    if (code == kVK_Return || code == kVK_ANSI_KeypadEnter) {
      launch_selected_app();
      return;
    }
    if (code == kVK_UpArrow || (ctrl && (code == kVK_ANSI_P || code == kVK_ANSI_K))) {
      move_picker_selection(-1);
      return;
    }
    if (code == kVK_DownArrow || (ctrl && (code == kVK_ANSI_N || code == kVK_ANSI_J))) {
      move_picker_selection(1);
      return;
    }
    if (code == kVK_Delete) {
      if (!picker_query_.empty()) {
        // Input is filtered to ASCII below, so pop_back removes one glyph.
        picker_query_.pop_back();
        picker_selection_ = 0;
        picker_scroll_ = 0;
        filter_picker_items();
        layout_picker();
        notify_picker_event(core::Event::Type::PickerHighlighted);
      }
      return;
    }
    // Command chords are hotkeys (including the Mod+p that toggles this
    // picker); they never contribute to the query.
    if ((event.modifierFlags & NSEventModifierFlagCommand) != 0 || ctrl) return;
    bool changed = false;
    for (const char typed : to_std_string(event.charactersIgnoringModifiers)) {
      if (typed >= 0x20 && typed < 0x7F) {
        picker_query_.push_back(typed);
        changed = true;
      }
    }
    if (changed) {
      picker_selection_ = 0;
      picker_scroll_ = 0;
      filter_picker_items();
      layout_picker();
      notify_picker_event(core::Event::Type::PickerHighlighted);
    }
  }

  void move_picker_selection(int delta) {
    if (picker_filtered_.empty()) return;
    const std::size_t count = picker_filtered_.size();
    picker_selection_ =
        (picker_selection_ + count + (delta > 0 ? 1 : count - 1)) % count;
    if (picker_selection_ < picker_scroll_) picker_scroll_ = picker_selection_;
    if (picker_selection_ >= picker_scroll_ + kPickerMaxRows) {
      picker_scroll_ = picker_selection_ - kPickerMaxRows + 1;
    }
    layout_picker();
    notify_picker_event(core::Event::Type::PickerHighlighted);
  }

  void filter_picker_items() {
    picker_filtered_.clear();
    std::vector<std::pair<int, std::size_t>> scored;
    for (std::size_t index = 0; index < picker_items_.size(); ++index) {
      const int score = fuzzy_score(picker_query_, picker_items_[index].label);
      if (score >= 0) scored.push_back({score, index});
    }
    // Stable: equal scores keep the supplied order (alphabetical for apps).
    std::stable_sort(scored.begin(), scored.end(),
                     [](const auto& left, const auto& right) { return left.first > right.first; });
    for (const auto& [score, index] : scored) picker_filtered_.push_back(index);
    if (picker_selection_ >= picker_filtered_.size()) picker_selection_ = 0;
  }

  void launch_selected_app() {
    const bool has_selection = picker_selection_ < picker_filtered_.size();
    const core::PickerItem selected =
        has_selection ? picker_items_[picker_filtered_[picker_selection_]] : core::PickerItem{};
    if (picker_kind_ == PickerKind::List) {
      // The engine decides what the choice means; it also gets the raw query
      // so typed input (e.g. a new project path) can beat the match list.
      const std::string query = picker_query_;
      close_app_picker();
      if (handler_ != nullptr) {
        core::Event event;
        event.type = core::Event::Type::PickerSelected;
        event.cell = selected.id;
        event.text = query;
        handler_(event);
      }
      return;
    }
    if (has_selection) {
      if (debug_) std::fprintf(stderr, "mepwm: launch: %s\n", selected.id.c_str());
      NSString* path = [NSString stringWithUTF8String:selected.id.c_str()];
      if (path != nil) {
        [NSWorkspace.sharedWorkspace
            openApplicationAtURL:[NSURL fileURLWithPath:path]
                   configuration:[NSWorkspaceOpenConfiguration configuration]
               completionHandler:nil];
      }
    }
    close_app_picker();
  }

  void layout_picker() {
    const std::size_t row_count =
        picker_filtered_.empty()
            ? 1  // one row for the "(no matches)" placeholder
            : std::min<std::size_t>(picker_filtered_.size() - picker_scroll_, kPickerMaxRows);
    const CGFloat height =
        kPickerQueryHeight + static_cast<CGFloat>(row_count) * kPickerRowHeight + 10;
    // Centered horizontally, upper part of the work area (launcher-style).
    const core::Rect area = work_area();
    const CGFloat panel_x = area.x + (area.width - kPickerWidth) / 2;
    const CGFloat panel_top = area.y + area.height / 5;  // top-left origin
    [picker_panel_ setFrame:NSMakeRect(panel_x, primary_screen_height() - (panel_top + height),
                                       kPickerWidth, height)
                    display:NO];
    picker_panel_.contentView.layer.backgroundColor =
        [color_from_hex(last_chrome_.background_color) colorWithAlphaComponent:0.97].CGColor;
    NSColor* foreground = color_from_hex(last_chrome_.text_color);
    NSColor* accent = color_from_hex(last_chrome_.accent_color);
    place_label(picker_query_label_,
                NSMakeRect(14, height - kPickerQueryHeight + 8, kPickerWidth - 28, 20),
                picker_title_ + ": " + picker_query_ + "_", accent);
    while (picker_row_labels_.size() > row_count) {
      [picker_row_labels_.back() removeFromSuperview];
      picker_row_labels_.pop_back();
    }
    while (picker_row_labels_.size() < row_count) {
      picker_row_labels_.push_back(
          make_chrome_label(picker_panel_, NSTextAlignmentLeft, picker_row_font_));
    }
    if (picker_filtered_.empty()) {
      place_label(picker_row_labels_[0],
                  NSMakeRect(14, height - kPickerQueryHeight - kPickerRowHeight + 4,
                             kPickerWidth - 28, 18),
                  "(no matches)", foreground);
      picker_row_labels_[0].layer.backgroundColor = NSColor.clearColor.CGColor;
      return;
    }
    for (std::size_t row = 0; row < row_count; ++row) {
      const std::size_t item = picker_scroll_ + row;
      const bool selected = item == picker_selection_;
      NSTextField* label = picker_row_labels_[row];
      const CGFloat row_top = kPickerQueryHeight + static_cast<CGFloat>(row) * kPickerRowHeight;
      place_label(label,
                  NSMakeRect(8, height - row_top - kPickerRowHeight, kPickerWidth - 16,
                             kPickerRowHeight - 4),
                  " " + picker_items_[picker_filtered_[item]].label,
                  selected ? NSColor.whiteColor : foreground);
      label.layer.backgroundColor = selected ? accent.CGColor : NSColor.clearColor.CGColor;
      label.layer.cornerRadius = 4;
    }
  }

 public:

  void run(const std::function<void(const core::Event&)>& on_event) override {
    handler_ = on_event;
    @autoreleasepool {
      start_poll_timer();
      [NSApp run];
      stop_poll_timer();
    }
    handler_ = nullptr;
  }

  void stop() override {
    [NSApp stop:nil];
    // -stop: only takes effect once the loop dequeues an event; feed it one.
    NSEvent* wake = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                       location:NSZeroPoint
                                  modifierFlags:0
                                      timestamp:0
                                   windowNumber:0
                                        context:nil
                                        subtype:0
                                          data1:0
                                          data2:0];
    [NSApp postEvent:wake atStart:YES];
  }

  // Maps a click in a chrome panel to the cell label under it and reports
  // the cell's id to the engine.
  void chrome_clicked(core::ChromeArea area, NSPoint point) {
    if (handler_ == nullptr) return;
    std::string cell;
    if (area == core::ChromeArea::BottomBar) {
      for (std::size_t index = 0;
           index < bottom_widget_labels_.size() && index < last_chrome_.bottom_widgets.size();
           ++index) {
        if (NSPointInRect(point, bottom_widget_labels_[index].frame)) {
          cell = last_chrome_.bottom_widgets[index].id;
          break;
        }
      }
      for (std::size_t index = 0; cell.empty() && index < bottom_left_labels_.size() &&
                                  index < last_chrome_.bottom_left_widgets.size();
           ++index) {
        if (NSPointInRect(point, bottom_left_labels_[index].frame)) {
          cell = last_chrome_.bottom_left_widgets[index].id;
        }
      }
    } else if (area == core::ChromeArea::LeftDock) {
      for (std::size_t index = 0;
           index < left_cell_labels_.size() && index < last_chrome_.left_cells.size(); ++index) {
        if (NSPointInRect(point, left_cell_labels_[index].frame)) {
          cell = last_chrome_.left_cells[index].id;
          break;
        }
      }
    } else if (area == core::ChromeArea::TopBar) {
      for (std::size_t index = 0;
           index < top_cell_labels_.size() && index < last_chrome_.top_cells.size(); ++index) {
        if (NSPointInRect(point, top_cell_labels_[index].frame)) {
          cell = last_chrome_.top_cells[index].id;
          break;
        }
      }
    } else if (area == core::ChromeArea::RightDock) {
      for (std::size_t index = 0;
           index < right_cell_labels_.size() && index < last_chrome_.right_cells.size(); ++index) {
        if (NSPointInRect(point, right_cell_labels_[index].frame)) {
          cell = last_chrome_.right_cells[index].id;
          break;
        }
      }
    }
    if (cell.empty()) return;
    if (debug_) std::fprintf(stderr, "mepwm: chrome click: %s\n", cell.c_str());
    core::Event event;
    event.type = core::Event::Type::ChromeClicked;
    event.area = area;
    event.cell = cell;
    handler_(event);
  }

  void side_panel_clicked(NSPoint point) {
    if (handler_ == nullptr) return;
    for (std::size_t index = 0;
         index < side_panel_labels_.size() && index < last_panel_.lines.size(); ++index) {
      if (!NSPointInRect(point, side_panel_labels_[index].frame)) continue;
      const std::string& cell = last_panel_.lines[index].id;
      if (cell.empty()) return;
      if (debug_) std::fprintf(stderr, "mepwm: panel click: %s\n", cell.c_str());
      core::Event event;
      event.type = core::Event::Type::ChromeClicked;
      event.area = core::ChromeArea::SidePanel;
      event.cell = cell;
      handler_(event);
      return;
    }
  }

  // Raw key presses from the side panel while it holds keyboard focus,
  // translated to engine PanelKeyPressed events: special keys by keycode,
  // printable input as Character text with a ctrl flag (^n/^p navigation).
  void side_panel_key_down(NSEvent* event) {
    if (handler_ == nullptr || !last_panel_.visible || !last_panel_.wants_keys) return;
    core::Event key_event;
    key_event.type = core::Event::Type::PanelKeyPressed;
    key_event.ctrl = (event.modifierFlags & NSEventModifierFlagControl) != 0;
    const unsigned short code = event.keyCode;
    if (code == kVK_Escape) {
      key_event.panel_key = core::PanelKey::Escape;
    } else if (code == kVK_Return || code == kVK_ANSI_KeypadEnter) {
      key_event.panel_key = core::PanelKey::Return;
    } else if (code == kVK_Delete) {
      key_event.panel_key = core::PanelKey::Backspace;
    } else if (code == kVK_UpArrow) {
      key_event.panel_key = core::PanelKey::Up;
    } else if (code == kVK_DownArrow) {
      key_event.panel_key = core::PanelKey::Down;
    } else {
      // Command chords stay hotkeys; they never reach the panel as text.
      if ((event.modifierFlags & NSEventModifierFlagCommand) != 0) return;
      key_event.panel_key = core::PanelKey::Character;
      for (const char typed : to_std_string(event.charactersIgnoringModifiers)) {
        if (typed >= 0x20 && typed < 0x7F) key_event.text.push_back(typed);
      }
      if (key_event.text.empty()) return;
    }
    handler_(key_event);
  }

  // Localized names of all selectable keyboard layouts/input modes.
  static std::vector<std::string> selectable_keyboard_layouts() {
    std::vector<std::string> names;
    NSDictionary* filter = @{
      (__bridge NSString*)kTISPropertyInputSourceCategory :
          (__bridge NSString*)kTISCategoryKeyboardInputSource
    };
    NSArray* sources =
        CFBridgingRelease(TISCreateInputSourceList((__bridge CFDictionaryRef)filter, false));
    for (id item in sources) {
      auto source = (__bridge TISInputSourceRef)item;
      auto selectable =
          (CFBooleanRef)TISGetInputSourceProperty(source, kTISPropertyInputSourceIsSelectCapable);
      if (selectable == nullptr || !CFBooleanGetValue(selectable)) continue;
      auto name = (CFStringRef)TISGetInputSourceProperty(source, kTISPropertyLocalizedName);
      if (name != nullptr) names.push_back(to_std_string((__bridge NSString*)name));
    }
    return names;
  }

  static void select_keyboard_layout(const std::string& layout_name) {
    NSDictionary* filter = @{
      (__bridge NSString*)kTISPropertyInputSourceCategory :
          (__bridge NSString*)kTISCategoryKeyboardInputSource
    };
    NSArray* sources =
        CFBridgingRelease(TISCreateInputSourceList((__bridge CFDictionaryRef)filter, false));
    for (id item in sources) {
      auto source = (__bridge TISInputSourceRef)item;
      auto name = (CFStringRef)TISGetInputSourceProperty(source, kTISPropertyLocalizedName);
      if (name != nullptr && to_std_string((__bridge NSString*)name) == layout_name) {
        TISSelectInputSource(source);
        return;
      }
    }
  }

  // Sends a transport command to whichever supported player is running.
  static void media_command(const std::string& verb) {
    for (const char* app : {"Spotify", "Music"}) {
      const std::string running = capture_command(std::string("pgrep -xq '") + app + "' && echo y");
      if (running != "y") continue;
      capture_command(std::string("osascript -e 'tell application \"") + app + "\" to " + verb +
                      "' 2>/dev/null");
      return;
    }
  }

  void dispatch_hotkey(UInt32 index) {
    if (debug_) std::fprintf(stderr, "mepwm: hotkey pressed id=%u\n", static_cast<unsigned int>(index));
    if (handler_ == nullptr || index >= hotkeys_.size()) return;
    core::Event event;
    event.type = core::Event::Type::ActionTriggered;
    event.action = hotkeys_[index].action;
    event.argument = hotkeys_[index].argument;
    handler_(event);
  }

 private:
  struct AxWindow {
    AXUIElementRef element;
    pid_t owner;
  };
  struct HotkeyEntry {
    EventHotKeyRef reference = nullptr;  // Carbon path only; null for fn-tap entries
    core::Action action = core::Action::FocusLeft;
    int argument = 0;
    UInt32 key_code = 0;  // fn-tap path only
    bool shift = false;   // fn-tap path only
    bool ctrl = false;    // fn-tap path only
  };

  // AXUIElementRefs for the same window compare CFEqual, which gives us a
  // stable public-API window identity: the registry index (+1) is the
  // WindowId handed to the engine.
  core::WindowId intern(AXUIElementRef window, pid_t owner) {
    for (std::size_t index = 0; index < registry_.size(); ++index) {
      if (CFEqual(registry_[index].element, window)) return index + 1;
    }
    CFRetain(window);
    registry_.push_back({window, owner});
    return registry_.size();
  }

  AXUIElementRef element(core::WindowId window_id) const {
    if (window_id == core::kNoWindow || window_id > registry_.size()) return nullptr;
    return registry_[window_id - 1].element;
  }

  static void set_position(AXUIElementRef window, const core::Rect& frame) {
    CGPoint point = CGPointMake(frame.x, frame.y);
    AXValueRef value = AXValueCreate(kAXValueTypeCGPoint, &point);
    AXUIElementSetAttributeValue(window, kAXPositionAttribute, value);
    CFRelease(value);
  }

  // Vertical stack of small text cells inside a dock panel; highlighted
  // cells get the accent color as background.
  void update_dock_cells(NSWindow* dock, std::vector<NSTextField*>& labels,
                         const std::vector<core::BarCell>& cells, const core::Rect& panel,
                         NSColor* foreground, NSColor* accent, bool center_vertically = false) {
    while (labels.size() > cells.size()) {
      [labels.back() removeFromSuperview];
      labels.pop_back();
    }
    while (labels.size() < cells.size()) {
      labels.push_back(make_chrome_label(dock, NSTextAlignmentCenter, chrome_font_));
    }
    constexpr CGFloat kCellHeight = 24;
    constexpr CGFloat kCellGap = 4;
    // The launcher column sits vertically centered in its dock (X11's
    // left_dock_top_offset); the right dock's toggles stay top-aligned.
    const CGFloat column_height =
        static_cast<CGFloat>(cells.size()) * (kCellHeight + kCellGap) + kCellGap;
    const CGFloat centering_offset =
        center_vertically ? std::max<CGFloat>(0, (panel.height - column_height) / 2) : 0;
    for (std::size_t index = 0; index < cells.size(); ++index) {
      // Cells fill top-down; the panel's content view is bottom-left origin.
      const CGFloat top_offset = centering_offset + kCellGap +
                                 static_cast<CGFloat>(index) * (kCellHeight + kCellGap);
      const NSRect frame = NSMakeRect(4, panel.height - top_offset - kCellHeight,
                                      panel.width - 8, kCellHeight);
      place_label(labels[index], frame, cells[index].text,
                  cells[index].highlight ? NSColor.whiteColor : foreground);
      labels[index].layer.backgroundColor =
          cells[index].highlight ? accent.CGColor : NSColor.clearColor.CGColor;
    }
  }

  // Matches fn-modified key presses when the primary modifier involves the
  // Globe/fn key (fn alone, or fn held with Command when fn_requires_cmd_),
  // which RegisterEventHotKey cannot express. An active
  // (filtering) keyboard tap only needs the Accessibility trust the app
  // already requires. Matched events are swallowed so the frontmost app
  // never sees them.
  static CGEventRef fn_tap_callback(CGEventTapProxy, CGEventType type, CGEventRef event,
                                    void* user) {
    auto* self = static_cast<MacosPlatform*>(user);
    if (type == kCGEventTapDisabledByTimeout || type == kCGEventTapDisabledByUserInput) {
      CGEventTapEnable(self->event_tap_, true);
      return event;
    }
    if (type != kCGEventKeyDown) return event;
    const CGEventFlags flags = CGEventGetFlags(event);
    if ((flags & kCGEventFlagMaskSecondaryFn) == 0) return event;
    const bool cmd = (flags & kCGEventFlagMaskCommand) != 0;
    if (cmd != self->fn_requires_cmd_) return event;
    if ((flags & kCGEventFlagMaskAlternate) != 0) return event;
    const bool shift = (flags & kCGEventFlagMaskShift) != 0;
    const bool ctrl = (flags & kCGEventFlagMaskControl) != 0;
    auto key_code =
        static_cast<UInt32>(CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode));
    // Holding fn turns Return into keypad Enter on Apple keyboards; fold it
    // back so Mod+Return bindings registered as kVK_Return still match.
    if (key_code == kVK_ANSI_KeypadEnter) key_code = kVK_Return;
    for (std::size_t index = 0; index < self->hotkeys_.size(); ++index) {
      const HotkeyEntry& entry = self->hotkeys_[index];
      if (entry.reference != nullptr) continue;
      if (entry.key_code != key_code || entry.shift != shift || entry.ctrl != ctrl) continue;
      // Dispatch async: retiling can outlast the tap's response deadline,
      // and a timed-out tap gets disabled by the system.
      dispatch_async(dispatch_get_main_queue(), ^{
        self->dispatch_hotkey(static_cast<UInt32>(index));
      });
      return nullptr;
    }
    return event;
  }

  bool ensure_event_tap() {
    if (event_tap_ != nullptr) return true;
    event_tap_ = CGEventTapCreate(kCGSessionEventTap, kCGHeadInsertEventTap, kCGEventTapOptionDefault,
                                  CGEventMaskBit(kCGEventKeyDown), &fn_tap_callback, this);
    if (event_tap_ == nullptr) {
      if (debug_) std::fprintf(stderr, "mepwm: failed to create fn event tap\n");
      return false;
    }
    event_tap_source_ = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, event_tap_, 0);
    CFRunLoopAddSource(CFRunLoopGetMain(), event_tap_source_, kCFRunLoopCommonModes);
    CGEventTapEnable(event_tap_, true);
    return true;
  }

  bool ensure_hotkey_handler() {
    if (hotkey_handler_ != nullptr) return true;
    EventTypeSpec pressed;
    pressed.eventClass = kEventClassKeyboard;
    pressed.eventKind = kEventHotKeyPressed;
    const OSStatus status = InstallEventHandler(
        GetEventDispatcherTarget(),
        [](EventHandlerCallRef, EventRef event, void* user) -> OSStatus {
          EventHotKeyID identifier;
          if (GetEventParameter(event, kEventParamDirectObject, typeEventHotKeyID, nullptr,
                                sizeof(identifier), nullptr, &identifier) == noErr &&
              identifier.signature == kHotkeySignature) {
            static_cast<MacosPlatform*>(user)->dispatch_hotkey(identifier.id);
          }
          return noErr;
        },
        1, &pressed, this, &hotkey_handler_);
    return status == noErr;
  }

  // Until AXObserver-based events land (docs/PORTING.md phase 2), a poll
  // timer diffs the window set and focus so the engine can retile.
  void start_poll_timer() {
    previous_ids_ = snapshot_ids();
    previous_focus_ = focused_window();
    poll_timer_ = CFRunLoopTimerCreateWithHandler(
        kCFAllocatorDefault, CFAbsoluteTimeGetCurrent() + 0.5, 0.5, 0, 0,
        ^(CFRunLoopTimerRef) {
          poll();
        });
    CFRunLoopAddTimer(CFRunLoopGetMain(), poll_timer_, kCFRunLoopCommonModes);
  }

  void stop_poll_timer() {
    if (poll_timer_ == nullptr) return;
    CFRunLoopTimerInvalidate(poll_timer_);
    CFRelease(poll_timer_);
    poll_timer_ = nullptr;
  }

  std::vector<core::WindowId> snapshot_ids() {
    std::vector<core::WindowId> ids;
    for (const core::WindowInfo& info : list_windows()) ids.push_back(info.id);
    std::sort(ids.begin(), ids.end());
    return ids;
  }

  void poll() {
    if (handler_ == nullptr) return;
    std::vector<core::WindowId> ids = snapshot_ids();
    if (ids != previous_ids_) {
      previous_ids_ = std::move(ids);
      core::Event event;
      event.type = core::Event::Type::WindowsChanged;
      handler_(event);
    }
    const core::WindowId focus = focused_window();
    if (focus != previous_focus_) {
      previous_focus_ = focus;
      core::Event event;
      event.type = core::Event::Type::FocusChanged;
      event.window = focus;
      handler_(event);
    }
    core::Event tick;
    tick.type = core::Event::Type::Tick;
    handler_(tick);
  }

  std::vector<AxWindow> registry_;
  std::vector<HotkeyEntry> hotkeys_;
  std::vector<NSWindow*> border_windows_;
  NSWindow* top_bar_ = nil;
  NSWindow* bottom_bar_ = nil;
  NSWindow* left_dock_ = nil;
  NSWindow* right_dock_ = nil;
  NSTextField* top_left_label_ = nil;
  NSTextField* top_center_label_ = nil;
  NSTextField* top_right_label_ = nil;
  NSTextField* bottom_label_ = nil;
  std::vector<NSTextField*> top_cell_labels_;
  std::vector<NSTextField*> left_cell_labels_;
  std::vector<NSTextField*> right_cell_labels_;
  std::vector<NSTextField*> bottom_widget_labels_;
  std::vector<NSTextField*> bottom_left_labels_;
  core::Chrome last_chrome_;
  NSWindow* side_panel_window_ = nil;
  NSTextField* side_panel_title_ = nil;
  NSView* side_panel_header_rule_ = nil;
  NSView* side_panel_footer_rule_ = nil;
  NSTextField* side_panel_footer_label_ = nil;
  std::vector<NSTextField*> side_panel_labels_;
  core::SidePanel last_panel_;
  std::vector<NSWindow*> tab_bar_windows_;
  std::vector<std::vector<NSTextField*>> tab_bar_labels_;
  std::vector<core::TabBar> last_tab_bars_;
  NSPanel* hint_panel_ = nil;  // full-screen key-capturing hint overlay
  std::vector<NSTextField*> hint_labels_;
  std::vector<HintTarget> hints_;
  std::string hint_query_;
  bool hints_visible_ = false;
  NSPanel* picker_panel_ = nil;
  NSTextField* picker_query_label_ = nil;
  NSFont* picker_row_font_ = nil;
  std::vector<NSTextField*> picker_row_labels_;
  PickerKind picker_kind_ = PickerKind::Apps;
  std::string picker_title_;
  std::vector<core::PickerItem> picker_items_;
  std::vector<std::size_t> picker_filtered_;  // indices into picker_items_, best first
  std::string picker_query_;
  std::string picker_initial_id_;  // row to highlight on open; consumed by open_picker()
  std::size_t picker_selection_ = 0;
  std::size_t picker_scroll_ = 0;
  bool picker_visible_ = false;
  NSFont* chrome_font_ = nil;
  NSFont* chrome_title_font_ = nil;
  std::string icon_font_name_;
  UInt32 primary_modifier_ = cmdKey;
  bool use_fn_tap_ = false;
  bool fn_requires_cmd_ = false;
  CFMachPortRef event_tap_ = nullptr;
  CFRunLoopSourceRef event_tap_source_ = nullptr;
  bool debug_ = std::getenv("MEPWM_DEBUG") != nullptr;
  EventHandlerRef hotkey_handler_ = nullptr;
  CFRunLoopTimerRef poll_timer_ = nullptr;
  std::function<void(const core::Event&)> handler_;
  std::vector<core::WindowId> previous_ids_;
  core::WindowId previous_focus_ = core::kNoWindow;
};

}  // namespace

std::unique_ptr<Backend> make_macos_backend() {
  return std::make_unique<core::TilingEngine>(std::make_unique<MacosPlatform>());
}

}  // namespace mepwm
