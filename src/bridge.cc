#include "imgui_accesskit/bridge.h"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <utility>

#include <accesskit.h>

namespace imgui_accesskit {
namespace {

constexpr uint64_t kRootId = 1;
constexpr uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr uint64_t kFnvPrime = 1099511628211ULL;

accesskit_role to_accesskit(Role role) {
  switch (role) {
    case Role::kWindow: return ACCESSKIT_ROLE_WINDOW;
    case Role::kGroup: return ACCESSKIT_ROLE_GROUP;
    case Role::kLabel: return ACCESSKIT_ROLE_LABEL;
    case Role::kButton: return ACCESSKIT_ROLE_BUTTON;
    case Role::kToolbar: return ACCESSKIT_ROLE_TOOLBAR;
    case Role::kTree: return ACCESSKIT_ROLE_TREE;
    case Role::kTreeItem: return ACCESSKIT_ROLE_TREE_ITEM;
    case Role::kTabList: return ACCESSKIT_ROLE_TAB_LIST;
    case Role::kTab: return ACCESSKIT_ROLE_TAB;
    case Role::kTable: return ACCESSKIT_ROLE_TABLE;
    case Role::kRow: return ACCESSKIT_ROLE_ROW;
    case Role::kRowHeader: return ACCESSKIT_ROLE_ROW_HEADER;
    case Role::kCell: return ACCESSKIT_ROLE_CELL;
    case Role::kColumnHeader: return ACCESSKIT_ROLE_COLUMN_HEADER;
    case Role::kHeading: return ACCESSKIT_ROLE_HEADING;
    case Role::kDocument: return ACCESSKIT_ROLE_DOCUMENT;
    case Role::kParagraph: return ACCESSKIT_ROLE_PARAGRAPH;
    case Role::kTextInput: return ACCESSKIT_ROLE_TEXT_INPUT;
    case Role::kSearchInput: return ACCESSKIT_ROLE_SEARCH_INPUT;
    case Role::kPasswordInput: return ACCESSKIT_ROLE_PASSWORD_INPUT;
    case Role::kStatus: return ACCESSKIT_ROLE_STATUS;
    case Role::kAlert: return ACCESSKIT_ROLE_ALERT;
    case Role::kAlertDialog: return ACCESSKIT_ROLE_ALERT_DIALOG;  // danger/blocking dialogs
    case Role::kMenu: return ACCESSKIT_ROLE_MENU;
    case Role::kMenuItem: return ACCESSKIT_ROLE_MENU_ITEM;
    case Role::kMenuItemRadio: return ACCESSKIT_ROLE_MENU_ITEM_RADIO;
    case Role::kMenuItemCheckBox: return ACCESSKIT_ROLE_MENU_ITEM_CHECK_BOX;
    case Role::kDialog: return ACCESSKIT_ROLE_DIALOG;
    case Role::kSplitter: return ACCESSKIT_ROLE_SPLITTER;
    case Role::kList: return ACCESSKIT_ROLE_LIST;
    case Role::kListItem: return ACCESSKIT_ROLE_LIST_ITEM;
    case Role::kScrollView: return ACCESSKIT_ROLE_SCROLL_VIEW;
    case Role::kTextRun: return ACCESSKIT_ROLE_TEXT_RUN;
    case Role::kLink: return ACCESSKIT_ROLE_LINK;
    case Role::kImage: return ACCESSKIT_ROLE_IMAGE;
    case Role::kCaption: return ACCESSKIT_ROLE_CAPTION;
    case Role::kCheckBox: return ACCESSKIT_ROLE_CHECK_BOX;
    case Role::kRadioButton: return ACCESSKIT_ROLE_RADIO_BUTTON;
    case Role::kComboBox: return ACCESSKIT_ROLE_COMBO_BOX;
    case Role::kComplementary: return ACCESSKIT_ROLE_COMPLEMENTARY;
    case Role::kNote: return ACCESSKIT_ROLE_NOTE;
  }
  return ACCESSKIT_ROLE_UNKNOWN;
}

// A relation target missing from the tree update panics AccessKit's consumer
// when an AT reads the relation set. Drop every reference to an id outside this frame.
void drop_dangling(std::vector<uint64_t>& ids, const std::set<uint64_t>& frame) {
  std::erase_if(ids, [&](uint64_t id) { return !frame.contains(id); });
}

bool has_selection(const Node& n) { return n.selection_anchor.node != 0 || n.selection_focus.node != 0; }

// AccessKit unwraps a text selection's positions (a panic on its thread otherwise): both must name a
// TextRun of this frame below the node that carries the selection.
void drop_invalid_selections(std::vector<Node>& nodes) {
  if (std::none_of(nodes.begin(), nodes.end(), has_selection)) return;
  std::unordered_map<uint64_t, size_t> index;
  std::unordered_map<uint64_t, uint64_t> parent;
  for (size_t i = 0; i < nodes.size(); ++i) {
    index.emplace(nodes[i].id, i);
    for (const uint64_t c : nodes[i].children) parent.emplace(c, nodes[i].id);
  }
  auto valid = [&](uint64_t root, const TextPosition& p) {
    const auto it = index.find(p.node);
    if (it == index.end() || nodes[it->second].role != Role::kTextRun) return false;
    for (auto up = parent.find(p.node); up != parent.end(); up = parent.find(up->second))
      if (up->second == root) return true;
    return false;
  };
  for (Node& n : nodes)
    if (has_selection(n) && (!valid(n.id, n.selection_anchor) || !valid(n.id, n.selection_focus)))
      n.selection_anchor = n.selection_focus = {};
}

accesskit_tree_update* activation_cb(void* user) { return static_cast<Bridge*>(user)->build_full_tree(); }
accesskit_tree_update* update_cb(void* user) { return static_cast<Bridge*>(user)->build_full_tree(); }
void action_cb(accesskit_action_request* request, void* user) {
  static_cast<Bridge*>(user)->on_action(*request);
  accesskit_action_request_free(request);
}
#ifndef _WIN32
void deactivation_cb(void* /*user*/) {}
#endif

}  // namespace

std::string Bridge::locale_language() {
  const char* v = nullptr;
  for (const char* name : {"LC_ALL", "LC_MESSAGES", "LANG"}) {
    v = std::getenv(name);
    if (v != nullptr && *v != '\0') break;
  }
  std::string s = v != nullptr ? v : "";
#ifdef _WIN32
  // Windows sets none of them: the user's default locale ("de-DE"), ASCII by definition.
  if (s.empty()) {
    wchar_t name[LOCALE_NAME_MAX_LENGTH];
    if (GetUserDefaultLocaleName(name, LOCALE_NAME_MAX_LENGTH) > 0)
      for (const wchar_t* p = name; *p != L'\0'; ++p) s += *p < 0x80 ? static_cast<char>(*p) : '?';
  }
#endif
  s = s.substr(0, s.find_first_of(".@"));  // drop the codeset and the modifier
  if (s.empty() || s == "C" || s == "POSIX") return "en";
  std::string out;
  for (char ch : s) {
    const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9');
    if (ch == '_' || ch == '-') out += '-';
    else if (ok) out += ch;
    else return "en";  // not a locale name
  }
  return out.empty() || out.front() == '-' ? "en" : out;
}

bool Bridge::start(SDL_Window* window, const std::string& title) {
  window_ = window;
  title_ = title;
  if (event_type_ == 0) event_type_ = SDL_RegisterEvents(1);  // 0 = none left: actions are dropped
  // Handlers run on AccessKit's threads; they only touch published_* under mu_.
#ifdef _WIN32
  if (adapter_ == nullptr && window != nullptr) {
    // AccessKit panics (aborts the process) when the window was ever shown: refuse that case here.
    if ((SDL_GetWindowFlags(window) & SDL_WINDOW_HIDDEN) == 0) return false;
    const auto hwnd = static_cast<HWND>(
        SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
    if (hwnd != nullptr)
      adapter_ = accesskit_windows_subclassing_adapter_new(hwnd, activation_cb, this, action_cb, this);
  }
#else
  if (adapter_ == nullptr)
    adapter_ = accesskit_unix_adapter_new(activation_cb, this, action_cb, this, deactivation_cb, this);
#endif
  return adapter_ != nullptr;
}

void Bridge::stop() {
#ifdef _WIN32
  if (adapter_ != nullptr) accesskit_windows_subclassing_adapter_free(adapter_);
#else
  if (adapter_ != nullptr) accesskit_unix_adapter_free(adapter_);
#endif
  adapter_ = nullptr;
}

void Bridge::begin_frame(float scale, float width, float height) {
  std::vector<Incoming> incoming;
  {
    std::lock_guard<std::mutex> lock(mu_);
    incoming.swap(incoming_);
  }
  for (Incoming& in : incoming) {
    if (in.action == ACCESSKIT_ACTION_CLICK) pending_clicks_.insert(in.node);
    if (in.action == ACCESSKIT_ACTION_FOCUS) pending_focus_.insert(in.node);
    if (in.action == ACCESSKIT_ACTION_SET_VALUE) pending_values_[in.node] = std::move(in.value);
  }
  scale_ = scale;
  nodes_.clear();
  parents_.clear();
  parent_has_rect_.clear();
  frame_focus_ = 0;
  frame_ids_ = {kRootId};
  Node root;
  root.id = kRootId;
  root.role = Role::kWindow;
  root.key = "window";
  root.label = title_;
  root.language = language_;
  // The root carries the window bounds (physical px); Wayland gives no global position.
  root.x1 = width * scale;
  root.y1 = height * scale;
  nodes_.push_back(std::move(root));
  parents_.push_back(0);
  parent_has_rect_.push_back(true);
}

Node& Bridge::add(uint64_t id, Role role, const std::string& key, const std::string& label, int actions) {
  // AccessKit aborts on a TreeUpdate that names a child twice. A repeat is counted and gets a
  // derived id so the published tree stays valid (never 0 or the root: bit 1 is set).
  if (!frame_ids_.insert(id).second) {
    ++duplicates_;
    do id = (id * kFnvPrime) | 2; while (!frame_ids_.insert(id).second);
  }
  Node n;
  n.id = id;
  n.role = role;
  n.key = key;
  n.label = label;
  n.actions = actions;
  nodes_[parents_.back()].children.push_back(id);
  nodes_.push_back(std::move(n));
  return nodes_.back();
}

Node& Bridge::push(uint64_t id, Role role, const std::string& key, const std::string& label) {
  add(id, role, key, label, 0);
  parents_.push_back(nodes_.size() - 1);
  parent_has_rect_.push_back(false);
  return nodes_.back();
}

void Bridge::pop() {
  if (parents_.size() <= 1) return;  // unbalanced pop: the root stays open
  const size_t idx = parents_.back();
  const bool has_rect = parent_has_rect_.back();
  parents_.pop_back();
  parent_has_rect_.pop_back();
  Node& n = nodes_[idx];
  if (has_rect || (n.x1 > n.x0 && n.y1 > n.y0)) return;
  // Direct children follow the container in declaration order, interleaved with their descendants.
  bool first = true;
  size_t k = 0;
  for (size_t j = idx + 1; j < nodes_.size() && k < n.children.size(); ++j) {
    const Node& c = nodes_[j];
    if (c.id != n.children[k]) continue;
    ++k;
    if (c.x1 <= c.x0 || c.y1 <= c.y0) continue;
    if (first) {
      n.x0 = c.x0, n.y0 = c.y0, n.x1 = c.x1, n.y1 = c.y1;
      first = false;
    } else {
      n.x0 = std::min(n.x0, c.x0), n.y0 = std::min(n.y0, c.y0);
      n.x1 = std::max(n.x1, c.x1), n.y1 = std::max(n.y1, c.y1);
    }
  }
}

void Bridge::set_rect(Node& n, float x0, float y0, float x1, float y1) {
  n.x0 = x0 * scale_, n.y0 = y0 * scale_, n.x1 = x1 * scale_, n.y1 = y1 * scale_;
  if (&n == &nodes_[parents_.back()]) parent_has_rect_.back() = true;
}

void Bridge::end_frame(bool window_focused) {
  while (parents_.size() > 1) pop();  // close containers the ui layer left open
  for (Node& n : nodes_) {
    drop_dangling(n.controls, frame_ids_);
    drop_dangling(n.described_by, frame_ids_);
    drop_dangling(n.labelled_by, frame_ids_);
    if (n.active_descendant != 0 && !frame_ids_.contains(n.active_descendant)) n.active_descendant = 0;
  }
  drop_invalid_selections(nodes_);
  last_frame_ = nodes_;
#ifdef _WIN32
  (void)window_focused;  // the subclassing adapter follows WM_SETFOCUS/WM_KILLFOCUS of the window itself
#else
  // AccessKit reports FOCUSED only while it believes the window is focused. SDL's focus event can
  // arrive before an assistive technology activates the adapter, so re-sync after every new
  // activation and on any change of the window's input focus.
  if (adapter_ != nullptr) {
    int builds = 0;
    {
      std::lock_guard<std::mutex> lock(mu_);
      builds = builds_;
    }
    if (window_focused != reported_focus_ || builds != reported_builds_) {
      accesskit_unix_adapter_update_window_focus_state(adapter_, window_focused);
      reported_focus_ = window_focused;
      reported_builds_ = builds;
    }
  }
#endif
  const uint64_t focus = frame_focus_ != 0 && frame_ids_.contains(frame_focus_) ? frame_focus_ : kRootId;
  bool changed = false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    changed = focus != published_focus_ || nodes_ != published_;
    if (changed) {
      published_ = nodes_;
      published_focus_ = focus;
    }
  }
#ifdef _WIN32
  // The queued UIA events are raised outside mu_: a UIA client may call back into build_full_tree().
  if (changed && adapter_ != nullptr) {
    accesskit_windows_queued_events* events =
        accesskit_windows_subclassing_adapter_update_if_active(adapter_, update_cb, this);
    if (events != nullptr) accesskit_windows_queued_events_raise(events);
  }
#else
  if (changed && adapter_ != nullptr) accesskit_unix_adapter_update_if_active(adapter_, update_cb, this);
#endif
}

accesskit_tree_update* Bridge::build_full_tree() {
  std::lock_guard<std::mutex> lock(mu_);
  ++builds_;
  accesskit_tree_update* update = accesskit_tree_update_with_capacity_and_focus(
      std::max<size_t>(published_.size(), 1), published_focus_ != 0 ? published_focus_ : kRootId);
  accesskit_tree_info* tree = accesskit_tree_info_new(kRootId);
  accesskit_tree_info_set_toolkit_name(tree, "Dear ImGui");
  accesskit_tree_update_set_tree_info(update, tree);
  if (published_.empty()) {
    accesskit_node* root = accesskit_node_new(ACCESSKIT_ROLE_WINDOW);
    accesskit_node_set_label(root, title_.c_str());
    if (!language_.empty()) accesskit_node_set_language(root, language_.c_str());
    accesskit_tree_update_push_node(update, kRootId, root);
    return update;
  }
  for (const Node& n : published_) {
    accesskit_node* node = accesskit_node_new(to_accesskit(n.role));
    if (!n.label.empty()) accesskit_node_set_label(node, n.label.c_str());
    if (!n.description.empty()) accesskit_node_set_description(node, n.description.c_str());
    if (!n.url.empty()) accesskit_node_set_url(node, n.url.c_str());
    if (!n.language.empty()) accesskit_node_set_language(node, n.language.c_str());
    // AccessKit unwraps a TextRun's value (a panic on its thread otherwise), so an empty run keeps "".
    if (!n.value.empty() || n.role == Role::kTextRun) {
      accesskit_node_set_value(node, n.value.c_str());
    } else if (n.role == Role::kLabel && !n.label.empty()) {
      // AccessKit names a Label from its value (static text): without it the AT-SPI name is empty.
      accesskit_node_set_value(node, n.label.c_str());
    }
    accesskit_node_set_bounds(node, accesskit_rect{n.x0, n.y0, n.x1, n.y1});
    if ((n.actions & kClick) != 0) accesskit_node_add_action(node, ACCESSKIT_ACTION_CLICK);
    if ((n.actions & kFocus) != 0) accesskit_node_add_action(node, ACCESSKIT_ACTION_FOCUS);
    if ((n.actions & kSetValue) != 0) accesskit_node_add_action(node, ACCESSKIT_ACTION_SET_VALUE);
    if (n.selected >= 0) accesskit_node_set_selected(node, n.selected == 1);
    if (n.expanded >= 0) accesskit_node_set_expanded(node, n.expanded == 1);
    if (n.live == 1) accesskit_node_set_live(node, ACCESSKIT_LIVE_POLITE);
    if (n.live == 2) accesskit_node_set_live(node, ACCESSKIT_LIVE_ASSERTIVE);
    if (n.live != 0) accesskit_node_set_live_atomic(node);
    if (n.modal) accesskit_node_set_modal(node);
    if (n.disabled) accesskit_node_set_disabled(node);
    if (n.invalid) accesskit_node_set_invalid(node, ACCESSKIT_INVALID_TRUE);
    // Node levels are one-based like ARIA and AT-SPI; AccessKit's level is zero-based.
    if (n.level > 0) accesskit_node_set_level(node, static_cast<size_t>(n.level - 1));
    if (n.has_popup == 1) accesskit_node_set_has_popup(node, ACCESSKIT_HAS_POPUP_MENU);
    if (n.has_popup == 2) accesskit_node_set_has_popup(node, ACCESSKIT_HAS_POPUP_LISTBOX);
    if (n.has_popup == 3) accesskit_node_set_has_popup(node, ACCESSKIT_HAS_POPUP_DIALOG);
    if (n.toggled >= 0) accesskit_node_set_toggled(node, n.toggled != 0 ? ACCESSKIT_TOGGLED_TRUE : ACCESSKIT_TOGGLED_FALSE);
    if (n.checked >= 0) accesskit_node_set_toggled(node, n.checked != 0 ? ACCESSKIT_TOGGLED_TRUE : ACCESSKIT_TOGGLED_FALSE);
    // Relations were filtered against this frame's ids in end_frame().
    if (n.active_descendant != 0) accesskit_node_set_active_descendant(node, n.active_descendant);
    if (!n.controls.empty()) accesskit_node_set_controls(node, n.controls.size(), n.controls.data());
    if (!n.described_by.empty()) accesskit_node_set_described_by(node, n.described_by.size(), n.described_by.data());
    if (!n.labelled_by.empty()) accesskit_node_set_labelled_by(node, n.labelled_by.size(), n.labelled_by.data());
    if (n.numeric >= 0) accesskit_node_set_numeric_value(node, n.numeric);
    if (n.numeric_min >= 0) accesskit_node_set_min_numeric_value(node, n.numeric_min);
    if (n.numeric_max >= 0) accesskit_node_set_max_numeric_value(node, n.numeric_max);
    if (n.row_index >= 0) accesskit_node_set_row_index(node, static_cast<size_t>(n.row_index));
    if (n.row_count >= 0) accesskit_node_set_row_count(node, static_cast<size_t>(n.row_count));
    if (n.orientation == 1) accesskit_node_set_orientation(node, ACCESSKIT_ORIENTATION_HORIZONTAL);
    if (n.orientation == 2) accesskit_node_set_orientation(node, ACCESSKIT_ORIENTATION_VERTICAL);
    if (!n.character_lengths.empty())
      accesskit_node_set_character_lengths(node, n.character_lengths.size(), n.character_lengths.data());
    // Positions were validated against this frame's TextRuns in end_frame().
    if (has_selection(n))
      accesskit_node_set_text_selection(node, accesskit_text_selection{{n.selection_anchor.node, n.selection_anchor.character},
                                                                       {n.selection_focus.node, n.selection_focus.character}});
    for (uint64_t child : n.children) accesskit_node_push_child(node, child);
    accesskit_tree_update_push_node(update, n.id, node);
  }
  return update;
}

void Bridge::on_action(const accesskit_action_request& request) {
  Incoming in{request.target_node, static_cast<int>(request.action), {}};
  if (in.action == ACCESSKIT_ACTION_SET_VALUE) {
    // Text only: a numeric SetValue (AT-SPI Value) has no taker yet.
    if (!request.data.has_value || request.data.value.tag != ACCESSKIT_ACTION_DATA_VALUE || request.data.value.value == nullptr)
      return;
    in.value = request.data.value.value;
  } else if (in.action != ACCESSKIT_ACTION_CLICK && in.action != ACCESSKIT_ACTION_FOCUS) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(mu_);
    incoming_.push_back(std::move(in));
  }
  if (event_type_ == 0) return;
  SDL_Event e;
  SDL_zero(e);
  e.type = event_type_;
  SDL_PushEvent(&e);  // wakes the UI thread; the next begin_frame() hands the action out
}

bool Bridge::handle_event(const SDL_Event& e) { return event_type_ != 0 && e.type == event_type_; }

bool Bridge::take_set_value(uint64_t id, std::string* value) {
  const auto it = pending_values_.find(id);
  if (it == pending_values_.end()) return false;
  *value = std::move(it->second);
  pending_values_.erase(it);
  return true;
}

uint64_t Bridge::id_of(const std::string& key) {
  uint64_t h = kFnvOffset;
  for (const char c : key) {
    h ^= static_cast<unsigned char>(c);
    h *= kFnvPrime;
  }
  if (h == 0 || h == kRootId) h |= 2;
  return h;
}

}  // namespace imgui_accesskit
