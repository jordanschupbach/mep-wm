// Windows implementation of core::Platform. Like the macOS backend, mepwm
// cannot replace the Windows shell's window manager (DWM), so it runs as an
// overlay (the GlazeWM/komorebi approach): it enumerates other applications'
// top-level windows, arranges them with SetWindowPos, and drives the shared
// core::TilingEngine from a low-level keyboard hook. Bars, docks, borders,
// pickers, and panels are non-activating topmost popups painted with GDI.

#include "backend.hpp"

#include <windows.h>
#include <windowsx.h>

#include <bluetoothapis.h>
#include <dwmapi.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <wlanapi.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/engine.hpp"
#include "core/platform.hpp"

namespace mepwm {
namespace {

// ---- small helpers ---------------------------------------------------------

std::wstring widen(const std::string& text) {
  if (text.empty()) return {};
  const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                         nullptr, 0);
  std::wstring result(static_cast<std::size_t>(length), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length);
  return result;
}

std::string narrow(const std::wstring& text) {
  if (text.empty()) return {};
  const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
  std::string result(static_cast<std::size_t>(length), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length,
                      nullptr, nullptr);
  return result;
}

COLORREF color_from_hex(const std::string& spec) {
  unsigned int red = 68;
  unsigned int green = 68;
  unsigned int blue = 68;
  if (spec.size() == 7 && spec[0] == '#') {
    std::sscanf(spec.c_str() + 1, "%02x%02x%02x", &red, &green, &blue);
  }
  return RGB(red, green, blue);
}

// `amount` of the way from `base` toward `toward` (0..1), per channel.
COLORREF blend(COLORREF base, COLORREF toward, double amount) {
  const auto channel = [amount](int from, int to) {
    return static_cast<BYTE>(std::clamp(
        static_cast<int>(from + (to - from) * amount), 0, 255));
  };
  return RGB(channel(GetRValue(base), GetRValue(toward)), channel(GetGValue(base), GetGValue(toward)),
             channel(GetBValue(base), GetBValue(toward)));
}

RECT to_rect(const core::Rect& rect) {
  return RECT{rect.x, rect.y, rect.x + rect.width, rect.y + rect.height};
}

HWND to_hwnd(core::WindowId window) {
  return reinterpret_cast<HWND>(static_cast<std::uintptr_t>(window));
}

core::WindowId to_id(HWND window) {
  return static_cast<core::WindowId>(reinterpret_cast<std::uintptr_t>(window));
}

bool debug_enabled() { return std::getenv("MEPWM_DEBUG") != nullptr; }

// Marks input mepwm injects itself (the menu-mask key, media keys) so the
// keyboard hook lets it through untouched.
constexpr ULONG_PTR kInjectedTag = 0x4D455057;  // 'MEPW'
// An unassigned virtual key: pressing it while Alt/Win is held makes the
// shell treat the later Alt/Win release as a chord, not a tap, so a
// swallowed Alt+h hotkey doesn't open the app's menu bar and Win+h doesn't
// pop the Start menu (the AutoHotkey "menu mask key" technique).
constexpr WORD kMenuMaskKey = 0xE8;

void send_key_tap(WORD key) {
  INPUT inputs[2] = {};
  for (INPUT& input : inputs) {
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = key;
    input.ki.dwExtraInfo = kInjectedTag;
  }
  inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
  SendInput(2, inputs, sizeof(INPUT));
}

// Lets the next window another process opens (a spawned terminal, an app
// from the picker) take the foreground. Windows only grants that to the
// process that received the last input event; hotkeys arrive through a hook,
// which doesn't count, so an empty mouse input claims it first (the
// komorebi/GlazeWM technique).
void claim_foreground_rights() {
  INPUT input{};
  input.type = INPUT_MOUSE;
  input.mi.dwExtraInfo = kInjectedTag;
  SendInput(1, &input, sizeof input);
}

// ---- hidden (parked) windows --------------------------------------------------
// Windows of inactive workspaces are hidden with ShowWindow(SW_HIDE). If
// mepwm died without showing them again they would be unreachable (no
// taskbar button, no Alt+Tab entry), so the set is mirrored to a file that
// the next start (and the crash handler) uses to restore them.

std::filesystem::path hidden_windows_file() {
  wchar_t temp[MAX_PATH + 1] = {};
  const DWORD length = GetTempPathW(MAX_PATH + 1, temp);
  return std::filesystem::path(std::wstring(temp, length)) / L"mepwm-hidden-windows.txt";
}

// HWND values get reused, so each entry also records the owning process
// and the class name; a restore only touches windows that still match
// (otherwise a stale file could show some other app's hidden helper window).
std::wstring hidden_window_class(HWND window) {
  wchar_t buffer[128] = {};
  GetClassNameW(window, buffer, 128);
  std::wstring name = buffer;
  std::replace(name.begin(), name.end(), L' ', L'_');
  return name.empty() ? L"-" : name;
}

DWORD hidden_window_pid(HWND window) {
  DWORD pid = 0;
  GetWindowThreadProcessId(window, &pid);
  return pid;
}

void save_hidden_windows(const std::set<HWND>& windows) {
  std::error_code error;
  if (windows.empty()) {
    std::filesystem::remove(hidden_windows_file(), error);
    return;
  }
  std::wofstream file(hidden_windows_file(), std::ios::trunc);
  for (HWND window : windows) {
    file << reinterpret_cast<std::uintptr_t>(window) << L' ' << hidden_window_pid(window) << L' '
         << hidden_window_class(window) << L'\n';
  }
}

void show_hidden_windows_from_file() {
  std::wifstream file(hidden_windows_file());
  std::uintptr_t value = 0;
  DWORD pid = 0;
  std::wstring name;
  while (file >> value >> pid >> name) {
    HWND window = reinterpret_cast<HWND>(value);
    if (!IsWindow(window) || IsWindowVisible(window)) continue;
    if (hidden_window_pid(window) != pid || hidden_window_class(window) != name) continue;
    ShowWindowAsync(window, SW_SHOWNOACTIVATE);
  }
  file.close();
  std::error_code error;
  std::filesystem::remove(hidden_windows_file(), error);
}

LONG WINAPI show_hidden_windows_on_crash(EXCEPTION_POINTERS*) {
  show_hidden_windows_from_file();
  return EXCEPTION_CONTINUE_SEARCH;
}

// ---- subprocesses -----------------------------------------------------------

// Runs `command` under cmd.exe with no console window and returns up to
// `limit` lines of its stdout (trimmed). Gives up after `timeout_ms` so a
// wedged tool cannot stall the event loop.
std::vector<std::string> capture_command_lines(const std::string& command, std::size_t limit,
                                               DWORD timeout_ms = 1500, bool* timed_out = nullptr) {
  std::vector<std::string> lines;
  SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
  HANDLE read_end = nullptr;
  HANDLE write_end = nullptr;
  if (!CreatePipe(&read_end, &write_end, &security, 0)) return lines;
  SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);
  STARTUPINFOW startup{};
  startup.cb = sizeof startup;
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdOutput = write_end;
  startup.hStdError = write_end;
  startup.hStdInput = nullptr;
  PROCESS_INFORMATION process{};
  std::wstring line = L"cmd.exe /d /s /c \"" + widen(command) + L"\"";
  // The child runs in a kill-on-close job: on timeout, closing the job
  // kills cmd.exe *and* what it started (git), instead of orphaning it.
  HANDLE job = CreateJobObjectW(nullptr, nullptr);
  if (job != nullptr) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof limits);
  }
  const BOOL started = CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE,
                                      CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &startup,
                                      &process);
  CloseHandle(write_end);
  if (!started) {
    CloseHandle(read_end);
    if (job != nullptr) CloseHandle(job);
    return lines;
  }
  if (job != nullptr) AssignProcessToJobObject(job, process.hProcess);
  ResumeThread(process.hThread);
  std::string output;
  const ULONGLONG deadline = GetTickCount64() + timeout_ms;
  char buffer[512];
  for (;;) {
    DWORD available = 0;
    if (!PeekNamedPipe(read_end, nullptr, 0, nullptr, &available, nullptr)) break;  // closed
    if (available > 0) {
      DWORD count = 0;
      if (!ReadFile(read_end, buffer, std::min<DWORD>(available, sizeof buffer), &count, nullptr) ||
          count == 0) {
        break;
      }
      output.append(buffer, count);
      continue;
    }
    if (WaitForSingleObject(process.hProcess, 10) == WAIT_OBJECT_0) {
      // Drain whatever arrived between the peek and the exit.
      DWORD count = 0;
      while (PeekNamedPipe(read_end, nullptr, 0, nullptr, &available, nullptr) && available > 0 &&
             ReadFile(read_end, buffer, std::min<DWORD>(available, sizeof buffer), &count, nullptr) &&
             count > 0) {
        output.append(buffer, count);
      }
      break;
    }
    if (GetTickCount64() > deadline) {
      if (timed_out != nullptr) *timed_out = true;
      if (job != nullptr) TerminateJobObject(job, 1);
      TerminateProcess(process.hProcess, 1);
      break;
    }
  }
  if (job != nullptr) CloseHandle(job);
  CloseHandle(read_end);
  CloseHandle(process.hProcess);
  CloseHandle(process.hThread);
  std::size_t start = 0;
  while (start < output.size() && lines.size() < limit) {
    std::size_t end = output.find('\n', start);
    if (end == std::string::npos) end = output.size();
    std::string text = output.substr(start, end - start);
    while (!text.empty() && (text.back() == '\r' || text.back() == ' ')) text.pop_back();
    lines.push_back(std::move(text));
    start = end + 1;
  }
  while (!lines.empty() && lines.back().empty()) lines.pop_back();
  return lines;
}

std::string capture_command(const std::string& command, bool* timed_out = nullptr) {
  const std::vector<std::string> lines = capture_command_lines(command, 1, 1500, timed_out);
  return lines.empty() ? std::string() : lines.front();
}

// ---- window classification --------------------------------------------------

DWORD window_process(HWND window) {
  DWORD pid = 0;
  GetWindowThreadProcessId(window, &pid);
  return pid;
}

std::wstring class_name(HWND window) {
  wchar_t buffer[128] = {};
  GetClassNameW(window, buffer, 128);
  return buffer;
}

std::wstring window_title(HWND window) {
  const int length = GetWindowTextLengthW(window);
  if (length <= 0) return {};
  std::wstring title(static_cast<std::size_t>(length) + 1, L'\0');
  const int copied = GetWindowTextW(window, title.data(), length + 1);
  title.resize(static_cast<std::size_t>(std::max(copied, 0)));
  return title;
}

bool is_cloaked(HWND window) {
  DWORD cloaked = 0;
  return SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof cloaked)) &&
         cloaked != 0;
}

// The frame DWM actually draws. Windows 10/11 windows carry invisible
// resize borders (~7px) outside it that GetWindowRect includes; tiling by
// GetWindowRect would leave visible gaps.
RECT visible_frame(HWND window) {
  RECT frame{};
  if (FAILED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof frame))) {
    GetWindowRect(window, &frame);
  }
  return frame;
}

bool is_more_privileged(DWORD pid);

// True for an app window covering its whole monitor (taskbar included):
// browser F11, video players, games, slideshows. The desktop and the shell
// also span the monitor but are not fullscreen apps.
bool is_fullscreen(HWND window) {
  if (window == nullptr || !IsWindowVisible(window) || IsIconic(window)) return false;
  if (window == GetDesktopWindow() || window == GetShellWindow()) return false;
  wchar_t name[64] = {};
  GetClassNameW(window, name, 64);
  if (std::wstring(name) == L"WorkerW" || std::wstring(name) == L"Progman") return false;
  MONITORINFO monitor{};
  monitor.cbSize = sizeof monitor;
  if (!GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor)) return false;
  RECT frame{};
  GetWindowRect(window, &frame);
  return frame.left <= monitor.rcMonitor.left && frame.top <= monitor.rcMonitor.top &&
         frame.right >= monitor.rcMonitor.right && frame.bottom >= monitor.rcMonitor.bottom;
}

// A window the engine should tile: a visible, resizable, unowned top-level
// application window, as opposed to dialogs, tool palettes, the shell's
// own surfaces, and UWP windows cloaked on other virtual desktops.
bool is_manageable(HWND window, DWORD own_pid) {
  if (!IsWindowVisible(window) || IsIconic(window)) return false;
  if (GetAncestor(window, GA_ROOT) != window) return false;
  if (GetWindow(window, GW_OWNER) != nullptr) return false;
  const LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE);
  const LONG_PTR ex_style = GetWindowLongPtrW(window, GWL_EXSTYLE);
  if ((ex_style & WS_EX_TOOLWINDOW) != 0 || (ex_style & WS_EX_NOACTIVATE) != 0) return false;
  if ((style & WS_CHILD) != 0 || (style & WS_THICKFRAME) == 0) return false;
  if (is_cloaked(window)) return false;
  if (GetWindowTextLengthW(window) == 0) return false;
  const DWORD pid = window_process(window);
  if (pid == own_pid || is_more_privileged(pid)) return false;
  static const wchar_t* const kShellClasses[] = {
      L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd",
      L"Windows.UI.Core.CoreWindow", L"XamlExplorerHostIslandWindow",
      L"TopLevelWindowForOverflowXamlIsland", L"MultitaskingViewFrame",
      L"ForegroundStaging", L"NotifyIconOverflowWindow",
  };
  const std::wstring name = class_name(window);
  for (const wchar_t* shell_class : kShellClasses) {
    if (name == shell_class) return false;
  }
  const RECT frame = visible_frame(window);
  return frame.right - frame.left > 1 && frame.bottom - frame.top > 1;
}

DWORD process_integrity(HANDLE process) {
  HANDLE token = nullptr;
  if (!OpenProcessToken(process, TOKEN_QUERY, &token)) return 0;
  DWORD level = 0;
  BYTE buffer[64] = {};
  DWORD size = 0;
  if (GetTokenInformation(token, TokenIntegrityLevel, buffer, sizeof buffer, &size)) {
    const auto* label = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(buffer);
    const UCHAR count = *GetSidSubAuthorityCount(label->Label.Sid);
    if (count > 0) level = *GetSidSubAuthority(label->Label.Sid, count - 1U);
  }
  CloseHandle(token);
  return level;
}

// Windows of elevated (administrator) programs can't be moved or hidden by
// a normal-integrity mepwm (UIPI silently ignores it), so tiling them would
// leave a pane the window never fills. They stay unmanaged.
bool is_more_privileged(DWORD pid) {
  static const DWORD own_level = process_integrity(GetCurrentProcess());
  static std::unordered_map<DWORD, std::pair<ULONGLONG, bool>> cache;  // pid -> (checked at, result)
  const ULONGLONG now = GetTickCount64();
  const auto cached = cache.find(pid);
  if (cached != cache.end() && now - cached->second.first < 30000) return cached->second.second;
  bool higher = false;
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (process == nullptr) {
    higher = true;  // can't even query it: protected or elevated
  } else {
    const DWORD level = process_integrity(process);
    // A token we may not open belongs to a higher integrity level.
    higher = level == 0 || level > own_level;
    CloseHandle(process);
  }
  if (cache.size() > 512) cache.clear();
  cache[pid] = {now, higher};
  return higher;
}

std::wstring process_image_path(DWORD pid) {
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (process == nullptr) return {};
  wchar_t buffer[MAX_PATH * 2] = {};
  DWORD size = MAX_PATH * 2;
  std::wstring result;
  if (QueryFullProcessImageNameW(process, 0, buffer, &size)) result.assign(buffer, size);
  CloseHandle(process);
  return result;
}

// ---- key mapping ------------------------------------------------------------

UINT virtual_key(core::Key key) {
  switch (key) {
    case core::Key::B: return 'B';
    case core::Key::D: return 'D';
    case core::Key::F: return 'F';
    case core::Key::H: return 'H';
    case core::Key::I: return 'I';
    case core::Key::J: return 'J';
    case core::Key::K: return 'K';
    case core::Key::L: return 'L';
    case core::Key::M: return 'M';
    case core::Key::N: return 'N';
    case core::Key::O: return 'O';
    case core::Key::P: return 'P';
    case core::Key::R: return 'R';
    case core::Key::S: return 'S';
    case core::Key::T: return 'T';
    case core::Key::U: return 'U';
    case core::Key::V: return 'V';
    case core::Key::W: return 'W';
    case core::Key::Return: return VK_RETURN;
    case core::Key::Tab: return VK_TAB;
    case core::Key::Q: return 'Q';
    case core::Key::Minus: return VK_OEM_MINUS;
    case core::Key::Equal: return VK_OEM_PLUS;  // the =/+ key
    case core::Key::Slash: return VK_OEM_2;     // the //? key on US layouts
    case core::Key::N1: return '1';
    case core::Key::N2: return '2';
    case core::Key::N3: return '3';
    case core::Key::N4: return '4';
    case core::Key::N5: return '5';
    case core::Key::N6: return '6';
    case core::Key::N7: return '7';
    case core::Key::N8: return '8';
    case core::Key::N9: return '9';
  }
  return 'H';
}

bool key_down(int virtual_key_code) { return (GetAsyncKeyState(virtual_key_code) & 0x8000) != 0; }

bool is_modifier_key(DWORD vk) {
  switch (vk) {
    case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
    case VK_MENU: case VK_LMENU: case VK_RMENU:
    case VK_LWIN: case VK_RWIN: case VK_CAPITAL:
      return true;
    default:
      return false;
  }
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

// Start-menu shortcuts are the Windows analogue of /Applications: every
// installer drops one there. Rescanned per open, like the macOS picker.
void scan_shortcuts(const std::filesystem::path& directory, int depth,
                    std::vector<core::PickerItem>& apps) {
  std::error_code error;
  for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
    const std::filesystem::path& path = entry.path();
    if (entry.is_directory(error)) {
      if (depth > 0) scan_shortcuts(path, depth - 1, apps);
      continue;
    }
    std::wstring extension = path.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(), ::towlower);
    if (extension != L".lnk" && extension != L".url" && extension != L".appref-ms") continue;
    std::string label = narrow(path.stem().wstring());
    std::string lowered = label;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lowered.find("uninstall") != std::string::npos) continue;
    apps.push_back({narrow(path.wstring()), std::move(label), {}});
  }
}

std::filesystem::path known_folder(REFKNOWNFOLDERID id) {
  PWSTR raw = nullptr;
  std::filesystem::path result;
  if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &raw))) result = raw;
  CoTaskMemFree(raw);
  return result;
}

std::vector<core::PickerItem> scan_applications() {
  std::vector<core::PickerItem> apps;
  for (REFKNOWNFOLDERID folder : {FOLDERID_CommonPrograms, FOLDERID_Programs}) {
    const std::filesystem::path root = known_folder(folder);
    if (!root.empty()) scan_shortcuts(root, 3, apps);
  }
  std::sort(apps.begin(), apps.end(), [](const core::PickerItem& left, const core::PickerItem& right) {
    return left.label < right.label;
  });
  // The same app often has a shortcut in both the all-users and per-user
  // menus; keep one row per name.
  apps.erase(std::unique(apps.begin(), apps.end(),
                         [](const core::PickerItem& left, const core::PickerItem& right) {
                           return left.label == right.label;
                         }),
             apps.end());
  return apps;
}

// ---- Surface: one non-activating topmost popup painted with GDI -------------

constexpr wchar_t kSurfaceClass[] = L"MepwmSurface";
constexpr wchar_t kMessageClass[] = L"MepwmMessageWindow";

struct DrawItem {
  RECT rect{};  // client coordinates
  std::wstring text;
  COLORREF text_color = RGB(255, 255, 255);
  bool filled = false;
  COLORREF fill = RGB(0, 0, 0);
  int radius = 0;
  UINT align = DT_CENTER;
  HFONT font = nullptr;
  bool hidden = false;
  // Clickable when non-empty; reported with `area` (and stays the hint
  // target identity).
  std::string id;
  core::ChromeArea area = core::ChromeArea::BottomBar;
};

class Surface {
 public:
  enum class Kind {
    Panel,    // opaque, clickable chrome
    Ring,     // click-through border ring (window region with a hole)
    Overlay,  // click-through, color-keyed transparency (hint chips)
  };

  Surface(Kind kind, int alpha, bool rounded = false) : kind_(kind), alpha_(static_cast<BYTE>(alpha)) {
    DWORD ex_style = WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_LAYERED;
    if (kind != Kind::Panel) ex_style |= WS_EX_TRANSPARENT;
    hwnd_ = CreateWindowExW(ex_style, kSurfaceClass, L"mepwm", WS_POPUP, 0, 0, 16, 16, nullptr,
                            nullptr, GetModuleHandleW(nullptr), this);
    if (kind == Kind::Overlay) {
      SetLayeredWindowAttributes(hwnd_, kColorKey, 0, LWA_COLORKEY);
    } else {
      SetLayeredWindowAttributes(hwnd_, 0, alpha_, LWA_ALPHA);
    }
    // Square corners for bars, panels, and rings (Windows 11 would round
    // every popup); only the floating picker asks for rounding.
    const DWM_WINDOW_CORNER_PREFERENCE corners = rounded ? DWMWCP_ROUND : DWMWCP_DONOTROUND;
    DwmSetWindowAttribute(hwnd_, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof corners);
  }
  ~Surface() {
    if (hwnd_ != nullptr) DestroyWindow(hwnd_);
  }
  Surface(const Surface&) = delete;
  Surface& operator=(const Surface&) = delete;
  Surface(Surface&&) = delete;
  Surface& operator=(Surface&&) = delete;

  static constexpr COLORREF kColorKey = RGB(255, 0, 255);

  HWND hwnd() const { return hwnd_; }
  bool visible() const { return visible_; }
  const RECT& frame() const { return frame_; }
  std::vector<DrawItem>& items() { return items_; }
  const std::vector<DrawItem>& items() const { return items_; }

  void set_background(COLORREF color) { background_ = color; }
  void set_rule(int y, COLORREF color) { rules_.push_back({y, color}); }
  void clear_rules() { rules_.clear(); }
  void set_ring_width(int width) { ring_width_ = width; }
  void set_on_click(std::function<void(const DrawItem&)> on_click) { on_click_ = std::move(on_click); }
  void set_on_any_click(std::function<void()> on_any_click) { on_any_click_ = std::move(on_any_click); }

  // Moves/resizes and shows. Only the first show (or `raise`) touches the
  // z-order, so a picker opened above the bars stays above them while the
  // bars repaint every tick.
  void show(const RECT& frame, bool raise = false) {
    const bool moved = !EqualRect(&frame, &frame_);
    frame_ = frame;
    UINT flags = SWP_NOACTIVATE | SWP_SHOWWINDOW;
    if (visible_ && !raise) flags |= SWP_NOZORDER;
    if (!moved && visible_) flags |= SWP_NOMOVE | SWP_NOSIZE;
    if (kind_ == Kind::Ring && (moved || !visible_)) apply_ring_region();
    SetWindowPos(hwnd_, HWND_TOPMOST, frame.left, frame.top, frame.right - frame.left,
                 frame.bottom - frame.top, flags);
    visible_ = true;
    InvalidateRect(hwnd_, nullptr, FALSE);
  }

  void hide() {
    if (!visible_) return;
    ShowWindow(hwnd_, SW_HIDE);
    visible_ = false;
  }

  // Screen rectangle of an item.
  RECT screen_rect(const DrawItem& item) const {
    RECT rect = item.rect;
    OffsetRect(&rect, frame_.left, frame_.top);
    return rect;
  }

  static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE) {
      const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* self = reinterpret_cast<Surface*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (message) {
      case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
      case WM_ERASEBKGND:
        return 1;
      case WM_PAINT:
        if (self != nullptr) {
          self->paint();
          return 0;
        }
        break;
      case WM_LBUTTONDOWN:
        if (self != nullptr) {
          self->clicked(POINT{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
          return 0;
        }
        break;
      case WM_NCDESTROY:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        break;
      default:
        break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
  }

 private:
  void apply_ring_region() {
    const int width = frame_.right - frame_.left;
    const int height = frame_.bottom - frame_.top;
    HRGN outer = CreateRectRgn(0, 0, width, height);
    HRGN inner = CreateRectRgn(ring_width_, ring_width_, width - ring_width_, height - ring_width_);
    CombineRgn(outer, outer, inner, RGN_DIFF);
    DeleteObject(inner);
    SetWindowRgn(hwnd_, outer, FALSE);  // the system owns `outer` now
  }

  void clicked(POINT point) {
    if (on_any_click_) on_any_click_();
    for (const DrawItem& item : items_) {
      if (item.hidden || item.id.empty() || !PtInRect(&item.rect, point)) continue;
      if (on_click_) {
        // Copy: the handler may rebuild items_ while it runs.
        const DrawItem target = item;
        on_click_(target);
      }
      return;
    }
  }

  void paint() {
    PAINTSTRUCT paint_struct;
    HDC target = BeginPaint(hwnd_, &paint_struct);
    RECT client;
    GetClientRect(hwnd_, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    // Double buffered: chrome repaints on every tick and would flicker.
    HDC dc = CreateCompatibleDC(target);
    HBITMAP bitmap = CreateCompatibleBitmap(target, std::max(width, 1), std::max(height, 1));
    HGDIOBJ previous_bitmap = SelectObject(dc, bitmap);
    HBRUSH background = CreateSolidBrush(kind_ == Kind::Overlay ? kColorKey : background_);
    FillRect(dc, &client, background);
    DeleteObject(background);
    for (const auto& [y, color] : rules_) {
      RECT rule{0, y, width, y + 1};
      HBRUSH brush = CreateSolidBrush(color);
      FillRect(dc, &rule, brush);
      DeleteObject(brush);
    }
    SetBkMode(dc, TRANSPARENT);
    for (const DrawItem& item : items_) {
      if (item.hidden || IsRectEmpty(&item.rect)) continue;
      if (item.filled) {
        HBRUSH brush = CreateSolidBrush(item.fill);
        if (item.radius > 0) {
          HPEN pen = CreatePen(PS_SOLID, 1, item.fill);
          HGDIOBJ old_pen = SelectObject(dc, pen);
          HGDIOBJ old_brush = SelectObject(dc, brush);
          RoundRect(dc, item.rect.left, item.rect.top, item.rect.right, item.rect.bottom,
                    item.radius * 2, item.radius * 2);
          SelectObject(dc, old_brush);
          SelectObject(dc, old_pen);
          DeleteObject(pen);
        } else {
          FillRect(dc, &item.rect, brush);
        }
        DeleteObject(brush);
      }
      if (item.text.empty()) continue;
      HGDIOBJ old_font = SelectObject(dc, item.font != nullptr ? item.font : GetStockObject(DEFAULT_GUI_FONT));
      SetTextColor(dc, item.text_color);
      RECT text_rect = item.rect;
      if (item.align != DT_CENTER) InflateRect(&text_rect, -2, 0);
      DrawTextW(dc, item.text.c_str(), static_cast<int>(item.text.size()), &text_rect,
                item.align | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
      SelectObject(dc, old_font);
    }
    BitBlt(target, 0, 0, width, height, dc, 0, 0, SRCCOPY);
    SelectObject(dc, previous_bitmap);
    DeleteObject(bitmap);
    DeleteDC(dc);
    EndPaint(hwnd_, &paint_struct);
  }

  Kind kind_;
  BYTE alpha_;
  HWND hwnd_ = nullptr;
  RECT frame_{};
  bool visible_ = false;
  int ring_width_ = 2;
  COLORREF background_ = RGB(29, 31, 33);
  std::vector<std::pair<int, COLORREF>> rules_;
  std::vector<DrawItem> items_;
  std::function<void(const DrawItem&)> on_click_;
  std::function<void()> on_any_click_;
};

// ---- system status probes ----------------------------------------------------

template <typename T>
void release(T*& pointer) {
  if (pointer != nullptr) {
    pointer->Release();
    pointer = nullptr;
  }
}

// Default render (speakers) or capture (microphone) endpoint's volume
// control; null when there is no such device.
IAudioEndpointVolume* endpoint_volume(EDataFlow flow) {
  IMMDeviceEnumerator* enumerator = nullptr;
  if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                              __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator)))) {
    return nullptr;
  }
  IMMDevice* device = nullptr;
  IAudioEndpointVolume* volume = nullptr;
  if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(flow, eConsole, &device))) {
    device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                     reinterpret_cast<void**>(&volume));
  }
  release(device);
  release(enumerator);
  return volume;
}

int read_volume(EDataFlow flow, bool* muted) {
  IAudioEndpointVolume* volume = endpoint_volume(flow);
  if (volume == nullptr) return -1;
  float level = 0.0F;
  int percent = -1;
  if (SUCCEEDED(volume->GetMasterVolumeLevelScalar(&level))) {
    percent = static_cast<int>(level * 100.0F + 0.5F);
  }
  if (muted != nullptr) {
    BOOL is_muted = FALSE;
    if (SUCCEEDED(volume->GetMute(&is_muted))) *muted = is_muted != FALSE;
  }
  release(volume);
  return percent;
}

void write_volume(EDataFlow flow, int percent) {
  IAudioEndpointVolume* volume = endpoint_volume(flow);
  if (volume == nullptr) return;
  volume->SetMasterVolumeLevelScalar(static_cast<float>(std::clamp(percent, 0, 100)) / 100.0F, nullptr);
  release(volume);
}

void write_mute(bool muted) {
  IAudioEndpointVolume* volume = endpoint_volume(eRender);
  if (volume == nullptr) return;
  volume->SetMute(muted ? TRUE : FALSE, nullptr);
  release(volume);
}

void read_battery(core::SystemStatus& status) {
  SYSTEM_POWER_STATUS power{};
  if (!GetSystemPowerStatus(&power)) return;
  // 128 = no system battery (desktops); 255 = unknown.
  if ((power.BatteryFlag & 128) != 0 || power.BatteryLifePercent > 100) return;
  status.battery_percent = power.BatteryLifePercent;
  status.battery_charging = (power.BatteryFlag & 8) != 0 || power.ACLineStatus == 1;
  if (!status.battery_charging && power.BatteryLifeTime != static_cast<DWORD>(-1)) {
    status.battery_minutes_remaining = static_cast<int>(power.BatteryLifeTime / 60);
  }
}

void read_memory(core::SystemStatus& status) {
  MEMORYSTATUSEX memory{};
  memory.dwLength = sizeof memory;
  if (GlobalMemoryStatusEx(&memory)) status.memory_used_percent = static_cast<int>(memory.dwMemoryLoad);
}

std::uint64_t filetime_ticks(const FILETIME& time) {
  return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32U) | time.dwLowDateTime;
}

// CPU usage from GetSystemTimes deltas -- the analogue of the X11 backend's
// /proc/stat sampling; -1 until the second sample.
void read_cpu(core::SystemStatus& status) {
  FILETIME idle_time;
  FILETIME kernel_time;
  FILETIME user_time;
  if (!GetSystemTimes(&idle_time, &kernel_time, &user_time)) return;
  static std::uint64_t previous_total = 0;
  static std::uint64_t previous_idle = 0;
  const std::uint64_t idle = filetime_ticks(idle_time);
  // Kernel time includes idle time.
  const std::uint64_t total = filetime_ticks(kernel_time) + filetime_ticks(user_time);
  if (previous_total > 0 && total > previous_total) {
    const std::uint64_t total_delta = total - previous_total;
    const std::uint64_t idle_delta = idle - previous_idle;
    status.cpu_used_percent =
        static_cast<int>(100 * (total_delta - std::min(idle_delta, total_delta)) / total_delta);
  }
  previous_total = total;
  previous_idle = idle;
}

void read_disk(core::SystemStatus& status) {
  wchar_t windows_dir[MAX_PATH] = {};
  if (GetWindowsDirectoryW(windows_dir, MAX_PATH) < 3) return;
  const std::wstring drive(windows_dir, 3);  // "C:\"
  ULARGE_INTEGER available{};
  ULARGE_INTEGER total{};
  ULARGE_INTEGER free_bytes{};
  if (!GetDiskFreeSpaceExW(drive.c_str(), &available, &total, &free_bytes) || total.QuadPart == 0) {
    return;
  }
  const std::uint64_t used = total.QuadPart - free_bytes.QuadPart;
  const std::uint64_t reachable = used + available.QuadPart;
  if (reachable > 0) status.disk_used_percent = static_cast<int>(used * 100 / reachable);
}

void read_wifi(core::SystemStatus& status) {
  static HANDLE client = nullptr;
  if (client == nullptr) {
    DWORD negotiated = 0;
    if (WlanOpenHandle(2, nullptr, &negotiated, &client) != ERROR_SUCCESS) {
      client = nullptr;
      return;
    }
  }
  PWLAN_INTERFACE_INFO_LIST interfaces = nullptr;
  if (WlanEnumInterfaces(client, nullptr, &interfaces) != ERROR_SUCCESS || interfaces == nullptr) return;
  for (DWORD index = 0; index < interfaces->dwNumberOfItems; ++index) {
    const WLAN_INTERFACE_INFO& info = interfaces->InterfaceInfo[index];
    if (info.isState == wlan_interface_state_not_ready) continue;
    status.wifi_on = true;
    if (info.isState != wlan_interface_state_connected) continue;
    DWORD size = 0;
    PWLAN_CONNECTION_ATTRIBUTES connection = nullptr;
    if (WlanQueryInterface(client, &info.InterfaceGuid, wlan_intf_opcode_current_connection, nullptr,
                           &size, reinterpret_cast<PVOID*>(&connection), nullptr) == ERROR_SUCCESS &&
        connection != nullptr) {
      const DOT11_SSID& ssid = connection->wlanAssociationAttributes.dot11Ssid;
      status.wifi_ssid.assign(reinterpret_cast<const char*>(ssid.ucSSID),
                              std::min<ULONG>(ssid.uSSIDLength, DOT11_SSID_MAX_LENGTH));
      WlanFreeMemory(connection);
    }
    break;
  }
  WlanFreeMemory(interfaces);
}

void read_bluetooth(core::SystemStatus& status) {
  BLUETOOTH_FIND_RADIO_PARAMS radio_params{sizeof(BLUETOOTH_FIND_RADIO_PARAMS)};
  HANDLE radio = nullptr;
  HBLUETOOTH_RADIO_FIND radio_find = BluetoothFindFirstRadio(&radio_params, &radio);
  if (radio_find == nullptr) return;
  BluetoothFindRadioClose(radio_find);
  status.bluetooth_on = true;
  BLUETOOTH_DEVICE_SEARCH_PARAMS search{};
  search.dwSize = sizeof search;
  search.fReturnAuthenticated = TRUE;
  search.fReturnRemembered = TRUE;
  search.fReturnConnected = TRUE;
  search.fIssueInquiry = FALSE;  // cached devices only: no radio scan
  search.hRadio = radio;
  BLUETOOTH_DEVICE_INFO device{};
  device.dwSize = sizeof device;
  HBLUETOOTH_DEVICE_FIND device_find = BluetoothFindFirstDevice(&search, &device);
  if (device_find != nullptr) {
    do {
      const std::string name = narrow(device.szName);
      if (!name.empty()) {
        status.bluetooth_devices.push_back({name, device.fConnected != FALSE, ""});
        if (device.fConnected && status.bluetooth_device.empty()) status.bluetooth_device = name;
      }
      device.dwSize = sizeof device;
    } while (status.bluetooth_devices.size() < 16 && BluetoothFindNextDevice(device_find, &device));
    BluetoothFindDeviceClose(device_find);
  }
  CloseHandle(radio);
}

bool registry_dword(HKEY root, const wchar_t* path, const wchar_t* name, DWORD* value) {
  DWORD size = sizeof(DWORD);
  return RegGetValueW(root, path, name, RRF_RT_REG_DWORD, nullptr, value, &size) == ERROR_SUCCESS;
}

constexpr wchar_t kPersonalizeKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";

std::string read_appearance() {
  DWORD light = 1;
  registry_dword(HKEY_CURRENT_USER, kPersonalizeKey, L"AppsUseLightTheme", &light);
  return light != 0 ? "light" : "dark";
}

void toggle_appearance() {
  const DWORD light = read_appearance() == "dark" ? 1 : 0;
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kPersonalizeKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) return;
  RegSetValueExW(key, L"AppsUseLightTheme", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&light), sizeof light);
  RegSetValueExW(key, L"SystemUsesLightTheme", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&light),
                 sizeof light);
  RegCloseKey(key);
  // Apps (and Explorer) re-read the theme on this broadcast.
  DWORD_PTR result = 0;
  SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0,
                      reinterpret_cast<LPARAM>(L"ImmersiveColorSet"), SMTO_ABORTIFHUNG, 200, &result);
}

std::string layout_name(HKL layout) {
  const LANGID language = LOWORD(reinterpret_cast<std::uintptr_t>(layout));
  wchar_t locale[LOCALE_NAME_MAX_LENGTH] = {};
  if (LCIDToLocaleName(MAKELCID(language, SORT_DEFAULT), locale, LOCALE_NAME_MAX_LENGTH, 0) == 0) {
    return {};
  }
  wchar_t display[128] = {};
  if (GetLocaleInfoEx(locale, LOCALE_SLOCALIZEDDISPLAYNAME, display, 128) == 0) return narrow(locale);
  return narrow(display);
}

HKL foreground_layout() {
  return GetKeyboardLayout(GetWindowThreadProcessId(GetForegroundWindow(), nullptr));
}

std::vector<HKL> installed_layouts() {
  const int count = GetKeyboardLayoutList(0, nullptr);
  std::vector<HKL> layouts(static_cast<std::size_t>(std::max(count, 0)));
  if (count > 0) GetKeyboardLayoutList(count, layouts.data());
  return layouts;
}

// "Artist - Title" from the Spotify desktop client's window title, which is
// "Spotify"/"Spotify Premium" while paused. (The system media transport
// controls need WinRT; the title is a cheap, dependency-free stand-in.)
std::string spotify_now_playing() {
  struct Search {
    std::unordered_map<DWORD, bool> is_spotify;
    std::string title;
  } search;
  EnumWindows(
      [](HWND window, LPARAM parameter) -> BOOL {
        auto* state = reinterpret_cast<Search*>(parameter);
        if (!IsWindowVisible(window) && !IsIconic(window)) return TRUE;
        const DWORD pid = window_process(window);
        auto cached = state->is_spotify.find(pid);
        if (cached == state->is_spotify.end()) {
          const std::filesystem::path image(process_image_path(pid));
          std::wstring stem = image.stem().wstring();
          std::transform(stem.begin(), stem.end(), stem.begin(), ::towlower);
          cached = state->is_spotify.emplace(pid, stem == L"spotify").first;
        }
        if (!cached->second) return TRUE;
        const std::string title = narrow(window_title(window));
        if (title.find(" - ") == std::string::npos) return TRUE;
        state->title = title;
        return FALSE;
      },
      reinterpret_cast<LPARAM>(&search));
  return search.title;
}

void count_agents(core::SystemStatus& status) {
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return;
  int claude = 0;
  int codex = 0;
  PROCESSENTRY32W entry{};
  entry.dwSize = sizeof entry;
  if (Process32FirstW(snapshot, &entry)) {
    do {
      std::wstring name = entry.szExeFile;
      std::transform(name.begin(), name.end(), name.begin(), ::towlower);
      if (name == L"claude.exe") ++claude;
      if (name == L"codex.exe") ++codex;
    } while (Process32NextW(snapshot, &entry));
  }
  CloseHandle(snapshot);
  for (const auto& [kind, count] : {std::pair<const char*, int>{"claude", claude}, {"codex", codex}}) {
    if (count <= 0) continue;
    core::AgentInfo agent;
    agent.kind = kind;
    agent.status = "running";
    if (count > 1) agent.label = std::to_string(count) + " processes";
    status.agents.push_back(std::move(agent));
  }
}

// Picks an installed Nerd Font family for the chrome so widget icon glyphs
// render. MEPWM_ICON_FONT overrides ("none" disables icons entirely).
std::wstring detect_icon_font() {
  if (const char* env = std::getenv("MEPWM_ICON_FONT")) {
    return std::string(env) == "none" ? std::wstring() : widen(env);
  }
  std::wstring best;
  HDC dc = GetDC(nullptr);
  LOGFONTW query{};
  query.lfCharSet = DEFAULT_CHARSET;
  EnumFontFamiliesExW(
      dc, &query,
      [](const LOGFONTW* font, const TEXTMETRICW*, DWORD, LPARAM parameter) -> int {
        auto* current = reinterpret_cast<std::wstring*>(parameter);
        const std::wstring face = font->lfFaceName;
        std::wstring lowered = face;
        std::transform(lowered.begin(), lowered.end(), lowered.begin(), ::towlower);
        if (lowered.find(L"nerd font") == std::wstring::npos || face[0] == L'@') return 1;
        // Prefer the base variant over "... Mono" / "... Propo" siblings.
        if (current->empty() || face.size() < current->size()) *current = face;
        return 1;
      },
      reinterpret_cast<LPARAM>(&best), 0);
  ReleaseDC(nullptr, dc);
  return best;
}

HFONT make_font(const std::wstring& face, int pixel_height, int weight) {
  return CreateFontW(-pixel_height, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                     OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                     DEFAULT_PITCH | FF_DONTCARE, face.c_str());
}

// ---- the platform ------------------------------------------------------------

constexpr UINT kHotkeyMessage = WM_APP + 1;   // wparam = hotkey index
constexpr UINT kKeyMessage = WM_APP + 2;      // a captured key is queued
constexpr UINT kStopMessage = WM_APP + 3;     // console Ctrl+C
constexpr UINT_PTR kPollTimer = 1;            // 0.5s heartbeat
constexpr UINT_PTR kQuickPollTimer = 2;       // coalesced WinEvent reaction

// Picker geometry (matches the macOS picker).
constexpr int kPickerWidth = 560;
constexpr int kPickerRowHeight = 26;
constexpr int kPickerQueryHeight = 40;
constexpr std::size_t kPickerMaxRows = 10;
// Footer strip at the bottom of the side panel; the engine mirrors it.
constexpr int kSidePanelFooterHeight = 26;

class WindowsPlatform;
WindowsPlatform* g_platform = nullptr;  // for the hook and WinEvent callbacks
// Signaled once run() has finished cleaning up (hidden windows shown).
HANDLE g_exited = CreateEventW(nullptr, TRUE, FALSE, nullptr);

class WindowsPlatform final : public core::Platform {
 public:
  WindowsPlatform() = default;
  ~WindowsPlatform() override {
    stop_hook_thread();
    show_all_hidden();
    for (HWINEVENTHOOK hook : win_event_hooks_) UnhookWinEvent(hook);
    border_surfaces_.clear();
    tab_bar_surfaces_.clear();
    top_bar_.reset();
    bottom_bar_.reset();
    left_dock_.reset();
    right_dock_.reset();
    side_panel_.reset();
    picker_.reset();
    hints_surface_.reset();
    if (message_window_ != nullptr) DestroyWindow(message_window_);
    for (HFONT font : {chrome_font_, title_font_, picker_query_font_, picker_row_font_}) {
      if (font != nullptr) DeleteObject(font);
    }
    if (g_platform == this) g_platform = nullptr;
    if (com_initialized_) CoUninitialize();
  }
  WindowsPlatform(const WindowsPlatform&) = delete;
  WindowsPlatform& operator=(const WindowsPlatform&) = delete;
  WindowsPlatform(WindowsPlatform&&) = delete;
  WindowsPlatform& operator=(WindowsPlatform&&) = delete;

  bool initialize(core::Modifier primary, std::string* error) override {
    switch (primary) {
      case core::Modifier::Alt:
        primary_ = PrimaryKey::Alt;
        break;
      case core::Modifier::Ctrl:
        primary_ = PrimaryKey::Ctrl;
        break;
      case core::Modifier::Super:
      case core::Modifier::Cmd:  // the Windows key sits where Command does
        primary_ = PrimaryKey::Win;
        break;
      case core::Modifier::Fn:
      case core::Modifier::FnCmd:
        if (error != nullptr) {
          *error = "the fn modifiers only exist on macOS; use --modifier alt|win|ctrl";
        }
        return false;
    }
    g_platform = this;
    own_pid_ = GetCurrentProcessId();
    // Physical-pixel coordinates everywhere: other apps' frames, the work
    // area, and our chrome all agree regardless of per-monitor scaling.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    com_initialized_ = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));

    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW surface_class{};
    surface_class.cbSize = sizeof surface_class;
    surface_class.lpfnWndProc = &Surface::window_proc;
    surface_class.hInstance = instance;
    surface_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    surface_class.lpszClassName = kSurfaceClass;
    RegisterClassExW(&surface_class);

    WNDCLASSEXW message_class{};
    message_class.cbSize = sizeof message_class;
    message_class.lpfnWndProc = &WindowsPlatform::message_proc;
    message_class.hInstance = instance;
    message_class.lpszClassName = kMessageClass;
    RegisterClassExW(&message_class);
    // A hidden top-level window rather than HWND_MESSAGE: message-only
    // windows miss the WM_DISPLAYCHANGE/WM_SETTINGCHANGE broadcasts.
    message_window_ = CreateWindowExW(WS_EX_TOOLWINDOW, kMessageClass, L"mepwm", WS_POPUP, 0, 0, 0, 0,
                                      nullptr, nullptr, instance, nullptr);
    if (message_window_ == nullptr) {
      if (error != nullptr) *error = "could not create the event window";
      return false;
    }

    // Windows a previous run hid for its inactive workspaces and never got
    // to show again (it crashed or was killed): bring them back first.
    show_hidden_windows_from_file();
    SetUnhandledExceptionFilter(&show_hidden_windows_on_crash);

    // Window lifecycle and focus notifications: they only schedule a quick
    // re-poll, so the diffing logic stays in one place (poll()). Narrow
    // ranges on purpose: the span between them includes location and name
    // changes, which fire constantly.
    const std::pair<DWORD, DWORD> ranges[] = {
        {EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND},
        {EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND},
        {EVENT_OBJECT_DESTROY, EVENT_OBJECT_HIDE},
        {EVENT_OBJECT_CLOAKED, EVENT_OBJECT_UNCLOAKED},
    };
    for (const auto& [first, last] : ranges) {
      if (HWINEVENTHOOK hook = SetWinEventHook(first, last, nullptr, &WindowsPlatform::win_event_proc, 0, 0,
                                               WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS)) {
        win_event_hooks_.push_back(hook);
      }
    }

    SetConsoleCtrlHandler(&WindowsPlatform::console_handler, TRUE);

    icon_font_name_ = detect_icon_font();
    const std::wstring text_face = icon_font_name_.empty() ? L"Segoe UI" : icon_font_name_;
    chrome_font_ = make_font(text_face, 15, FW_MEDIUM);
    title_font_ = make_font(text_face, 17, FW_BOLD);
    picker_query_font_ = make_font(icon_font_name_.empty() ? L"Consolas" : icon_font_name_, 19, FW_SEMIBOLD);
    picker_row_font_ = make_font(icon_font_name_.empty() ? L"Consolas" : icon_font_name_, 16, FW_NORMAL);
    if (debug_) {
      std::fprintf(stderr, "mepwm: icon font: %s\n",
                   icon_font_name_.empty() ? "(none, text fallback)" : narrow(icon_font_name_).c_str());
    }
    return true;
  }

  bool has_icon_font() override { return !icon_font_name_.empty(); }

  core::Rect work_area() override {
    RECT area{};
    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &area, 0)) {
      area = RECT{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    }
    return {area.left, area.top, area.right - area.left, area.bottom - area.top};
  }

  std::vector<core::WindowInfo> list_windows() override {
    struct Enumeration {
      WindowsPlatform* self;
      std::vector<core::WindowInfo> windows;
    } enumeration{this, {}};
    EnumWindows(
        [](HWND window, LPARAM parameter) -> BOOL {
          auto* state = reinterpret_cast<Enumeration*>(parameter);
          if (!is_manageable(window, state->self->own_pid_)) return TRUE;
          core::WindowInfo info;
          info.id = to_id(window);
          info.title = narrow(window_title(window));
          info.application = state->self->application_name(window, info.title);
          const RECT frame = visible_frame(window);
          info.frame = {frame.left, frame.top, frame.right - frame.left, frame.bottom - frame.top};
          state->windows.push_back(std::move(info));
          return TRUE;
        },
        reinterpret_cast<LPARAM>(&enumeration));
    // Windows hidden for inactive workspaces still exist as far as the
    // engine is concerned; drop the ones that were closed meanwhile.
    bool changed = false;
    for (auto it = hidden_.begin(); it != hidden_.end();) {
      HWND window = *it;
      if (!IsWindow(window)) {
        it = hidden_.erase(it);
        changed = true;
        continue;
      }
      ++it;
      const core::WindowId id = to_id(window);
      if (std::any_of(enumeration.windows.begin(), enumeration.windows.end(),
                      [id](const core::WindowInfo& info) { return info.id == id; })) {
        continue;  // the app showed it again by itself
      }
      core::WindowInfo info;
      info.id = id;
      info.title = narrow(window_title(window));
      info.application = application_name(window, info.title);
      const RECT frame = visible_frame(window);
      info.frame = {frame.left, frame.top, frame.right - frame.left, frame.bottom - frame.top};
      enumeration.windows.push_back(std::move(info));
    }
    if (changed) save_hidden_windows(hidden_);
    return enumeration.windows;
  }

  bool hide_window(core::WindowId window_id) override {
    HWND window = to_hwnd(window_id);
    if (!IsWindow(window)) return true;
    hidden_.insert(window);
    save_hidden_windows(hidden_);
    ShowWindowAsync(window, SW_HIDE);
    return true;
  }

  core::WindowId focused_window() override {
    HWND window = GetForegroundWindow();
    return window == nullptr ? core::kNoWindow : to_id(window);
  }

  void set_window_frame(core::WindowId window_id, const core::Rect& frame) override {
    HWND window = to_hwnd(window_id);
    if (!IsWindow(window)) return;
    unhide(window);
    // A maximized window ignores SetWindowPos sizing; restore it first. The
    // Async variants post to the owning thread, so a hung app can't stall
    // mepwm, and the restore still lands before the move.
    if (IsZoomed(window)) ShowWindowAsync(window, SW_RESTORE);
    // Grow the requested (visible) frame by the invisible resize borders.
    RECT inset{};
    RECT outer{};
    RECT visible{};
    const bool zoomed = IsZoomed(window) != FALSE;
    if (!zoomed && IsWindowVisible(window) && GetWindowRect(window, &outer) &&
        SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &visible, sizeof visible))) {
      inset = RECT{visible.left - outer.left, visible.top - outer.top, outer.right - visible.right,
                   outer.bottom - visible.bottom};
      insets_[window] = inset;
    } else if (const auto cached = insets_.find(window); cached != insets_.end()) {
      inset = cached->second;
    } else if (zoomed) {
      // The restore is still in flight; assume the standard resize border
      // (Windows 10/11 have no invisible border at the top).
      const int border = GetSystemMetrics(SM_CXSIZEFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
      inset = RECT{border, 0, border, border};
    }
    const int left = inset.left;
    const int top = inset.top;
    const int right = inset.right;
    const int bottom = inset.bottom;
    SetWindowPos(window, nullptr, frame.x - left, frame.y - top, frame.width + left + right,
                 frame.height + top + bottom,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_ASYNCWINDOWPOS);
  }

  void focus_window(core::WindowId window_id) override {
    HWND window = to_hwnd(window_id);
    if (!IsWindow(window)) return;
    unhide(window);
    if (IsIconic(window)) ShowWindowAsync(window, SW_RESTORE);
    claim_foreground_rights();
    if (!SetForegroundWindow(window)) {
      // Fallback: borrow the foreground thread's input state.
      const DWORD foreground_thread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
      const DWORD own_thread = GetCurrentThreadId();
      if (foreground_thread != 0 && foreground_thread != own_thread &&
          AttachThreadInput(own_thread, foreground_thread, TRUE)) {
        SetForegroundWindow(window);
        BringWindowToTop(window);
        AttachThreadInput(own_thread, foreground_thread, FALSE);
      }
    }
  }

  void raise_window(core::WindowId window_id) override {
    HWND window = to_hwnd(window_id);
    if (!IsWindow(window)) return;
    unhide(window);
    SetWindowPos(window, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
  }

  bool register_hotkey(const core::Hotkey& hotkey, core::Action action, int argument) override {
    // Alt+Tab / Alt+Shift+Tab are the system window switcher; with Alt as
    // the primary modifier the tab-cycling bindings are skipped rather than
    // breaking it (Mod+n still cycles tabs).
    if (primary_ == PrimaryKey::Alt && hotkey.key == core::Key::Tab &&
        (hotkey.modifiers & core::kModPrimary) != 0U) {
      return true;
    }
    HotkeyEntry entry;
    entry.virtual_key = virtual_key(hotkey.key);
    entry.primary = (hotkey.modifiers & core::kModPrimary) != 0U;
    entry.shift = (hotkey.modifiers & core::kModShift) != 0U;
    entry.ctrl = (hotkey.modifiers & core::kModCtrl) != 0U;
    entry.action = action;
    entry.argument = argument;
    hotkeys_.push_back(entry);
    return true;
  }

  void update_borders(const std::vector<core::Border>& borders) override {
    if (fullscreen_) return;  // redrawn by the retile when fullscreen ends
    while (border_surfaces_.size() > borders.size()) border_surfaces_.pop_back();
    while (border_surfaces_.size() < borders.size()) {
      border_surfaces_.push_back(std::make_unique<Surface>(Surface::Kind::Ring, 255));
    }
    for (std::size_t index = 0; index < borders.size(); ++index) {
      const core::Border& border = borders[index];
      const int width = static_cast<int>(border.width);
      Surface& surface = *border_surfaces_[index];
      surface.set_ring_width(width);
      surface.set_background(color_from_hex(border.color));
      // The ring sits just outside the managed window's frame, in the gap.
      surface.show(RECT{border.frame.x - width, border.frame.y - width,
                        border.frame.x + border.frame.width + width,
                        border.frame.y + border.frame.height + width});
    }
  }

  void update_chrome(const core::Chrome& chrome) override {
    if (top_bar_ == nullptr) {
      top_bar_ = make_panel(245, core::ChromeArea::TopBar);
      bottom_bar_ = make_panel(245, core::ChromeArea::BottomBar);
      left_dock_ = make_panel(245, core::ChromeArea::LeftDock);
      right_dock_ = make_panel(245, core::ChromeArea::RightDock);
    }
    last_chrome_ = chrome;
    if (fullscreen_) return;
    if (!chrome.visible) {
      for (Surface* surface : {top_bar_.get(), bottom_bar_.get(), left_dock_.get(), right_dock_.get()}) {
        surface->hide();
      }
      return;
    }
    const COLORREF background = color_from_hex(chrome.background_color);
    const COLORREF foreground = color_from_hex(chrome.text_color);
    const COLORREF accent = color_from_hex(chrome.accent_color);
    const COLORREF highlight_text = RGB(255, 255, 255);

    // Top bar: label, workspace cells, centered title, clock.
    {
      std::vector<DrawItem>& items = top_bar_->items();
      items.clear();
      const int width = chrome.top.width;
      const int height = chrome.top.height;
      items.push_back(text_item(RECT{10, 0, 62, height}, chrome.top_left, accent, DT_LEFT));
      // Cells size to their text; whatever would run under the clock is
      // dropped rather than clipped mid-bar.
      const int cells_end = width - 180;
      int cell_x = 70;
      const int cell_top = (height - 20) / 2;
      for (const core::BarCell& cell : chrome.top_cells) {
        const int cell_width = std::max(22, text_width(chrome_font_, cell.text) + 12);
        if (cell_x + cell_width > cells_end) break;
        DrawItem item = text_item(RECT{cell_x, cell_top, cell_x + cell_width, cell_top + 20}, cell.text,
                                  cell.highlight ? highlight_text : foreground, DT_CENTER);
        item.filled = cell.highlight;
        item.fill = accent;
        item.radius = 3;
        item.id = cell.id;
        item.area = core::ChromeArea::TopBar;
        items.push_back(std::move(item));
        cell_x += cell_width + 4;
      }
      const int center_start = std::max(cell_x + 12, 180);
      items.push_back(text_item(RECT{center_start, 0, std::max(width - 180, center_start + 50), height},
                                chrome.top_center, foreground, DT_CENTER));
      items.push_back(text_item(RECT{width - 170, 0, width - 10, height}, chrome.top_right, foreground,
                                DT_RIGHT));
      top_bar_->set_background(background);
      top_bar_->show(to_rect(chrome.top));
    }

    // Bottom bar: system widgets right-aligned, pills left, text between.
    {
      std::vector<DrawItem>& items = bottom_bar_->items();
      items.clear();
      const int height = chrome.bottom.height;
      const int widget_top = (height - 20) / 2;
      int right_edge = chrome.bottom.width - 10;
      for (std::size_t index = chrome.bottom_widgets.size(); index-- > 0;) {
        const core::BarCell& cell = chrome.bottom_widgets[index];
        const int cell_width = text_width(chrome_font_, cell.text) + 10;
        DrawItem item = text_item(RECT{right_edge - cell_width, widget_top, right_edge, widget_top + 20},
                                  cell.text, cell.highlight ? highlight_text : foreground, DT_CENTER);
        item.filled = cell.highlight;
        item.fill = accent;
        item.radius = 3;
        item.id = cell.id;
        item.area = core::ChromeArea::BottomBar;
        items.push_back(std::move(item));
        right_edge -= cell_width + 10;
      }
      int left_edge = 10;
      for (const core::BarCell& cell : chrome.bottom_left_widgets) {
        // A cell with a color is a filled pill (text in the bar background
        // color); a colorless cell is plain bar text (the OS icon).
        const bool pill = !cell.color.empty();
        const int cell_width = text_width(chrome_font_, cell.text) + 14;
        DrawItem item = text_item(RECT{left_edge, widget_top, left_edge + cell_width, widget_top + 20},
                                  cell.text, pill ? background : foreground, DT_CENTER);
        item.filled = pill;
        item.fill = pill ? color_from_hex(cell.color) : background;
        item.radius = 4;
        item.id = cell.id;
        item.area = core::ChromeArea::BottomBar;
        items.push_back(std::move(item));
        left_edge += cell_width + 10;
      }
      items.push_back(text_item(RECT{left_edge, 0, std::max(right_edge - 10, left_edge + 50), height},
                                chrome.bottom_text, foreground, DT_CENTER));
      bottom_bar_->set_background(background);
      bottom_bar_->show(to_rect(chrome.bottom));
    }

    layout_dock(*left_dock_, chrome.left_cells, chrome.left, core::ChromeArea::LeftDock, background,
                foreground, accent, /*center_vertically=*/true);
    layout_dock(*right_dock_, chrome.right_cells, chrome.right, core::ChromeArea::RightDock, background,
                foreground, accent, /*center_vertically=*/false);
    keep_overlays_on_top();
  }

  core::SystemStatus system_status() override {
    core::SystemStatus status;
    read_battery(status);
    read_cpu(status);
    read_memory(status);
    read_disk(status);
    read_wifi(status);
    read_bluetooth(status);
    bool muted = false;
    status.volume_percent = read_volume(eRender, &muted);
    status.volume_muted = muted;
    status.input_volume_percent = read_volume(eCapture, nullptr);
    status.media_title = spotify_now_playing();

    const HKL current = foreground_layout();
    status.keyboard_layout = layout_name(current);
    layouts_.clear();
    for (HKL layout : installed_layouts()) {
      const std::string name = layout_name(layout);
      if (name.empty() || layouts_.count(name) != 0) continue;
      layouts_[name] = layout;
      status.keyboard_layouts.push_back({name, name == status.keyboard_layout, ""});
    }
    status.appearance = read_appearance();
    count_agents(status);

    // Git state of the directory mepwm was launched from. The probes run on
    // the UI thread, so a repo slow enough to time out (a dotfiles repo in
    // a big home folder) is left alone for a minute instead of stalling
    // every refresh; -uno skips the untracked-file scan that makes it slow.
    if (GetTickCount64() >= git_retry_at_) {
      bool timed_out = false;
      status.git_branch = capture_command("git rev-parse --abbrev-ref HEAD 2>nul", &timed_out);
      if (!status.git_branch.empty() && !timed_out) {
        status.git_status_lines =
            capture_command_lines("git status --porcelain -uno 2>nul", 20, 1500, &timed_out);
        status.git_dirty = static_cast<int>(status.git_status_lines.size());
      }
      if (timed_out) git_retry_at_ = GetTickCount64() + 60000;
    }
    return status;
  }

  void perform(core::SystemAction action, int value, const std::string& argument) override {
    if (debug_) std::fprintf(stderr, "mepwm: perform action=%d value=%d\n", static_cast<int>(action), value);
    switch (action) {
      case core::SystemAction::SetOutputVolume:
        write_volume(eRender, value);
        break;
      case core::SystemAction::SetInputVolume:
        write_volume(eCapture, value);
        break;
      case core::SystemAction::SetOutputMuted:
        write_mute(value != 0);
        break;
      case core::SystemAction::ToggleAppearance:
        toggle_appearance();
        break;
      case core::SystemAction::SelectKeyboardLayout: {
        const auto found = layouts_.find(argument);
        if (found != layouts_.end()) {
          PostMessageW(GetForegroundWindow(), WM_INPUTLANGCHANGEREQUEST, 0,
                       reinterpret_cast<LPARAM>(found->second));
        }
        break;
      }
      case core::SystemAction::SetWallpaper: {
        std::wstring path = std::filesystem::absolute(std::filesystem::path(widen(argument))).wstring();
        if (!SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, path.data(),
                                   SPIF_UPDATEINIFILE | SPIF_SENDCHANGE) &&
            debug_) {
          std::fprintf(stderr, "mepwm: could not set wallpaper %s\n", argument.c_str());
        }
        break;
      }
      case core::SystemAction::MediaPrevious:
        send_key_tap(VK_MEDIA_PREV_TRACK);
        break;
      case core::SystemAction::MediaPlayPause:
        send_key_tap(VK_MEDIA_PLAY_PAUSE);
        break;
      case core::SystemAction::MediaNext:
        send_key_tap(VK_MEDIA_NEXT_TRACK);
        break;
    }
  }

  void update_side_panel(const core::SidePanel& panel) override {
    const bool was_capturing = last_panel_.visible && last_panel_.wants_keys;
    last_panel_ = panel;
    if (!panel.visible) {
      if (side_panel_ != nullptr) side_panel_->hide();
      panel_keys_ = false;
      return;
    }
    if (side_panel_ == nullptr) {
      side_panel_ = make_panel(250, core::ChromeArea::SidePanel);
      // Clicking the panel hands it the keyboard again after the user
      // clicked away into another app.
      side_panel_->set_on_any_click([this] {
        if (last_panel_.visible && last_panel_.wants_keys) {
          panel_keys_ = true;
          panel_focus_snapshot_ = GetForegroundWindow();
        }
      });
    }
    // The overlay analogue of the macOS key panel: while a wants_keys panel
    // is open, the keyboard hook routes typing to it -- until the user
    // focuses another window.
    if (panel.wants_keys && !was_capturing) {
      panel_keys_ = true;
      panel_focus_snapshot_ = GetForegroundWindow();
    }
    if (!panel.wants_keys) panel_keys_ = false;

    const COLORREF background = color_from_hex(last_chrome_.background_color);
    const COLORREF foreground = color_from_hex(last_chrome_.text_color);
    const COLORREF accent = color_from_hex(last_chrome_.accent_color);
    const COLORREF rule = blend(background, foreground, 0.25);
    const int width = panel.frame.width;
    const int height = panel.frame.height;

    std::vector<DrawItem>& items = side_panel_->items();
    items.clear();
    side_panel_->clear_rules();
    DrawItem title = text_item(RECT{12, 12, width - 12, 30}, panel.title, accent, DT_LEFT);
    title.font = title_font_;
    items.push_back(std::move(title));
    side_panel_->set_rule(36, rule);

    const int footer_height = panel.footer.empty() ? 0 : kSidePanelFooterHeight;
    if (footer_height > 0) {
      side_panel_->set_rule(height - footer_height, rule);
      items.push_back(text_item(RECT{12, height - footer_height, width - 12, height}, panel.footer,
                                foreground, DT_CENTER));
    }
    constexpr int kLineHeight = 18;
    for (std::size_t index = 0; index < panel.lines.size(); ++index) {
      const core::BarCell& line = panel.lines[index];
      const int top = 40 + static_cast<int>(index) * kLineHeight;
      DrawItem item = text_item(RECT{8, top, width - 8, top + kLineHeight}, line.text,
                                line.highlight ? RGB(255, 255, 255) : foreground, DT_LEFT);
      item.filled = line.highlight;
      item.fill = accent;
      item.radius = 3;
      item.id = line.id;
      item.area = core::ChromeArea::SidePanel;
      // Rows that would run into the footer strip are clipped out.
      item.hidden = top + kLineHeight > height - footer_height - 2;
      items.push_back(std::move(item));
    }
    side_panel_->set_background(background);
    side_panel_->show(to_rect(panel.frame), !side_panel_->visible());
    keep_overlays_on_top();
  }

  void update_tab_bars(const std::vector<core::TabBar>& bars) override {
    last_tab_bars_ = bars;
    if (fullscreen_) return;
    while (tab_bar_surfaces_.size() > bars.size()) tab_bar_surfaces_.pop_back();
    while (tab_bar_surfaces_.size() < bars.size()) {
      tab_bar_surfaces_.push_back(make_panel(245, core::ChromeArea::TabBar));
    }
    const COLORREF background = color_from_hex(last_chrome_.background_color);
    const COLORREF foreground = color_from_hex(last_chrome_.text_color);
    const COLORREF accent = color_from_hex(last_chrome_.accent_color);
    for (std::size_t index = 0; index < bars.size(); ++index) {
      const core::TabBar& bar = bars[index];
      Surface& surface = *tab_bar_surfaces_[index];
      std::vector<DrawItem>& items = surface.items();
      items.clear();
      const int count = std::max(1, static_cast<int>(bar.tabs.size()));
      constexpr int kGap = 2;
      constexpr int kInset = 4;
      const int cell_width = (bar.frame.width - 2 * kInset - kGap * (count - 1)) / count;
      const int cell_top = (bar.frame.height - 18) / 2;
      for (std::size_t tab = 0; tab < bar.tabs.size(); ++tab) {
        const core::BarCell& cell = bar.tabs[tab];
        const int left = kInset + static_cast<int>(tab) * (cell_width + kGap);
        DrawItem item = text_item(RECT{left, cell_top, left + cell_width, cell_top + 18}, cell.text,
                                  cell.highlight ? RGB(255, 255, 255) : foreground, DT_CENTER);
        item.filled = cell.highlight;
        item.fill = accent;
        item.radius = 3;
        item.id = cell.id;
        item.area = core::ChromeArea::TabBar;
        items.push_back(std::move(item));
      }
      surface.set_background(background);
      surface.show(to_rect(bar.frame));
    }
    keep_overlays_on_top();
  }

  void spawn(const std::string& shell_command) override {
    if (debug_) std::fprintf(stderr, "mepwm: spawn: %s\n", shell_command.c_str());
    // cmd.exe without a console of its own: `start` gives console programs
    // (cmd, pwsh) their own window, GUI programs need none.
    std::wstring line = L"cmd.exe /d /s /c \"" + widen(shell_command) + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof startup;
    PROCESS_INFORMATION process{};
    // Otherwise a spawned terminal opens behind the current window.
    claim_foreground_rights();
    AllowSetForegroundWindow(ASFW_ANY);
    if (CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP, nullptr, nullptr, &startup,
                       &process)) {
      CloseHandle(process.hProcess);
      CloseHandle(process.hThread);
    } else if (debug_) {
      std::fprintf(stderr, "mepwm: spawn failed (%lu)\n", GetLastError());
    }
  }

  void toggle_app_picker() override {
    if (picker_visible_ && picker_kind_ == PickerKind::Apps) {
      close_picker();
      return;
    }
    // Replacing a showing list picker counts as cancelling it.
    if (picker_visible_) notify_picker_event(core::Event::Type::PickerCancelled);
    picker_kind_ = PickerKind::Apps;
    picker_title_ = "Run";
    picker_items_ = scan_applications();
    open_picker();
  }

  void show_list_picker(const std::string& title, const std::vector<core::PickerItem>& items,
                        const std::string& selected_id) override {
    if (picker_visible_ && picker_kind_ == PickerKind::List && picker_title_ == title) {
      close_picker();
      notify_picker_event(core::Event::Type::PickerCancelled);
      return;
    }
    picker_kind_ = PickerKind::List;
    picker_title_ = title;
    picker_items_ = items;
    picker_initial_id_ = selected_id;
    open_picker();
  }

  // ---- Vimium-style click hints (the X11 backend's Super+f overlay) ----

  void toggle_hints() override {
    if (hints_visible_) {
      close_hints();
      return;
    }
    if (picker_visible_) return;
    build_hints();
    if (hints_.empty()) return;
    if (hints_surface_ == nullptr) hints_surface_ = std::make_unique<Surface>(Surface::Kind::Overlay, 255);
    const RECT screen{GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
                      GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN),
                      GetSystemMetrics(SM_YVIRTUALSCREEN) + GetSystemMetrics(SM_CYVIRTUALSCREEN)};
    const COLORREF chip = color_from_hex(last_chrome_.accent_color);
    const COLORREF text = color_from_hex(last_chrome_.background_color);
    std::vector<DrawItem>& items = hints_surface_->items();
    items.clear();
    for (const HintTarget& hint : hints_) {
      const int width = std::max(18, text_width(chrome_font_, hint.label) + 10);
      constexpr int kChipHeight = 18;
      const int left = hint.frame.left - screen.left;
      const int top = hint.frame.top - screen.top;
      DrawItem item = text_item(RECT{left, top, left + width, top + kChipHeight}, hint.label, text, DT_CENTER);
      item.filled = true;
      item.fill = chip;
      item.radius = 3;
      items.push_back(std::move(item));
    }
    hint_query_.clear();
    hints_visible_ = true;
    hints_surface_->show(screen, /*raise=*/true);
  }

  void run(const std::function<void(const core::Event&)>& on_event) override {
    handler_ = on_event;
    // Started here rather than in initialize(): the engine registers its
    // hotkeys in between, and the hook thread reads them unsynchronized.
    start_hook_thread();
    previous_ids_ = snapshot_ids();
    previous_focus_ = focused_window();
    previous_area_ = work_area();
    SetTimer(message_window_, kPollTimer, 500, nullptr);
    running_ = true;
    MSG message;
    while (running_ && GetMessageW(&message, nullptr, 0, 0) > 0) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
    KillTimer(message_window_, kPollTimer);
    stop_hook_thread();
    handler_ = nullptr;
    show_all_hidden();
    if (g_exited != nullptr) SetEvent(g_exited);
  }

  void stop() override {
    running_ = false;
    PostQuitMessage(0);
  }

 private:
  enum class PrimaryKey { Alt, Win, Ctrl };
  enum class PickerKind { Apps, List };

  struct HotkeyEntry {
    UINT virtual_key = 0;
    bool primary = true;
    bool shift = false;
    bool ctrl = false;
    core::Action action = core::Action::FocusLeft;
    int argument = 0;
  };

  struct KeyPress {
    DWORD virtual_key = 0;
    bool ctrl = false;
    std::string text;  // printable input (UTF-8), empty for special keys
  };

  struct HintTarget {
    std::string label;  // letters to type
    RECT frame{};       // target cell, screen coordinates
    core::ChromeArea area = core::ChromeArea::BottomBar;
    std::string cell;   // BarCell id to report
  };

  std::unique_ptr<Surface> make_panel(int alpha, core::ChromeArea area) {
    auto surface = std::make_unique<Surface>(Surface::Kind::Panel, alpha);
    surface->set_on_click([this, area](const DrawItem& item) {
      if (handler_ == nullptr || item.id.empty()) return;
      if (debug_) std::fprintf(stderr, "mepwm: chrome click: %s\n", item.id.c_str());
      core::Event event;
      event.type = core::Event::Type::ChromeClicked;
      event.area = area;
      event.cell = item.id;
      dispatch(event);
    });
    return surface;
  }

  DrawItem text_item(const RECT& rect, const std::string& text, COLORREF color, UINT align) const {
    DrawItem item;
    item.rect = rect;
    item.text = widen(text);
    item.text_color = color;
    item.align = align;
    item.font = chrome_font_;
    return item;
  }

  int text_width(HFONT font, const std::string& text) const {
    if (text.empty()) return 0;
    const std::wstring wide = widen(text);
    HDC dc = GetDC(nullptr);
    HGDIOBJ previous = SelectObject(dc, font);
    SIZE size{};
    GetTextExtentPoint32W(dc, wide.c_str(), static_cast<int>(wide.size()), &size);
    SelectObject(dc, previous);
    ReleaseDC(nullptr, dc);
    return size.cx;
  }

  // Vertical stack of cells inside a dock; highlighted cells get the accent
  // as background.
  void layout_dock(Surface& dock, const std::vector<core::BarCell>& cells, const core::Rect& panel,
                   core::ChromeArea area, COLORREF background, COLORREF foreground, COLORREF accent,
                   bool center_vertically) {
    std::vector<DrawItem>& items = dock.items();
    items.clear();
    constexpr int kCellHeight = 24;
    constexpr int kCellGap = 4;
    // The launcher column sits vertically centered in its dock (X11's
    // left_dock_top_offset); the right dock's toggles stay top-aligned.
    const int column_height = static_cast<int>(cells.size()) * (kCellHeight + kCellGap) + kCellGap;
    const int offset = center_vertically ? std::max(0, (panel.height - column_height) / 2) : 0;
    for (std::size_t index = 0; index < cells.size(); ++index) {
      const int top = offset + kCellGap + static_cast<int>(index) * (kCellHeight + kCellGap);
      DrawItem item = text_item(RECT{4, top, panel.width - 4, top + kCellHeight}, cells[index].text,
                                cells[index].highlight ? RGB(255, 255, 255) : foreground, DT_CENTER);
      item.filled = cells[index].highlight;
      item.fill = accent;
      item.radius = 3;
      item.id = cells[index].id;
      item.area = area;
      items.push_back(std::move(item));
    }
    dock.set_background(background);
    dock.show(to_rect(panel));
  }

  // Bars repaint every tick; the picker and hint overlay must stay above
  // them (and above the side panel).
  void keep_overlays_on_top() {
    if (picker_visible_ && picker_ != nullptr) {
      SetWindowPos(picker_->hwnd(), HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    if (hints_visible_ && hints_surface_ != nullptr) {
      SetWindowPos(hints_surface_->hwnd(), HWND_TOPMOST, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
  }

  std::string application_name(HWND window, const std::string& title) {
    const DWORD pid = window_process(window);
    auto cached = app_names_.find(pid);
    if (cached == app_names_.end()) {
      std::string name = narrow(std::filesystem::path(process_image_path(pid)).stem().wstring());
      cached = app_names_.emplace(pid, std::move(name)).first;
    }
    // UWP apps all run inside ApplicationFrameHost; their title is the
    // better name.
    if (cached->second == "ApplicationFrameHost") return title;
    return cached->second;
  }

  // ---- event plumbing ----

  // Delivers an engine event, deferring it if one is already being handled
  // (a Win32 call inside the handler can pump sent messages and re-enter).
  void dispatch(const core::Event& event) {
    if (handler_ == nullptr) return;
    if (in_handler_) {
      deferred_.push_back(event);
      return;
    }
    in_handler_ = true;
    handler_(event);
    while (!deferred_.empty() && handler_ != nullptr) {
      const core::Event next = deferred_.front();
      deferred_.pop_front();
      handler_(next);
    }
    in_handler_ = false;
  }

  std::vector<core::WindowId> snapshot_ids() {
    std::vector<core::WindowId> ids;
    for (const core::WindowInfo& info : list_windows()) ids.push_back(info.id);
    std::sort(ids.begin(), ids.end());
    return ids;
  }

  void poll(bool tick) {
    if (handler_ == nullptr) return;
    std::vector<core::WindowId> ids = snapshot_ids();
    if (ids != previous_ids_) {
      previous_ids_ = std::move(ids);
      core::Event event;
      event.type = core::Event::Type::WindowsChanged;
      dispatch(event);
    }
    const core::WindowId focus = focused_window();
    if (focus != previous_focus_) {
      previous_focus_ = focus;
      // Focus moved to another app: the overlays that borrowed the keyboard
      // give it back (the macOS panels lose key status the same way).
      if (picker_visible_) {
        close_picker();
        notify_picker_event(core::Event::Type::PickerCancelled);
      }
      if (hints_visible_) close_hints();
      if (panel_keys_ && to_hwnd(focus) != panel_focus_snapshot_) panel_keys_ = false;
      core::Event event;
      event.type = core::Event::Type::FocusChanged;
      event.window = focus;
      dispatch(event);
    }
    // A fullscreen app in front (F11, video, game, slideshow) must not have
    // topmost chrome painted over it: hide the always-on-top surfaces while
    // it is up, and retile (which redraws them) once it is gone.
    const bool fullscreen = is_fullscreen(GetForegroundWindow());
    if (fullscreen != fullscreen_) {
      fullscreen_ = fullscreen;
      if (fullscreen) {
        for (Surface* surface : {top_bar_.get(), bottom_bar_.get(), left_dock_.get(), right_dock_.get()}) {
          if (surface != nullptr) surface->hide();
        }
        for (const auto& surface : tab_bar_surfaces_) surface->hide();
        for (const auto& surface : border_surfaces_) surface->hide();
      } else {
        core::Event event;
        event.type = core::Event::Type::ScreenChanged;
        dispatch(event);
      }
    }
    const core::Rect area = work_area();
    if (area.x != previous_area_.x || area.y != previous_area_.y || area.width != previous_area_.width ||
        area.height != previous_area_.height) {
      previous_area_ = area;
      core::Event event;
      event.type = core::Event::Type::ScreenChanged;
      dispatch(event);
    }
    if (tick) {
      core::Event event;
      event.type = core::Event::Type::Tick;
      dispatch(event);
    }
  }

  void unhide(HWND window) {
    if (hidden_.erase(window) == 0) return;
    save_hidden_windows(hidden_);
    ShowWindowAsync(window, SW_SHOWNOACTIVATE);
  }

  // On exit nothing may stay hidden: those windows would have no taskbar
  // button or Alt+Tab entry to get them back.
  void show_all_hidden() {
    for (HWND window : hidden_) {
      if (IsWindow(window)) ShowWindowAsync(window, SW_SHOWNOACTIVATE);
    }
    hidden_.clear();
    save_hidden_windows(hidden_);
  }

  // The low-level keyboard hook runs on its own thread with its own message
  // loop. Windows silently uninstalls a hook that keeps missing its timeout,
  // and the UI thread can be busy (status probes run subprocesses), so the
  // hook must never wait on it: it only matches keys and posts messages.
  void start_hook_thread() {
    // GetKeyState on the hook thread never sees toggles, so the hook tracks
    // Caps Lock itself, seeded from the UI thread's view.
    caps_lock_ = (GetKeyState(VK_CAPITAL) & 1) != 0;
    std::promise<DWORD> started;
    std::future<DWORD> thread_id = started.get_future();
    hook_thread_ = std::thread([&started] {
      MSG message;
      PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);  // creates the queue
      SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
      HHOOK hook = SetWindowsHookExW(WH_KEYBOARD_LL, &WindowsPlatform::keyboard_proc,
                                     GetModuleHandleW(nullptr), 0);
      started.set_value(hook != nullptr ? GetCurrentThreadId() : 0);
      if (hook == nullptr) return;
      while (GetMessageW(&message, nullptr, 0, 0) > 0) DispatchMessageW(&message);
      UnhookWindowsHookEx(hook);
    });
    hook_thread_id_ = thread_id.get();
    if (hook_thread_id_ == 0) {
      hook_thread_.join();
      throw std::runtime_error("could not install the keyboard hook");
    }
  }

  void stop_hook_thread() {
    if (!hook_thread_.joinable()) return;
    PostThreadMessageW(hook_thread_id_, WM_QUIT, 0, 0);
    hook_thread_.join();
  }

  bool capturing_keys() const { return picker_visible_ || hints_visible_ || panel_keys_; }

  // Runs on the hook thread (see start_hook_thread): it must return fast and
  // touch only state that is immutable while running (hotkeys_, primary_) or
  // atomic (the capture flags); the engine runs from the UI message loop.
  LRESULT on_keyboard(WPARAM message, const KBDLLHOOKSTRUCT& key) {
    if ((key.flags & LLKHF_INJECTED) != 0 && key.dwExtraInfo == kInjectedTag) return 0;
    if (message != WM_KEYDOWN && message != WM_SYSKEYDOWN) return 0;
    if (key.vkCode == VK_CAPITAL) caps_lock_ = !caps_lock_;
    if (is_modifier_key(key.vkCode)) return 0;
    // AltGr arrives as LCtrl+RAlt: a character key, never a Mod chord
    // (AltGr+l is "l-stroke" on Polish layouts, not swap-right).
    const bool alt_gr = key_down(VK_RMENU) && key_down(VK_LCONTROL);
    const bool alt = key_down(VK_MENU) && !alt_gr;
    const bool win = key_down(VK_LWIN) || key_down(VK_RWIN);
    const bool ctrl = key_down(VK_CONTROL) && !alt_gr;
    const bool shift = key_down(VK_SHIFT);
    bool primary = false;
    bool ctrl_free = ctrl;  // Ctrl beyond the primary, when Ctrl is primary
    switch (primary_) {
      case PrimaryKey::Alt:
        primary = alt && !win;
        break;
      case PrimaryKey::Win:
        primary = win && !alt;
        break;
      case PrimaryKey::Ctrl:
        primary = ctrl && !alt && !win;
        ctrl_free = false;
        break;
    }
    if (primary) {
      for (std::size_t index = 0; index < hotkeys_.size(); ++index) {
        const HotkeyEntry& entry = hotkeys_[index];
        if (entry.virtual_key != key.vkCode || entry.shift != shift || entry.ctrl != ctrl_free) continue;
        if (primary_ != PrimaryKey::Ctrl) send_key_tap(kMenuMaskKey);
        PostMessageW(message_window_, kHotkeyMessage, index, 0);
        return 1;
      }
    }
    if (!capturing_keys() || win || alt) return 0;
    KeyPress press;
    press.virtual_key = key.vkCode;
    press.ctrl = ctrl;
    if (press.ctrl) {
      if (key.vkCode >= 'A' && key.vkCode <= 'Z') {
        press.text.push_back(static_cast<char>('a' + (key.vkCode - 'A')));
      }
    } else {
      BYTE state[256] = {};
      if (shift) state[VK_SHIFT] = 0x80;
      if (caps_lock_) state[VK_CAPITAL] = 0x01;
      if (alt_gr) {
        state[VK_CONTROL] = 0x80;
        state[VK_MENU] = 0x80;
      }
      wchar_t buffer[8] = {};
      // Flag 0x4: leave the kernel's dead-key state alone.
      const int count = ToUnicodeEx(key.vkCode, key.scanCode, state, buffer, 8, 0x4, foreground_layout());
      if (count > 0) {
        std::wstring typed;
        for (int index = 0; index < count; ++index) {
          if (buffer[index] >= 0x20 && buffer[index] != 0x7F) typed.push_back(buffer[index]);
        }
        press.text = narrow(typed);
      }
    }
    {
      const std::lock_guard<std::mutex> lock(key_queue_mutex_);
      key_queue_.push_back(std::move(press));
    }
    PostMessageW(message_window_, kKeyMessage, 0, 0);
    return 1;
  }

  void handle_key(const KeyPress& press) {
    if (picker_visible_) {
      picker_key(press);
    } else if (hints_visible_) {
      hint_key(press);
    } else if (panel_keys_) {
      panel_key(press);
    }
  }

  // ---- side panel keys ----

  void panel_key(const KeyPress& press) {
    if (handler_ == nullptr || !last_panel_.visible || !last_panel_.wants_keys) return;
    core::Event event;
    event.type = core::Event::Type::PanelKeyPressed;
    event.ctrl = press.ctrl;
    switch (press.virtual_key) {
      case VK_ESCAPE: event.panel_key = core::PanelKey::Escape; break;
      case VK_RETURN: event.panel_key = core::PanelKey::Return; break;
      case VK_BACK: event.panel_key = core::PanelKey::Backspace; break;
      case VK_UP: event.panel_key = core::PanelKey::Up; break;
      case VK_DOWN: event.panel_key = core::PanelKey::Down; break;
      default:
        if (press.text.empty()) return;
        event.panel_key = core::PanelKey::Character;
        event.text = press.text;
        break;
    }
    dispatch(event);
  }

  // ---- picker ----

  void open_picker() {
    picker_query_.clear();
    picker_selection_ = 0;
    picker_scroll_ = 0;
    if (picker_ == nullptr) picker_ = std::make_unique<Surface>(Surface::Kind::Panel, 248, /*rounded=*/true);
    picker_visible_ = true;
    filter_picker_items();
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
    layout_picker(/*raise=*/true);
    notify_picker_event(core::Event::Type::PickerHighlighted);
  }

  void close_picker() {
    picker_visible_ = false;
    if (picker_ != nullptr) picker_->hide();
  }

  void notify_picker_event(core::Event::Type type) {
    if (handler_ == nullptr || picker_kind_ != PickerKind::List) return;
    core::Event event;
    event.type = type;
    if (type == core::Event::Type::PickerHighlighted && picker_selection_ < picker_filtered_.size()) {
      event.cell = picker_items_[picker_filtered_[picker_selection_]].id;
    }
    dispatch(event);
  }

  void picker_key(const KeyPress& press) {
    const DWORD vk = press.virtual_key;
    if (vk == VK_ESCAPE) {
      close_picker();
      notify_picker_event(core::Event::Type::PickerCancelled);
      return;
    }
    if (vk == VK_RETURN) {
      choose_picker_item();
      return;
    }
    if (vk == VK_UP || (press.ctrl && (press.text == "p" || press.text == "k"))) {
      move_picker_selection(-1);
      return;
    }
    if (vk == VK_DOWN || (press.ctrl && (press.text == "n" || press.text == "j"))) {
      move_picker_selection(1);
      return;
    }
    if (vk == VK_BACK) {
      if (!picker_query_.empty()) {
        // Pop one UTF-8 code point.
        do {
          picker_query_.pop_back();
        } while (!picker_query_.empty() && (static_cast<unsigned char>(picker_query_.back()) & 0xC0) == 0x80);
        refilter_picker();
      }
      return;
    }
    if (press.ctrl || press.text.empty()) return;
    picker_query_ += press.text;
    refilter_picker();
  }

  void refilter_picker() {
    picker_selection_ = 0;
    picker_scroll_ = 0;
    filter_picker_items();
    layout_picker(false);
    notify_picker_event(core::Event::Type::PickerHighlighted);
  }

  void move_picker_selection(int delta) {
    if (picker_filtered_.empty()) return;
    const std::size_t count = picker_filtered_.size();
    picker_selection_ = (picker_selection_ + count + (delta > 0 ? 1 : count - 1)) % count;
    if (picker_selection_ < picker_scroll_) picker_scroll_ = picker_selection_;
    if (picker_selection_ >= picker_scroll_ + kPickerMaxRows) {
      picker_scroll_ = picker_selection_ - kPickerMaxRows + 1;
    }
    layout_picker(false);
    notify_picker_event(core::Event::Type::PickerHighlighted);
  }

  void filter_picker_items() {
    picker_filtered_.clear();
    std::vector<std::pair<int, std::size_t>> scored;
    for (std::size_t index = 0; index < picker_items_.size(); ++index) {
      const int score = fuzzy_score(picker_query_, picker_items_[index].label);
      if (score >= 0) scored.emplace_back(score, index);
    }
    // Stable: equal scores keep the supplied order (alphabetical for apps).
    std::stable_sort(scored.begin(), scored.end(),
                     [](const auto& left, const auto& right) { return left.first > right.first; });
    for (const auto& entry : scored) picker_filtered_.push_back(entry.second);
    if (picker_selection_ >= picker_filtered_.size()) picker_selection_ = 0;
  }

  void choose_picker_item() {
    const bool has_selection = picker_selection_ < picker_filtered_.size();
    const core::PickerItem selected =
        has_selection ? picker_items_[picker_filtered_[picker_selection_]] : core::PickerItem{};
    if (picker_kind_ == PickerKind::List) {
      const std::string query = picker_query_;
      close_picker();
      core::Event event;
      event.type = core::Event::Type::PickerSelected;
      event.cell = selected.id;
      event.text = query;
      dispatch(event);
      return;
    }
    close_picker();
    if (has_selection) {
      if (debug_) std::fprintf(stderr, "mepwm: launch: %s\n", selected.id.c_str());
      // The launched app should come to the front; let it take the
      // foreground (it would otherwise open behind the current window).
      claim_foreground_rights();
      AllowSetForegroundWindow(ASFW_ANY);
      ShellExecuteW(nullptr, L"open", widen(selected.id).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
  }

  void layout_picker(bool raise) {
    const std::size_t row_count =
        picker_filtered_.empty() ? 1
                                 : std::min<std::size_t>(picker_filtered_.size() - picker_scroll_, kPickerMaxRows);
    const int height = kPickerQueryHeight + static_cast<int>(row_count) * kPickerRowHeight + 10;
    // Centered horizontally, upper part of the work area (launcher-style).
    const core::Rect area = work_area();
    const int left = area.x + (area.width - kPickerWidth) / 2;
    const int top = area.y + area.height / 5;
    const COLORREF background = color_from_hex(last_chrome_.background_color);
    const COLORREF foreground = color_from_hex(last_chrome_.text_color);
    const COLORREF accent = color_from_hex(last_chrome_.accent_color);
    std::vector<DrawItem>& items = picker_->items();
    items.clear();
    DrawItem query = text_item(RECT{14, 8, kPickerWidth - 14, kPickerQueryHeight - 4},
                               picker_title_ + ": " + picker_query_ + "_", accent, DT_LEFT);
    query.font = picker_query_font_;
    items.push_back(std::move(query));
    if (picker_filtered_.empty()) {
      DrawItem empty = text_item(RECT{14, kPickerQueryHeight, kPickerWidth - 14, kPickerQueryHeight + kPickerRowHeight},
                                 "(no matches)", foreground, DT_LEFT);
      empty.font = picker_row_font_;
      items.push_back(std::move(empty));
    }
    for (std::size_t row = 0; !picker_filtered_.empty() && row < row_count; ++row) {
      const std::size_t item_index = picker_scroll_ + row;
      const bool selected = item_index == picker_selection_;
      const int row_top = kPickerQueryHeight + static_cast<int>(row) * kPickerRowHeight;
      DrawItem item = text_item(RECT{8, row_top, kPickerWidth - 8, row_top + kPickerRowHeight - 4},
                                " " + picker_items_[picker_filtered_[item_index]].label,
                                selected ? RGB(255, 255, 255) : foreground, DT_LEFT);
      item.font = picker_row_font_;
      item.filled = selected;
      item.fill = accent;
      item.radius = 4;
      items.push_back(std::move(item));
    }
    picker_->set_background(background);
    picker_->show(RECT{left, top, left + kPickerWidth, top + height}, raise);
  }

  // ---- hints ----

  static std::string hint_label(std::size_t index, std::size_t total) {
    static constexpr char kCharset[] = "asdfghjklqwertyuiopzxcvbnm";
    constexpr std::size_t kBase = 26;
    if (total <= kBase) return std::string(1, kCharset[index]);  // NOLINT(modernize-return-braced-init-list)
    const std::size_t first = std::min(index / kBase, kBase - 1);
    return std::string(1, kCharset[first]) + kCharset[index % kBase];
  }

  void build_hints() {
    hints_.clear();
    std::vector<HintTarget> candidates;
    const auto add_cells = [&](const Surface* surface) {
      if (surface == nullptr || !surface->visible()) return;
      for (const DrawItem& item : surface->items()) {
        if (item.id.empty() || item.hidden) continue;  // decorative / clipped
        candidates.push_back({{}, surface->screen_rect(item), item.area, item.id});
      }
    };
    add_cells(top_bar_.get());
    add_cells(left_dock_.get());
    add_cells(right_dock_.get());
    add_cells(bottom_bar_.get());
    for (const auto& surface : tab_bar_surfaces_) add_cells(surface.get());
    if (last_panel_.visible) add_cells(side_panel_.get());
    // Reading order, top-left first.
    std::sort(candidates.begin(), candidates.end(), [](const HintTarget& left, const HintTarget& right) {
      return left.frame.top != right.frame.top ? left.frame.top < right.frame.top
                                               : left.frame.left < right.frame.left;
    });
    for (std::size_t index = 0; index < candidates.size(); ++index) {
      candidates[index].label = hint_label(index, candidates.size());
      hints_.push_back(std::move(candidates[index]));
    }
  }

  void hint_key(const KeyPress& press) {
    if (press.virtual_key == VK_ESCAPE) {
      close_hints();
      return;
    }
    if (press.virtual_key == VK_BACK) {
      if (!hint_query_.empty()) {
        hint_query_.pop_back();
        refresh_hint_matches();
      }
      return;
    }
    if (press.ctrl) return;
    bool changed = false;
    for (const char raw : press.text) {
      const char typed = static_cast<char>(std::tolower(static_cast<unsigned char>(raw)));
      if (typed < 'a' || typed > 'z') continue;
      const std::string candidate = hint_query_ + typed;
      if (std::any_of(hints_.begin(), hints_.end(), [&](const HintTarget& hint) {
            return hint.label.compare(0, candidate.size(), candidate) == 0;
          })) {
        hint_query_ = candidate;
        changed = true;
      }
    }
    if (changed) refresh_hint_matches();
  }

  void refresh_hint_matches() {
    const HintTarget* exact = nullptr;
    std::vector<DrawItem>& items = hints_surface_->items();
    for (std::size_t index = 0; index < hints_.size() && index < items.size(); ++index) {
      const bool matches = hints_[index].label.compare(0, hint_query_.size(), hint_query_) == 0;
      items[index].hidden = !matches;
      if (matches && hints_[index].label == hint_query_) exact = &hints_[index];
    }
    if (exact != nullptr) {
      // Copy before close_hints() clears the vector the pointer is into.
      const core::ChromeArea area = exact->area;
      const std::string cell = exact->cell;
      close_hints();
      core::Event event;
      event.type = core::Event::Type::ChromeClicked;
      event.area = area;
      event.cell = cell;
      dispatch(event);
      return;
    }
    InvalidateRect(hints_surface_->hwnd(), nullptr, FALSE);
  }

  void close_hints() {
    hints_visible_ = false;
    hint_query_.clear();
    hints_.clear();
    if (hints_surface_ != nullptr) hints_surface_->hide();
  }

  // ---- static callbacks ----

  static LRESULT CALLBACK keyboard_proc(int code, WPARAM wparam, LPARAM lparam) {
    if (code == HC_ACTION && g_platform != nullptr &&
        g_platform->on_keyboard(wparam, *reinterpret_cast<const KBDLLHOOKSTRUCT*>(lparam)) != 0) {
      return 1;  // swallowed
    }
    return CallNextHookEx(nullptr, code, wparam, lparam);
  }

  static void CALLBACK win_event_proc(HWINEVENTHOOK, DWORD event, HWND window, LONG object, LONG child,
                                      DWORD, DWORD) {
    if (g_platform == nullptr || window == nullptr || object != OBJID_WINDOW || child != CHILDID_SELF) return;
    switch (event) {
      case EVENT_SYSTEM_FOREGROUND:
      case EVENT_SYSTEM_MINIMIZESTART:
      case EVENT_SYSTEM_MINIMIZEEND:
      case EVENT_OBJECT_SHOW:
      case EVENT_OBJECT_HIDE:
      case EVENT_OBJECT_DESTROY:
      case EVENT_OBJECT_CLOAKED:
      case EVENT_OBJECT_UNCLOAKED:
        // Coalesce bursts (an app opening fires several) into one re-poll.
        SetTimer(g_platform->message_window_, kQuickPollTimer, 40, nullptr);
        break;
      default:
        break;
    }
  }

  static BOOL WINAPI console_handler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT ||
        type == CTRL_LOGOFF_EVENT || type == CTRL_SHUTDOWN_EVENT) {
      if (g_platform != nullptr && g_platform->message_window_ != nullptr) {
        PostMessageW(g_platform->message_window_, kStopMessage, 0, 0);
        // Closing the console window terminates the process as soon as
        // this returns; hold on until hidden windows are shown again.
        if (type != CTRL_C_EVENT && type != CTRL_BREAK_EVENT && g_exited != nullptr) {
          WaitForSingleObject(g_exited, 3000);
        }
        return TRUE;
      }
    }
    return FALSE;
  }

  static LRESULT CALLBACK message_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    WindowsPlatform* self = g_platform;
    if (self == nullptr) return DefWindowProcW(hwnd, message, wparam, lparam);
    switch (message) {
      case WM_TIMER:
        if (wparam == kQuickPollTimer) {
          KillTimer(hwnd, kQuickPollTimer);
          self->poll(false);
        } else if (wparam == kPollTimer) {
          self->poll(true);
        }
        return 0;
      case kHotkeyMessage: {
        const std::size_t index = static_cast<std::size_t>(wparam);
        if (index >= self->hotkeys_.size()) return 0;
        if (self->debug_) std::fprintf(stderr, "mepwm: hotkey %zu\n", index);
        core::Event event;
        event.type = core::Event::Type::ActionTriggered;
        event.action = self->hotkeys_[index].action;
        event.argument = self->hotkeys_[index].argument;
        self->dispatch(event);
        return 0;
      }
      case kKeyMessage:
        for (;;) {
          KeyPress press;
          {
            const std::lock_guard<std::mutex> lock(self->key_queue_mutex_);
            if (self->key_queue_.empty()) break;
            press = std::move(self->key_queue_.front());
            self->key_queue_.pop_front();
          }
          self->handle_key(press);
        }
        return 0;
      case kStopMessage:
      case WM_CLOSE:  // `mepwm --quit` (just service-stop)
        self->stop();
        return 0;
      case WM_ENDSESSION:
        // Logoff/shutdown tears the process down without run() returning.
        if (wparam != 0) self->show_all_hidden();
        return 0;
      case WM_DISPLAYCHANGE:
      case WM_SETTINGCHANGE:
        SetTimer(hwnd, kQuickPollTimer, 100, nullptr);
        break;
      default:
        break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
  }

  PrimaryKey primary_ = PrimaryKey::Alt;
  DWORD own_pid_ = 0;
  bool com_initialized_ = false;
  bool debug_ = debug_enabled();
  bool running_ = false;
  bool in_handler_ = false;
  ULONGLONG git_retry_at_ = 0;
  bool fullscreen_ = false;  // a fullscreen app is in front; chrome hidden
  bool caps_lock_ = false;  // hook thread only; seeded in start_hook_thread
  HWND message_window_ = nullptr;
  std::thread hook_thread_;
  DWORD hook_thread_id_ = 0;
  std::vector<HWINEVENTHOOK> win_event_hooks_;
  // Invisible resize-border widths per window, remembered because they
  // can't be measured while a window is hidden.
  std::unordered_map<HWND, RECT> insets_;
  std::function<void(const core::Event&)> handler_;
  std::deque<core::Event> deferred_;
  std::mutex key_queue_mutex_;
  std::deque<KeyPress> key_queue_;  // hook thread -> UI thread
  std::set<HWND> hidden_;           // windows hidden for inactive workspaces
  std::vector<HotkeyEntry> hotkeys_;
  std::vector<core::WindowId> previous_ids_;
  core::WindowId previous_focus_ = core::kNoWindow;
  core::Rect previous_area_;
  std::unordered_map<DWORD, std::string> app_names_;
  std::unordered_map<std::string, HKL> layouts_;

  std::wstring icon_font_name_;
  HFONT chrome_font_ = nullptr;
  HFONT title_font_ = nullptr;
  HFONT picker_query_font_ = nullptr;
  HFONT picker_row_font_ = nullptr;

  std::vector<std::unique_ptr<Surface>> border_surfaces_;
  std::unique_ptr<Surface> top_bar_;
  std::unique_ptr<Surface> bottom_bar_;
  std::unique_ptr<Surface> left_dock_;
  std::unique_ptr<Surface> right_dock_;
  core::Chrome last_chrome_;
  std::vector<std::unique_ptr<Surface>> tab_bar_surfaces_;
  std::vector<core::TabBar> last_tab_bars_;

  std::unique_ptr<Surface> side_panel_;
  core::SidePanel last_panel_;
  // The three capture flags are read by the hook thread.
  std::atomic<bool> panel_keys_{false};  // typing currently routes to the panel
  HWND panel_focus_snapshot_ = nullptr;  // foreground when it started

  std::unique_ptr<Surface> picker_;
  PickerKind picker_kind_ = PickerKind::Apps;
  std::string picker_title_;
  std::vector<core::PickerItem> picker_items_;
  std::vector<std::size_t> picker_filtered_;
  std::string picker_query_;
  std::string picker_initial_id_;
  std::size_t picker_selection_ = 0;
  std::size_t picker_scroll_ = 0;
  std::atomic<bool> picker_visible_{false};

  std::unique_ptr<Surface> hints_surface_;
  std::vector<HintTarget> hints_;
  std::string hint_query_;
  std::atomic<bool> hints_visible_{false};
};

}  // namespace

std::unique_ptr<Backend> make_windows_backend() {
  return std::make_unique<core::TilingEngine>(std::make_unique<WindowsPlatform>());
}

}  // namespace mepwm
