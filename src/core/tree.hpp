#pragma once

#include <memory>
#include <vector>

#include "core/geometry.hpp"
#include "core/platform.hpp"

namespace mepwm::core {

// One node of the manual layout tree (the X11 backend's Node concept,
// ported to core): a leaf holds tabbed windows; an internal node splits its
// area between exactly two children.
struct PaneNode {
  // Leaf payload: the tabs, in order, and which one is showing.
  std::vector<WindowId> tabs;
  std::size_t active_tab = 0;
  // Internal payload.
  bool vertical_split = false;  // true = side-by-side columns
  float ratio = 0.5F;           // first child's share of the area
  std::unique_ptr<PaneNode> first;
  std::unique_ptr<PaneNode> second;
  PaneNode* parent = nullptr;

  bool is_leaf() const { return first == nullptr; }
  WindowId active_window() const {
    return is_leaf() && active_tab < tabs.size() ? tabs[active_tab] : kNoWindow;
  }
};

struct PaneFrame {
  PaneNode* leaf = nullptr;
  Rect frame;
};

PaneNode* first_leaf(PaneNode* node);
PaneNode* leaf_of(PaneNode* root, WindowId window);
void collect_windows(const PaneNode* node, std::vector<WindowId>& out);

// Assigns an area to every leaf, splitting internal nodes by orientation
// and ratio with `gap` between siblings.
void layout_panes(PaneNode* node, const Rect& area, int gap, std::vector<PaneFrame>& out);

// Turns `leaf` into a split: its tabs move to the first child; the returned
// second child is a new empty pane (select it so the next window opens there).
PaneNode* split_leaf(PaneNode* leaf, bool vertical);

// Removes `window` from the tree. A leaf that becomes empty is pruned (its
// sibling absorbs the parent's slot) unless it is *selected; *selected is
// re-pointed when the node it referenced is destroyed.
void remove_window(std::unique_ptr<PaneNode>& root, WindowId window, PaneNode** selected);

// Dissolves the split above `leaf`: the sibling subtree's windows join
// `leaf` as tabs and `leaf` takes the parent's place.
void merge_with_sibling(std::unique_ptr<PaneNode>& root, PaneNode* leaf, PaneNode** selected);

}  // namespace mepwm::core
