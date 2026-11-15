// libFuzzer target: the mirror of CEF accessibility JSON (src/cef_tree.cc).
// Byte 0: 'L' = location update, else tree update.
// Invariants: the export is a tree (each id once, children exported, depth bounded) and every
// exported URL is http/https/mailto. Seeds: CefWriteJSON captures.
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <unordered_set>

#include "imgui_accesskit/cef_tree.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size == 0) return 0;
  imgui_accesskit::cef::CefTreeMirror m;
  m.reset(1);
  const std::string_view in(reinterpret_cast<const char*>(data) + 1, size - 1);
  // Apply twice (tree then the same bytes as the requested kind) to exercise incremental updates.
  m.apply(1, in, false);
  m.apply(1, in, data[0] == 'L');
  std::unordered_set<int32_t> ids;
  for (const auto& e : m.export_tree()) {
    if (!ids.insert(e.id).second || e.depth >= 128) __builtin_trap();
    if (!e.url.empty() && e.url.rfind("http://", 0) != 0 && e.url.rfind("https://", 0) != 0 && e.url.rfind("mailto:", 0) != 0)
      __builtin_trap();
  }
  return 0;
}
