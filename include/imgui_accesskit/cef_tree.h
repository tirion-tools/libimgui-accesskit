// Mirror of a CEF accessibility tree, with a bounded JSON parser of its own (the libFuzzer target
// is fuzz/fuzz_cef_tree.cc).
//
// Input: CefWriteJSON of OnAccessibilityTreeChange / OnAccessibilityLocationChange values, forwarded
// unchanged from the renderer process. It is renderer-controlled: the mirror is the trust boundary.
// Default limits (CefTreeLimits): 1.5 MiB of JSON, 20,000 nodes, 4,096 children per node, depth 128,
// 16 KiB strings (cut at a UTF-8 boundary), http/https/mailto URLs only, BCP 47-shaped languages only,
// 2 MiB of exported text, finite coordinates; JSON nesting 64. After every update a reachability pass
// from the root rebuilds a proper tree (unreachable nodes dropped, second parents ignored, depth cut), so
// AccessKit only ever sees a well-formed tree. Other generations are ignored; a rejected update invalidates the mirror
// until the next generation (the caller then exposes a placeholder).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace imgui_accesskit::cef {

struct CefTreeLimits {
  size_t max_json_bytes = size_t{1536} * 1024;  // larger updates are rejected
  size_t max_json_depth = 64;
  size_t max_nodes = 20000;
  size_t max_children = 4096;
  size_t max_depth = 128;
  size_t max_string = 16384;
  size_t max_total_text = size_t{2} << 20;
  size_t max_url = 2048;
  size_t max_updates = 64;
};

enum class CefKind : uint8_t {
  kDocument, kGeneric, kParagraph, kHeading, kLink, kList, kListItem, kTable, kRow, kCell, kColumnHeader, kRowHeader,
  kCaption, kStaticText, kInlineTextBox, kImage, kIgnored
};

struct CefNode {
  int32_t id = 0;
  CefKind kind = CefKind::kGeneric;
  std::string name, value, url;
  std::string language;  // the `language` attribute (HTML lang), a BCP 47-shaped tag; empty = inherited
  std::vector<int32_t> children;
  int32_t offset_container = 0;
  float x = 0, y = 0, w = 0, h = 0;  // relative to offset_container (CSS px)
  float scroll_x = 0, scroll_y = 0;
  int level = 0;
  bool focusable = false, invisible = false;
};

// One exported node in document order: depth-first, with absolute bounds in CSS px from the root.
struct CefExport {
  int32_t id = 0;
  int depth = 0;  // 0 = root
  CefKind kind = CefKind::kGeneric;
  std::string name, url, language;
  int level = 0;
  float x = 0, y = 0, w = 0, h = 0;
  std::vector<int32_t> children;  // exported children only
};

class CefTreeMirror {
 public:
  explicit CefTreeMirror(CefTreeLimits limits = {}) : limits_(limits) {}
  void reset(uint64_t generation);
  // false when rejected (why in last_reject()); the mirror is invalid until the next reset().
  bool apply(uint64_t generation, std::string_view json, bool location_only);

  uint64_t generation() const { return generation_; }
  bool valid() const { return valid_ && root_ != 0 && nodes_.count(root_) != 0; }
  int32_t focus() const { return focus_; }
  const std::string& last_reject() const { return last_reject_; }
  size_t node_count() const { return nodes_.size(); }
  // The well-formed tree (reachable from the root, depth-cut, one parent each).
  std::vector<CefExport> export_tree() const;

 private:
  bool reject(const std::string& why);
  CefTreeLimits limits_;
  uint64_t generation_ = 0;
  bool valid_ = false;
  int32_t root_ = 0, focus_ = 0;
  std::unordered_map<int32_t, CefNode> nodes_;
  std::string last_reject_;
};

}  // namespace imgui_accesskit::cef
