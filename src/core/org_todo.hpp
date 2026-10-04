#pragma once

#include <ctime>
#include <string>
#include <vector>

// Read-only org-mode plumbing shared by the todo pickers of every backend:
// turns the body of a "* TODO" headline (the lines between it and the next
// headline -- its :LOGBOOK: drawer, CLOCK lines, free-form notes) into the
// text a picker's preview pane shows. The backends keep their own file
// I/O and clock-in/out writers; this only interprets what they read.
namespace mepwm::core {

// One CLOCK line of a :LOGBOOK: drawer. `start`/`end` are the org
// timestamps' inner text ("2026-08-08 Sat 07:19"); `end` is empty and
// `open` is true for a still-running clock. `seconds` is the entry's
// length: the "=> H:MM" org wrote for a closed entry, or the time since
// `start` for an open one.
struct ClockEntry {
  std::string start;
  std::string end;
  bool open = false;
  long seconds = 0;
};

// Everything a preview wants to say about one todo.
struct TodoPreview {
  std::string title;    // the headline text
  std::string status;   // "Clocked in for 12m (since ...)" / "Not clocked in"
  std::string summary;  // logbook totals: entry count, total time, last clock-out
  std::vector<std::string> notes;   // body lines outside the drawer, blank lines collapsed
  std::vector<std::string> clocks;  // one line per CLOCK entry, file order (newest first in org)
};

// Parses the CLOCK lines of `body` (only those inside a :LOGBOOK: drawer
// or bare in the body -- org accepts both), in file order.
std::vector<ClockEntry> parse_clock_entries(const std::vector<std::string>& body, std::time_t now);

// Formats seconds as org's H:MM ("0:07", "12:05").
std::string format_org_duration(long seconds);

// Builds the preview for a todo whose headline text is `text` and whose
// body lines are `body`; `now` anchors the running-clock arithmetic.
TodoPreview build_todo_preview(const std::string& text, const std::vector<std::string>& body, std::time_t now);

// Flattens a preview into plain lines (blank lines between sections) for
// renderers without their own section styling.
std::vector<std::string> flatten_todo_preview(const TodoPreview& preview);

}  // namespace mepwm::core
