#include "core/tree.hpp"

#include <algorithm>

namespace mepwm::core {

namespace {

// The unique_ptr slot that owns `node` (root slot when it has no parent).
std::unique_ptr<PaneNode>& slot_of(std::unique_ptr<PaneNode>& root, PaneNode* node) {
  if (node->parent == nullptr) return root;
  return node->parent->first.get() == node ? node->parent->first : node->parent->second;
}

// Replaces `parent` (an internal node) with the subtree `keep`, which must
// be one of its children's subtrees.
void replace_with(std::unique_ptr<PaneNode>& root, PaneNode* parent,
                  std::unique_ptr<PaneNode> keep) {
  keep->parent = parent->parent;
  slot_of(root, parent) = std::move(keep);  // destroys `parent` and the other child
}

}  // namespace

PaneNode* first_leaf(PaneNode* node) {
  while (node != nullptr && !node->is_leaf()) node = node->first.get();
  return node;
}

PaneNode* leaf_of(PaneNode* root, WindowId window) {
  if (root == nullptr || window == kNoWindow) return nullptr;
  if (root->is_leaf()) {
    const auto found = std::find(root->tabs.begin(), root->tabs.end(), window);
    return found != root->tabs.end() ? root : nullptr;
  }
  if (PaneNode* leaf = leaf_of(root->first.get(), window)) return leaf;
  return leaf_of(root->second.get(), window);
}

void collect_windows(const PaneNode* node, std::vector<WindowId>& out) {
  if (node == nullptr) return;
  if (node->is_leaf()) {
    out.insert(out.end(), node->tabs.begin(), node->tabs.end());
    return;
  }
  collect_windows(node->first.get(), out);
  collect_windows(node->second.get(), out);
}

void layout_panes(PaneNode* node, const Rect& area, int gap, std::vector<PaneFrame>& out) {
  if (node == nullptr) return;
  if (node->is_leaf()) {
    out.push_back({node, area});
    return;
  }
  const float ratio = std::clamp(node->ratio, 0.1F, 0.9F);
  if (node->vertical_split) {
    const int first_width =
        std::max(1, static_cast<int>(static_cast<float>(area.width - gap) * ratio));
    layout_panes(node->first.get(), {area.x, area.y, first_width, area.height}, gap, out);
    layout_panes(node->second.get(),
                 {area.x + first_width + gap, area.y,
                  std::max(1, area.width - first_width - gap), area.height},
                 gap, out);
  } else {
    const int first_height =
        std::max(1, static_cast<int>(static_cast<float>(area.height - gap) * ratio));
    layout_panes(node->first.get(), {area.x, area.y, area.width, first_height}, gap, out);
    layout_panes(node->second.get(),
                 {area.x, area.y + first_height + gap, area.width,
                  std::max(1, area.height - first_height - gap)},
                 gap, out);
  }
}

PaneNode* split_leaf(PaneNode* leaf, bool vertical) {
  auto first = std::make_unique<PaneNode>();
  first->tabs = std::move(leaf->tabs);
  first->active_tab = leaf->active_tab;
  first->parent = leaf;
  auto second = std::make_unique<PaneNode>();
  second->parent = leaf;
  leaf->tabs.clear();
  leaf->active_tab = 0;
  leaf->vertical_split = vertical;
  leaf->ratio = 0.5F;
  leaf->first = std::move(first);
  leaf->second = std::move(second);
  return leaf->second.get();
}

void remove_window(std::unique_ptr<PaneNode>& root, WindowId window, PaneNode** selected) {
  PaneNode* leaf = leaf_of(root.get(), window);
  if (leaf == nullptr) return;
  leaf->tabs.erase(std::remove(leaf->tabs.begin(), leaf->tabs.end(), window), leaf->tabs.end());
  if (leaf->active_tab >= leaf->tabs.size()) {
    leaf->active_tab = leaf->tabs.empty() ? 0 : leaf->tabs.size() - 1;
  }
  if (!leaf->tabs.empty() || *selected == leaf) return;
  // Prune the empty leaf: its sibling takes the parent's place. A root
  // leaf stays as the workspace's landing pane.
  PaneNode* parent = leaf->parent;
  if (parent == nullptr) return;
  std::unique_ptr<PaneNode> sibling =
      std::move(parent->first.get() == leaf ? parent->second : parent->first);
  PaneNode* sibling_raw = sibling.get();
  replace_with(root, parent, std::move(sibling));
  if (*selected == leaf) *selected = first_leaf(sibling_raw);
}

void merge_with_sibling(std::unique_ptr<PaneNode>& root, PaneNode* leaf, PaneNode** selected) {
  PaneNode* parent = leaf->parent;
  if (parent == nullptr) return;
  PaneNode* sibling = parent->first.get() == leaf ? parent->second.get() : parent->first.get();
  std::vector<WindowId> absorbed;
  collect_windows(sibling, absorbed);
  leaf->tabs.insert(leaf->tabs.end(), absorbed.begin(), absorbed.end());
  std::unique_ptr<PaneNode> keep =
      std::move(parent->first.get() == leaf ? parent->first : parent->second);
  PaneNode* keep_raw = keep.get();
  replace_with(root, parent, std::move(keep));  // destroys the sibling subtree
  *selected = keep_raw;
}

}  // namespace mepwm::core
