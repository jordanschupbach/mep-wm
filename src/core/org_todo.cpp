#include "core/org_todo.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <sstream>

namespace mepwm::core {

namespace {

std::string trim(const std::string& value) {
  const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char c) { return std::isspace(c); });
  const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char c) { return std::isspace(c); }).base();
  return first < last ? std::string(first, last) : std::string();
}

// Inverse of org's "[%Y-%m-%d %a %H:%M]" given the text between the
// brackets ("2026-08-07 Fri 10:23"); -1 when it doesn't parse.
std::time_t parse_org_timestamp(const std::string& text) {
  std::tm parsed{};
  std::istringstream stream(text);
  std::string date;
  std::string day;
  std::string time;
  stream >> date >> day >> time;
  char dash = 0;
  char colon = 0;
  std::istringstream date_stream(date);
  std::istringstream time_stream(time);
  if (!(date_stream >> parsed.tm_year >> dash >> parsed.tm_mon >> dash >> parsed.tm_mday)) return -1;
  if (!(time_stream >> parsed.tm_hour >> colon >> parsed.tm_min)) return -1;
  parsed.tm_year -= 1900;
  parsed.tm_mon -= 1;
  parsed.tm_isdst = -1;
  return std::mktime(&parsed);
}

// The text inside the first [...] pair at or after `from`; sets *next to
// just past the closing bracket. Empty when there is none.
std::string bracketed(const std::string& line, std::size_t from, std::size_t* next) {
  const std::size_t open = line.find('[', from);
  if (open == std::string::npos) return {};
  const std::size_t close = line.find(']', open);
  if (close == std::string::npos) return {};
  *next = close + 1;
  return line.substr(open + 1, close - open - 1);
}

// Parses org's "=> H:MM" duration suffix; -1 when absent or malformed.
long parse_org_duration(const std::string& line) {
  const std::size_t arrow = line.find("=>");
  if (arrow == std::string::npos) return -1;
  long hours = 0;
  long minutes = 0;
  char colon = 0;
  std::istringstream stream(line.substr(arrow + 2));
  if (!(stream >> hours >> colon >> minutes) || colon != ':') return -1;
  return hours * 3600 + minutes * 60;
}

// Elapsed time the way a running timer reads at a glance (matches the
// backends' bar pill): seconds under a minute, minutes under an hour,
// hours+minutes above.
std::string format_elapsed(long seconds) {
  seconds = std::max(seconds, 0L);
  if (seconds < 60) return std::to_string(seconds) + "s";
  const long total_minutes = seconds / 60;
  if (total_minutes < 60) return std::to_string(total_minutes) + "m";
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%ldh%02ldm", total_minutes / 60, total_minutes % 60);
  return buffer;
}

// True for a drawer opener (":LOGBOOK:", ":PROPERTIES:", ...): a line
// that is exactly a colon-wrapped word. ":END:" closes any drawer.
bool is_drawer_start(const std::string& trimmed) {
  if (trimmed.size() < 3 || trimmed.front() != ':' || trimmed.back() != ':' || trimmed == ":END:") return false;
  return std::all_of(trimmed.begin() + 1, trimmed.end() - 1,
                     [](unsigned char c) { return std::isalnum(c) || c == '_' || c == '-'; });
}

// Drops the date when `end` falls on the same day as `start` so a closed
// entry reads "2026-08-08 Sat 07:19 -- 07:31" rather than repeating it.
std::string short_end(const std::string& start, const std::string& end) {
  const std::size_t start_time = start.rfind(' ');
  const std::size_t end_time = end.rfind(' ');
  if (start_time == std::string::npos || end_time == std::string::npos) return end;
  return start.compare(0, start_time, end, 0, end_time) == 0 ? end.substr(end_time + 1) : end;
}

}  // namespace

std::vector<ClockEntry> parse_clock_entries(const std::vector<std::string>& body, std::time_t now) {
  std::vector<ClockEntry> entries;
  for (const std::string& raw : body) {
    const std::string line = trim(raw);
    if (line.rfind("CLOCK:", 0) != 0) continue;
    ClockEntry entry;
    std::size_t cursor = 6;
    entry.start = bracketed(line, cursor, &cursor);
    if (entry.start.empty()) continue;
    const std::size_t range = line.find("--", cursor);
    if (range == std::string::npos) {
      entry.open = true;
      const std::time_t start = parse_org_timestamp(entry.start);
      entry.seconds = start < 0 ? 0 : static_cast<long>(now - start);
    } else {
      entry.end = bracketed(line, range, &cursor);
      entry.seconds = parse_org_duration(line);
      if (entry.seconds < 0) {
        const std::time_t start = parse_org_timestamp(entry.start);
        const std::time_t end = parse_org_timestamp(entry.end);
        entry.seconds = (start < 0 || end < 0) ? 0 : static_cast<long>(end - start);
      }
    }
    entry.seconds = std::max(entry.seconds, 0L);
    entries.push_back(std::move(entry));
  }
  return entries;
}

std::string format_org_duration(long seconds) {
  seconds = std::max(seconds, 0L);
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%ld:%02ld", seconds / 3600, (seconds % 3600) / 60);
  return buffer;
}

TodoPreview build_todo_preview(const std::string& text, const std::vector<std::string>& body, std::time_t now) {
  TodoPreview preview;
  preview.title = trim(text);

  // Notes: whatever the body says outside drawers and clock lines, with
  // runs of blank lines collapsed and none at either end.
  bool in_drawer = false;
  for (const std::string& raw : body) {
    const std::string line = trim(raw);
    if (in_drawer) {
      if (line == ":END:") in_drawer = false;
      continue;
    }
    if (is_drawer_start(line)) {
      in_drawer = true;
      continue;
    }
    if (line.rfind("CLOCK:", 0) == 0) continue;
    if (line.empty()) {
      if (!preview.notes.empty() && !preview.notes.back().empty()) preview.notes.emplace_back();
      continue;
    }
    preview.notes.push_back(line);
  }
  if (!preview.notes.empty() && preview.notes.back().empty()) preview.notes.pop_back();

  const std::vector<ClockEntry> entries = parse_clock_entries(body, now);
  const auto running = std::find_if(entries.begin(), entries.end(), [](const ClockEntry& entry) { return entry.open; });
  preview.status = running == entries.end()
                       ? "Not clocked in"
                       : "Clocked in for " + format_elapsed(running->seconds) + "  (since " + running->start + ")";

  long total = 0;
  const ClockEntry* last_closed = nullptr;
  for (const ClockEntry& entry : entries) {
    total += entry.seconds;
    if (!entry.open && last_closed == nullptr) last_closed = &entry;  // org keeps newest first
  }
  if (entries.empty()) {
    preview.summary = "Logbook: no clock entries";
  } else {
    preview.summary = "Logbook: " + std::to_string(entries.size()) + (entries.size() == 1 ? " entry, " : " entries, ") +
                      format_org_duration(total) + " total";
    if (last_closed != nullptr) preview.summary += ", last clocked out " + last_closed->end;
  }
  for (const ClockEntry& entry : entries) {
    if (entry.open)
      preview.clocks.push_back(entry.start + " -- running  (" + format_elapsed(entry.seconds) + ")");
    else
      preview.clocks.push_back(entry.start + " -- " + short_end(entry.start, entry.end) + "  " +
                               format_org_duration(entry.seconds));
  }
  return preview;
}

std::vector<std::string> flatten_todo_preview(const TodoPreview& preview) {
  std::vector<std::string> lines;
  lines.push_back(preview.title);
  lines.push_back(preview.status);
  if (!preview.notes.empty()) {
    lines.emplace_back();
    lines.insert(lines.end(), preview.notes.begin(), preview.notes.end());
  }
  lines.emplace_back();
  lines.push_back(preview.summary);
  lines.insert(lines.end(), preview.clocks.begin(), preview.clocks.end());
  return lines;
}

}  // namespace mepwm::core
