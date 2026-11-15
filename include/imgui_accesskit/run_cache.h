// Pango/cairo text runs. Every string, user content and UI labels alike, is shaped by Pango,
// rasterised by cairo into an A8 alpha mask and drawn
// with ImGui as a tinted image (GL_R8 with an alpha swizzle, so a theme switch needs no re-raster and a
// run costs one byte per pixel). Runs are cached per (text, style, scale); cold rasterisation is
// budgeted per frame (for example 2 ms), the rest is drawn on later frames.
#pragma once

#include <cstddef>
#include <cstdint>
#include <list>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "imgui.h"

// Pango's own type names (forward declarations, so this header does not pull in Pango).
using PangoContext = struct _PangoContext;  // NOLINT(bugprone-reserved-identifier)
using PangoFontMap = struct _PangoFontMap;  // NOLINT(bugprone-reserved-identifier)
using PangoLayout = struct _PangoLayout;    // NOLINT(bugprone-reserved-identifier)

namespace imgui_accesskit::text {

enum class Align : uint8_t { kStart, kEnd };
enum class Dir : uint8_t { kAuto, kLtr, kRtl };

struct RunStyle {
  std::string family;    // Pango family list, e.g. "Noto Sans"
  float size = 14;       // logical px
  int weight = 400;      // 400 regular, 600 semibold
  bool monospace = false;
  float max_width = -1;  // logical px; > 0: wrap (wrap = true) or end-ellipsize
  bool wrap = false;
  Dir dir = Dir::kAuto;  // kAuto: first strong character (user text); forced for UI sentences
  Align align = Align::kStart;
  // Fit text: > 0 = slot height in logical px; a single-line run whose ink exceeds it is
  // drawn at the largest of 95/90/85 % that fits, then clipped to the slot.
  float fit_slot = -1;
  float scale = 1;       // framebuffer px per logical px
  std::string key() const;
};

struct TextRun {
  unsigned texture = 0;
  // Colour glyphs (emoji: Noto Color Emoji is a bitmap colour font) cannot be tinted masks; runs
  // that contain them get an RGBA layer with those pixels, drawn untinted over the mask.
  unsigned color_texture = 0;
  int px_w = 0, px_h = 0;  // physical px
  float scale = 1;
  float baseline = 0;      // logical px from the top to the first baseline
  int lines = 0;
  bool ellipsized = false;
  bool rtl = false;        // resolved base direction of the first line
  float shrink = 1;        // Fit text factor applied (1, .95, .90, .85)
  size_t bytes() const { return size_t(px_w) * size_t(px_h) * (color_texture ? 5 : 1); }
  ImVec2 size() const { return {float(px_w) / scale, float(px_h) / scale}; }
  // Draws the run with its top-left at pos (snapped to physical pixels), tinted.
  void draw(ImDrawList* dl, ImVec2 pos, ImU32 color) const;
};

// A highlight rectangle in logical px relative to a run's top-left.
struct TextBox {
  float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};

class RunCache {
 public:
  RunCache();
  ~RunCache();
  RunCache(const RunCache&) = delete;
  RunCache& operator=(const RunCache&) = delete;

  // A cached run, rasterising it now if needed (no budget: chrome that must appear this frame).
  const TextRun& get(const std::string& text, const RunStyle& style);
  // Budgeted: nullptr when not cached and this frame's cold budget is spent (caller draws a
  // placeholder and keeps the window invalidated until it fills).
  const TextRun* try_get(const std::string& text, const RunStyle& style);
  // Logical width of the unwrapped single-line text, without rasterising (cached: a cached run's
  // width, else a measured width).
  float measure(const std::string& text, const RunStyle& style);
  // Font metrics in logical px for the style's family/size/weight.
  struct Metrics {
    float ascent = 0, descent = 0;
  };
  Metrics metrics(const RunStyle& style);

  // Per frame: resets the cold budget and evicts least-recently-used runs over the byte budget.
  void new_frame(double cold_budget_ms);
  bool deferred_this_frame() const { return deferred_ > 0; }
  int cold_this_frame() const { return cold_; }
  double cold_ms_this_frame() const { return spent_ms_; }
  size_t texture_bytes() const { return bytes_; }
  size_t size() const { return map_.size(); }
  void clear();

  // Hit-testing for text selection, on a layout identical to the run's (same text and style; the run
  // need not be rasterised). Offsets are byte offsets into `text`, points logical px relative to the
  // run's top-left. Layouts are kept for the most recently hit-tested runs.
  // The caret position nearest to the point (a grapheme boundary).
  size_t index_at(const std::string& text, const RunStyle& style, ImVec2 point);
  // Highlight boxes for the bytes [begin, end), one or more per line; `through_end` (the selection
  // continues after the text) also marks the hard line break after each selected line with a box
  // `break_w` wide, so a selected line break stays visible.
  void range_boxes(const std::string& text, const RunStyle& style, size_t begin, size_t end, bool through_end,
                   float break_w, std::vector<TextBox>& out);
  // The word around offset (Unicode word boundaries from Pango), or the grapheme there when the
  // offset is not inside a word; [first, second) in bytes.
  std::pair<size_t, size_t> word_at(const std::string& text, const RunStyle& style, size_t offset);

  // Shared Pango state for the line editor; a layout configured for the style (caller unrefs).
  PangoContext* context() const { return context_; }
  PangoLayout* make_layout(const std::string& text, const RunStyle& style);

  size_t byte_budget = size_t{24} << 20;  // A8 texture bytes kept (software GL renderers keep them in RAM)

 private:
  struct Entry {
    TextRun run;
    std::list<std::string>::iterator lru;
    uint64_t frame = 0;
  };
  struct HitLayout {
    PangoLayout* layout = nullptr;
    int dx = 0, dy = 0;  // where the run's raster puts the layout origin (physical px)
    std::list<std::string>::iterator lru;
  };
  HitLayout& hit_layout(const std::string& text, const RunStyle& style);
  TextRun rasterize(const std::string& text, const RunStyle& style);
  const TextRun& insert(std::string key, TextRun run);
  void touch(Entry& e);
  PangoFontMap* fontmap_ = nullptr;
  PangoContext* context_ = nullptr;
  std::unordered_map<std::string, Entry> map_;
  std::unordered_map<std::string, Metrics> metrics_;
  std::unordered_map<std::string, float> widths_;  // measure() results; cleared when large
  std::list<std::string> lru_;  // front = most recent
  std::unordered_map<std::string, HitLayout> hits_;
  std::list<std::string> hits_lru_;  // front = most recent
  size_t bytes_ = 0;
  uint64_t frame_ = 0;
  double budget_ms_ = 2, spent_ms_ = 0;
  int cold_ = 0, deferred_ = 0;
};

// Uploads an A8 buffer (rows of `stride` bytes) into a GL_R8 texture with the alpha swizzle; 0 on failure.
unsigned upload_alpha_texture(const unsigned char* data, int width, int height, int stride, unsigned reuse = 0);
void delete_texture(unsigned texture);

}  // namespace imgui_accesskit::text
