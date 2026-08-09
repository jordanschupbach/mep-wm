#pragma once

// Nerd Font glyphs shared by every backend -- the same codepoints the X11
// backend uses (src/x11/x11_backend.cpp), so the chrome looks identical
// across platforms. Only used when Platform::has_icon_font() is true;
// otherwise the engine falls back to short text labels.
namespace mepwm::core::icons {

constexpr const char kBatteryFull[] = "\uf240";
constexpr const char kBattery75[] = "\uf241";
constexpr const char kBattery50[] = "\uf242";
constexpr const char kBattery25[] = "\uf243";
constexpr const char kBatteryEmpty[] = "\uf244";
constexpr const char kBatteryCharging[] = "\uf0e7";
constexpr const char kVolumeMuted[] = "\uf026";
constexpr const char kVolumeLow[] = "\uf027";
constexpr const char kVolumeHigh[] = "\uf028";
constexpr const char kThemeDark[] = "\uf186";
constexpr const char kThemeLight[] = "\U000f0599";
constexpr const char kCpu[] = "\U000f061a";
constexpr const char kMemory[] = "\U000f035b";
constexpr const char kDisk[] = "\uf0a0";
constexpr const char kWifi[] = "\uf1eb";
constexpr const char kBluetooth[] = "\uf293";
constexpr const char kMicrophone[] = "\uf130";
constexpr const char kMicrophoneMuted[] = "\uf131";
constexpr const char kGit[] = "\uf126";
constexpr const char kMedia[] = "\uf001";
constexpr const char kKeyboard[] = "\uf11c";
constexpr const char kClock[] = "\uf017";
constexpr const char kPomodoro[] = "\uf254";
constexpr const char kBell[] = "\uf0f3";
constexpr const char kTodo[] = "\uf0ae";
constexpr const char kAgents[] = "\uf120";
constexpr const char kInfo[] = "\uf05a";

// OS logos for the bottom bar's left-corner widget (the X11 backend keeps
// its distro icon there).
constexpr const char kApple[] = "\uf179";
constexpr const char kWindows[] = "\uf17a";
constexpr const char kLinux[] = "\uf17c";
constexpr const char kNixOs[] = "\uf313";

// Left-dock launcher apps (the X11 backend's hardcoded column).
constexpr const char kFirefox[] = "\uf269";
constexpr const char kTerminal[] = "\uf120";
constexpr const char kInkscape[] = "\ue801";
constexpr const char kGimp[] = "\ue7e7";
constexpr const char kLibreOffice[] = "\uf376";
constexpr const char kVsCode[] = "\ue8da";
constexpr const char kEmacs[] = "\ue7cf";
constexpr const char kNeovim[] = "\ue6a9";

}  // namespace mepwm::core::icons
