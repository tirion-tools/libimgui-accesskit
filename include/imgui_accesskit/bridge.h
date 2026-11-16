// Dear ImGui → AccessKit (AT-SPI) bridge. The UI layer declares nodes every frame with explicit roles;
// containers nest with push/pop. end_frame() publishes the tree only when it changed. AccessKit
// actions arrive on AccessKit's thread and are marshalled to the UI thread as an SDL user event.
// Rules: levels are one-based here and passed to AccessKit as level − 1;
// relation targets must exist in the same frame; node ids are unique per frame (a repeat gets a
// derived id and is counted in duplicate_ids(); tests fail on any repeat).
#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <SDL3/SDL.h>

struct accesskit_unix_adapter;
struct accesskit_tree_update;

namespace imgui_accesskit {

// AccessKit roles, mapped to accesskit_role in bridge.cc (callers never include accesskit.h).
enum class Role : uint8_t {
  kWindow, kGroup, kLabel, kButton, kToolbar, kTree, kTreeItem, kTabList, kTab, kTable, kRow, kRowHeader,
  kCell, kColumnHeader, kHeading, kDocument, kParagraph, kTextInput, kSearchInput, kPasswordInput, kStatus, kAlert, kAlertDialog, kMenu,
  kMenuItem, kMenuItemRadio, kMenuItemCheckBox, kDialog, kSplitter, kList, kListItem, kScrollView, kTextRun, kLink,
  kImage, kCaption,  // mirrored CEF content (cef_tree.h)
};

// A caret position in AccessKit's text model: a character index (not bytes) within a kTextRun node.
struct TextPosition {
  uint64_t node = 0;  // 0 = unset
  size_t character = 0;
  bool operator==(const TextPosition&) const = default;
};

struct Node {
  uint64_t id = 0;
  Role role = Role::kGroup;
  std::string key;  // stable widget key (tests, logs)
  std::string label, description, value;
  std::string url;  // kLink (http/https/mailto, validated by the CEF mirror)
  std::string language;  // BCP 47 tag (AccessKit language); the root carries the UI language
  float x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // physical px, window relative
  int selected = -1, expanded = -1, toggled = -1, checked = -1;  // -1 = not applicable
  int live = 0;        // 0 off, 1 polite, 2 assertive
  int level = 0;       // one-based heading / tree level (0 = none)
  int has_popup = 0;   // 1 menu, 2 listbox, 3 dialog
  int orientation = 0; // 1 horizontal, 2 vertical
  bool modal = false, disabled = false;
  int actions = 0;     // Bridge::kClick | Bridge::kFocus
  double numeric = -1, numeric_min = -1, numeric_max = -1;  // splitters: < 0 = unset
  int64_t row_index = -1, row_count = -1;  // table rows / table size (AccessKit row_index/row_count)
  uint64_t active_descendant = 0;
  // Text: a kTextRun is one line; `value` holds its text (ending in '\n' at a hard line
  // break) and character_lengths the UTF-8 bytes of each character. A text root (kDocument, a text
  // input) names its selection by TextRun descendants; an invalid one is dropped in end_frame().
  std::vector<uint8_t> character_lengths;
  TextPosition selection_anchor, selection_focus;
  std::vector<uint64_t> controls, described_by, labelled_by;
  std::vector<uint64_t> children;
  bool operator==(const Node&) const = default;
};

class Bridge {
 public:
  static constexpr int kClick = 1, kFocus = 2;
  // Creates the AccessKit Unix adapter (it activates when an AT asks the registry). false if the
  // adapter could not be created; the application keeps running without accessibility then.
  // `window` is reserved for platform adapters that attach to a native window; the Unix adapter
  // does not use it. `title` labels the root node.
  bool start(SDL_Window* window, const std::string& title);
  void stop();
  ~Bridge() { stop(); }
  // The UI language on the root node (WCAG 3.1.1), a BCP 47 tag such as "en-US"; empty = none.
  // Defaults to locale_language().
  void set_language(std::string tag) { language_ = std::move(tag); }
  const std::string& language() const { return language_; }
  // The process locale as a BCP 47 tag: LC_ALL, else LC_MESSAGES, else LANG ("de_DE.UTF-8@euro"
  // → "de-DE"); "C", "POSIX" or unset → "en".
  static std::string locale_language();
  Bridge() = default;
  Bridge(const Bridge&) = delete;  // owns the adapter; AccessKit holds `this`
  Bridge& operator=(const Bridge&) = delete;

  // Frame protocol. scale = framebuffer px per logical px; window size in logical px.
  void begin_frame(float scale, float width, float height);
  // The returned reference is valid only until the next add()/push(): keep the id, not the reference.
  Node& add(uint64_t id, Role role, const std::string& key, const std::string& label, int actions = 0);
  Node& push(uint64_t id, Role role, const std::string& key, const std::string& label);
  void pop();  // a container without its own rect covers its children
  Node& top() { return nodes_[parents_.back()]; }  // the innermost open container (same validity as add())
  void set_rect(Node& n, float x0, float y0, float x1, float y1);  // logical px
  void focus(uint64_t id) { frame_focus_ = id; }
  void end_frame(bool window_focused);

  // SDL event for marshalled actions; handle_event consumes it (returns true).
  bool handle_event(const SDL_Event& e);
  bool take_click(uint64_t id) { return pending_clicks_.erase(id) != 0; }
  bool take_focus(uint64_t id) { return pending_focus_.erase(id) != 0; }

  // Stable id from a widget key (FNV-1a), never 0 or the root id.
  static uint64_t id_of(const std::string& key);

  const std::vector<Node>& last_frame() const { return last_frame_; }
  int duplicate_ids() const { return duplicates_; }
  bool active() const { return adapter_ != nullptr; }
  // An assistive technology has asked for the tree (AccessKit activated the adapter). It stays true
  // afterwards: the adapter's deactivation is not tracked.
  bool at_active() {
    std::lock_guard<std::mutex> lock(mu_);
    return builds_ > 0;
  }

  accesskit_tree_update* build_full_tree();  // AccessKit callbacks (any thread)
  void on_action(uint64_t node, int action) const;

 private:
  SDL_Window* window_ = nullptr;
  std::string title_;
  std::string language_ = locale_language();
  accesskit_unix_adapter* adapter_ = nullptr;
  Uint32 event_type_ = 0;
  float scale_ = 1;
  std::vector<Node> nodes_, last_frame_;
  std::vector<size_t> parents_;
  std::vector<bool> parent_has_rect_;
  std::set<uint64_t> frame_ids_, pending_clicks_, pending_focus_;
  uint64_t frame_focus_ = 0;
  int duplicates_ = 0;
  std::mutex mu_;  // guards published_*, builds_
  std::vector<Node> published_;
  uint64_t published_focus_ = 0;
  int builds_ = 0, reported_builds_ = -1;
  bool reported_focus_ = false;
};

}  // namespace imgui_accesskit
