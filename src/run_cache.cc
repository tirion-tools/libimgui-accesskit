#include "imgui_accesskit/run_cache.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include <SDL3/SDL_opengl.h>
#include <pango/pangocairo.h>

#ifndef GL_TEXTURE_SWIZZLE_RGBA
#define GL_TEXTURE_SWIZZLE_RGBA 0x8E46
#endif
#ifndef GL_R8
#define GL_R8 0x8229
#endif
#ifndef GL_RED
#define GL_RED 0x1903
#endif
#ifndef GL_UNPACK_ROW_LENGTH
#define GL_UNPACK_ROW_LENGTH 0x0CF2
#endif

namespace imgui_accesskit::text {
namespace {

double ms_since(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

PangoFontDescription* font_for(const RunStyle& s, float size_factor) {
  PangoFontDescription* d = pango_font_description_new();
  pango_font_description_set_family(d, s.family.empty() ? (s.monospace ? "Noto Sans Mono" : "Noto Sans")
                                                        : s.family.c_str());
  pango_font_description_set_weight(d, static_cast<PangoWeight>(s.weight));
  // Absolute sizes are device pixels and ignore the context resolution, so size × scale.
  pango_font_description_set_absolute_size(d, double(s.size * size_factor * s.scale) * PANGO_SCALE);
  return d;
}

// The context state a layout of this style is shaped with (shared by every layout of the context).
void set_context_for(PangoContext* context, const RunStyle& style) {
  pango_cairo_context_set_resolution(context, 96.0);
  pango_context_set_base_dir(context, style.dir == Dir::kRtl   ? PANGO_DIRECTION_RTL
                                      : style.dir == Dir::kLtr ? PANGO_DIRECTION_LTR
                                                               : PANGO_DIRECTION_WEAK_LTR);
}

constexpr size_t kHitLayouts = 48;  // ≥ the body blocks visible at once, so a selection never thrashes

}  // namespace

std::string RunStyle::key() const {
  char buf[160];
  // Colour is not part of the key: runs are alpha masks tinted at draw time.
  std::snprintf(buf, sizeof buf, "|%.2f|%d|%d|%.1f|%d|%d|%d|%.2f|%.3f|", double(size), weight, int(monospace),
                double(max_width), int(wrap), int(dir), int(align), double(fit_slot), double(scale));
  return family + buf;
}

void TextRun::draw(ImDrawList* dl, ImVec2 pos, ImU32 color) const {
  if (!texture) return;
  const float x = std::floor(pos.x * scale + 0.5f) / scale;
  const float y = std::floor(pos.y * scale + 0.5f) / scale;
  const ImVec2 s = size();
  dl->AddImage(ImTextureRef(ImTextureID(texture)), ImVec2(x, y), ImVec2(x + s.x, y + s.y), ImVec2(0, 0), ImVec2(1, 1),
               color);
  if (color_texture)  // keeps the run's opacity (disabled text), not its colour
    dl->AddImage(ImTextureRef(ImTextureID(color_texture)), ImVec2(x, y), ImVec2(x + s.x, y + s.y), ImVec2(0, 0),
                 ImVec2(1, 1), (color & IM_COL32_A_MASK) | 0x00FFFFFFu);
}

namespace {

// Codepoints that may be drawn from a colour font: pictographs, dingbats/misc symbols, regional
// indicators, keycap and emoji presentation selectors.
bool may_need_color(const std::string& text) {
  for (const gchar* p = text.c_str(); *p;) {
    const gunichar c = g_utf8_get_char_validated(p, -1);
    if (c == gunichar(-1) || c == gunichar(-2)) return false;
    if (c >= 0x1F000 || (c >= 0x2600 && c <= 0x27BF) || c == 0xFE0F || c == 0x20E3 || (c >= 0x2B00 && c <= 0x2BFF))
      return true;
    p = g_utf8_next_char(p);
  }
  return false;
}

unsigned upload_rgba_texture(const unsigned char* data, int width, int height) {
  GLuint tex = 0;
  glGenTextures(1, &tex);
  if (!tex) return 0;
  glBindTexture(GL_TEXTURE_2D, tex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
  return tex;
}

}  // namespace

unsigned upload_alpha_texture(const unsigned char* data, int width, int height, int stride, unsigned reuse) {
  GLuint tex = reuse;
  if (!tex) glGenTextures(1, &tex);
  if (!tex) return 0;
  glBindTexture(GL_TEXTURE_2D, tex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  const GLint swizzle[4] = {GL_ONE, GL_ONE, GL_ONE, GL_RED};  // alpha mask; RGB comes from the tint
  glTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_RGBA, swizzle);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, stride);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, width, height, 0, GL_RED, GL_UNSIGNED_BYTE, data);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
  return tex;
}

void delete_texture(unsigned texture) {
  if (!texture) return;
  GLuint t = texture;
  glDeleteTextures(1, &t);
}

RunCache::RunCache()
    : fontmap_(pango_cairo_font_map_new()), context_(pango_font_map_create_context(fontmap_)) {
  cairo_font_options_t* options = cairo_font_options_create();
  cairo_font_options_set_antialias(options, CAIRO_ANTIALIAS_GRAY);  // no subpixel AA in textures
  cairo_font_options_set_hint_style(options, CAIRO_HINT_STYLE_SLIGHT);
  cairo_font_options_set_hint_metrics(options, CAIRO_HINT_METRICS_ON);
  pango_cairo_context_set_font_options(context_, options);
  cairo_font_options_destroy(options);
}

RunCache::~RunCache() {
  clear();
  g_object_unref(context_);
  g_object_unref(fontmap_);
}

PangoLayout* RunCache::make_layout(const std::string& text, const RunStyle& style) {
  set_context_for(context_, style);
  PangoLayout* layout = pango_layout_new(context_);
  PangoFontDescription* desc = font_for(style, 1.0f);
  pango_layout_set_font_description(layout, desc);
  pango_font_description_free(desc);
  pango_layout_set_auto_dir(layout, style.dir == Dir::kAuto);
  if (style.align == Align::kEnd) pango_layout_set_alignment(layout, PANGO_ALIGN_RIGHT);
  if (style.max_width > 0) {
    pango_layout_set_width(layout, static_cast<int>(style.max_width * style.scale) * PANGO_SCALE);
    if (style.wrap) {
      pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
    } else {
      pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
      pango_layout_set_single_paragraph_mode(layout, TRUE);
    }
  }
  pango_layout_set_text(layout, text.c_str(), static_cast<int>(text.size()));
  return layout;
}

float RunCache::measure(const std::string& text, const RunStyle& style) {
  RunStyle s = style;
  s.max_width = -1;
  s.wrap = false;
  std::string key = s.key() + text;
  if (auto it = map_.find(key); it != map_.end()) return it->second.run.size().x;
  if (auto it = widths_.find(key); it != widths_.end()) return it->second;
  PangoLayout* layout = make_layout(text, s);
  PangoRectangle logical;
  pango_layout_get_pixel_extents(layout, nullptr, &logical);
  g_object_unref(layout);
  const float w = float(logical.width) / s.scale;
  if (widths_.size() >= 16384) widths_.clear();
  widths_.emplace(std::move(key), w);
  return w;
}

RunCache::Metrics RunCache::metrics(const RunStyle& style) {
  const std::string key = style.family + "|" + std::to_string(style.size) + "|" + std::to_string(style.weight) + "|" +
                          std::to_string(style.scale);
  if (auto it = metrics_.find(key); it != metrics_.end()) return it->second;
  PangoFontDescription* desc = font_for(style, 1.0f);
  PangoFontMetrics* m = pango_context_get_metrics(context_, desc, nullptr);
  Metrics out;
  out.ascent = float(pango_font_metrics_get_ascent(m)) / PANGO_SCALE / style.scale;
  out.descent = float(pango_font_metrics_get_descent(m)) / PANGO_SCALE / style.scale;
  pango_font_metrics_unref(m);
  pango_font_description_free(desc);
  metrics_.emplace(key, out);
  return out;
}

TextRun RunCache::rasterize(const std::string& text, const RunStyle& style) {
  TextRun run;
  run.scale = style.scale;
  PangoLayout* layout = make_layout(text, style);
  PangoRectangle ink;
  PangoRectangle logical;
  pango_layout_get_pixel_extents(layout, &ink, &logical);
  if (style.fit_slot > 0 && !style.wrap) {
    // Fit text: shrink only the runs whose ink would overflow the slot.
    const float slot_px = style.fit_slot * style.scale;
    for (float f : {0.95f, 0.90f, 0.85f}) {
      if (float(ink.height) <= slot_px) break;
      PangoFontDescription* desc = font_for(style, f);
      pango_layout_set_font_description(layout, desc);
      pango_font_description_free(desc);
      pango_layout_get_pixel_extents(layout, &ink, &logical);
      run.shrink = f;
    }
  }
  const bool boxed = style.max_width > 0 && (style.wrap || style.align == Align::kEnd);
  int width = logical.width;
  if (boxed) width = std::max(width, static_cast<int>(style.max_width * style.scale));
  run.px_w = std::max(1, width);
  run.px_h = std::max(1, logical.height);
  run.lines = pango_layout_get_line_count(layout);
  run.ellipsized = pango_layout_is_ellipsized(layout);
  if (PangoLayoutLine* first = pango_layout_get_line_readonly(layout, 0))
    run.rtl = pango_layout_line_get_resolved_direction(first) == PANGO_DIRECTION_RTL;
  run.baseline = (float(pango_layout_get_baseline(layout)) / PANGO_SCALE - float(logical.y)) / style.scale;

  if (may_need_color(text)) {
    // ARGB pass with a black source: pixels with any colour came from a colour glyph and go to the
    // RGBA layer (un-premultiplied); pure black/alpha pixels form the tinted mask as usual.
    cairo_surface_t* argb = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, run.px_w, run.px_h);
    cairo_t* cr = cairo_create(argb);
    cairo_set_source_rgba(cr, 0, 0, 0, 1);
    cairo_move_to(cr, boxed ? 0 : -logical.x, -logical.y);
    pango_cairo_show_layout(cr, layout);
    cairo_destroy(cr);
    cairo_surface_flush(argb);
    const unsigned char* src = cairo_image_surface_get_data(argb);
    const int stride = cairo_image_surface_get_stride(argb);
    std::vector<unsigned char> mask(size_t(run.px_w) * size_t(run.px_h));
    std::vector<unsigned char> rgba(mask.size() * 4);
    bool any_color = false;
    for (int y = 0; y < run.px_h; ++y) {
      for (int x = 0; x < run.px_w; ++x) {
        uint32_t px = 0;
        std::memcpy(&px, src + size_t(y) * size_t(stride) + size_t(x) * 4, 4);  // native-endian ARGB
        const uint32_t a = px >> 24;
        const uint32_t r = (px >> 16) & 0xFF;
        const uint32_t g = (px >> 8) & 0xFF;
        const uint32_t b = px & 0xFF;
        const size_t i = size_t(y) * size_t(run.px_w) + size_t(x);
        if (r == 0 && g == 0 && b == 0) {
          mask[i] = static_cast<unsigned char>(a);
        } else {
          any_color = true;
          rgba[i * 4 + 0] = static_cast<unsigned char>(std::min<uint32_t>(255, r * 255 / a));
          rgba[i * 4 + 1] = static_cast<unsigned char>(std::min<uint32_t>(255, g * 255 / a));
          rgba[i * 4 + 2] = static_cast<unsigned char>(std::min<uint32_t>(255, b * 255 / a));
          rgba[i * 4 + 3] = static_cast<unsigned char>(a);
        }
      }
    }
    cairo_surface_destroy(argb);
    g_object_unref(layout);
    run.texture = upload_alpha_texture(mask.data(), run.px_w, run.px_h, run.px_w);
    if (any_color) run.color_texture = upload_rgba_texture(rgba.data(), run.px_w, run.px_h);
    return run;
  }
  cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_A8, run.px_w, run.px_h);
  cairo_t* cr = cairo_create(surface);
  cairo_set_source_rgba(cr, 0, 0, 0, 1);
  cairo_move_to(cr, boxed ? 0 : -logical.x, -logical.y);
  pango_cairo_show_layout(cr, layout);
  cairo_destroy(cr);
  cairo_surface_flush(surface);
  g_object_unref(layout);
  run.texture = upload_alpha_texture(cairo_image_surface_get_data(surface), run.px_w, run.px_h,
                                     cairo_image_surface_get_stride(surface));
  cairo_surface_destroy(surface);
  return run;
}

void RunCache::touch(Entry& e) {
  if (e.lru != lru_.begin()) lru_.splice(lru_.begin(), lru_, e.lru);
  e.frame = frame_;
}

const TextRun& RunCache::insert(std::string key, TextRun run) {
  bytes_ += run.bytes();
  lru_.push_front(key);
  Entry& e = map_[std::move(key)];
  e.run = run;
  e.lru = lru_.begin();
  e.frame = frame_;
  return e.run;
}

const TextRun* RunCache::try_get(const std::string& text, const RunStyle& style) {
  std::string key = style.key() + text;
  if (auto it = map_.find(key); it != map_.end()) {
    touch(it->second);
    return &it->second.run;
  }
  if (spent_ms_ >= budget_ms_) {
    ++deferred_;
    return nullptr;
  }
  const auto t0 = std::chrono::steady_clock::now();
  TextRun run = rasterize(text, style);
  spent_ms_ += ms_since(t0);
  ++cold_;
  return &insert(std::move(key), run);
}

const TextRun& RunCache::get(const std::string& text, const RunStyle& style) {
  std::string key = style.key() + text;
  if (auto it = map_.find(key); it != map_.end()) {
    touch(it->second);
    return it->second.run;
  }
  const auto t0 = std::chrono::steady_clock::now();
  TextRun run = rasterize(text, style);
  spent_ms_ += ms_since(t0);
  ++cold_;
  return insert(std::move(key), run);
}

void RunCache::new_frame(double cold_budget_ms) {
  ++frame_;
  budget_ms_ = cold_budget_ms;
  spent_ms_ = 0;
  cold_ = 0;
  deferred_ = 0;
  // Evict from the cold end; never a run used in the previous frame (it is still on screen).
  while (bytes_ > byte_budget && !lru_.empty()) {
    auto it = map_.find(lru_.back());
    if (it == map_.end() || it->second.frame + 1 >= frame_) break;
    bytes_ -= it->second.run.bytes();
    delete_texture(it->second.run.texture);
    delete_texture(it->second.run.color_texture);
    map_.erase(it);
    lru_.pop_back();
  }
}

RunCache::HitLayout& RunCache::hit_layout(const std::string& text, const RunStyle& style) {
  std::string key = style.key() + text;
  if (auto it = hits_.find(key); it != hits_.end()) {
    HitLayout& h = it->second;
    if (h.lru != hits_lru_.begin()) hits_lru_.splice(hits_lru_.begin(), hits_lru_, h.lru);
    // Layouts re-shape when the shared context changed since; give it back this style's state.
    set_context_for(context_, style);
    return h;
  }
  while (hits_.size() >= kHitLayouts) {
    auto old = hits_.find(hits_lru_.back());
    g_object_unref(old->second.layout);
    hits_.erase(old);
    hits_lru_.pop_back();
  }
  HitLayout h;
  h.layout = make_layout(text, style);
  PangoRectangle logical;
  pango_layout_get_pixel_extents(h.layout, nullptr, &logical);
  const bool boxed = style.max_width > 0 && (style.wrap || style.align == Align::kEnd);
  h.dx = boxed ? 0 : -logical.x;  // as rasterize() moves the layout into its texture
  h.dy = -logical.y;
  hits_lru_.push_front(key);
  h.lru = hits_lru_.begin();
  return hits_.emplace(std::move(key), h).first->second;
}

size_t RunCache::index_at(const std::string& text, const RunStyle& style, ImVec2 point) {
  const HitLayout& h = hit_layout(text, style);
  const int x = static_cast<int>((point.x * style.scale - float(h.dx)) * PANGO_SCALE);
  const int y = static_cast<int>((point.y * style.scale - float(h.dy)) * PANGO_SCALE);
  int index = 0;
  int trailing = 0;
  pango_layout_xy_to_index(h.layout, x, y, &index, &trailing);
  // trailing: characters to the end of the grapheme when the point is on its trailing half.
  const char* s = text.c_str();
  const char* end = s + text.size();
  const char* p = s + std::clamp(index, 0, static_cast<int>(text.size()));
  for (; trailing > 0 && p < end; --trailing) p = g_utf8_next_char(p);
  return static_cast<size_t>(std::min(p, end) - s);
}

void RunCache::range_boxes(const std::string& text, const RunStyle& style, size_t begin, size_t end, bool through_end,
                           float break_w, std::vector<TextBox>& out) {
  out.clear();
  end = std::min(end, text.size());
  if (begin >= end && !through_end) return;
  const HitLayout& h = hit_layout(text, style);
  const float s = style.scale;
  auto box = [&](int x0, int x1, int y0, int y1) {  // Pango units in layout space → run-relative logical px
    out.push_back({(float(x0) / PANGO_SCALE + float(h.dx)) / s, (float(y0) / PANGO_SCALE + float(h.dy)) / s,
                   (float(x1) / PANGO_SCALE + float(h.dx)) / s, (float(y1) / PANGO_SCALE + float(h.dy)) / s});
  };
  PangoLayoutIter* it = pango_layout_get_iter(h.layout);
  do {
    PangoLayoutLine* line = pango_layout_iter_get_line_readonly(it);
    const auto ls = static_cast<size_t>(line->start_index);
    const size_t le = ls + static_cast<size_t>(line->length);  // excludes the paragraph delimiter
    if (ls > end) break;
    int y0 = 0;
    int y1 = 0;
    pango_layout_iter_get_line_yrange(it, &y0, &y1);
    const size_t a = std::max(begin, ls);
    const size_t b = std::min(end, le);
    if (a < b) {
      int* ranges = nullptr;
      int n = 0;
      pango_layout_line_get_x_ranges(line, static_cast<int>(a), static_cast<int>(b), &ranges, &n);
      for (int i = 0; i < n; ++i) box(ranges[2 * i], ranges[2 * i + 1], y0, y1);
      g_free(ranges);
    }
    // The hard line break after this line is selected: inside the text, or after it (through_end).
    const bool hard_break = le < text.size() && text[le] == '\n';
    if (begin <= le && ((hard_break && end > le) || (le == text.size() && through_end))) {
      PangoRectangle extent;  // index_to_x is line-relative; the line's offset is in its extents
      pango_layout_iter_get_line_extents(it, nullptr, &extent);
      int x = 0;
      pango_layout_line_index_to_x(line, static_cast<int>(le), 0, &x);
      x += extent.x;
      const int w = static_cast<int>(break_w * s * PANGO_SCALE);
      const bool rtl = pango_layout_line_get_resolved_direction(line) == PANGO_DIRECTION_RTL;
      box(rtl ? x - w : x, rtl ? x : x + w, y0, y1);
    }
  } while (pango_layout_iter_next_line(it));
  pango_layout_iter_free(it);
}

std::pair<size_t, size_t> RunCache::word_at(const std::string& text, const RunStyle& style, size_t offset) {
  if (text.empty()) return {0, 0};
  const HitLayout& h = hit_layout(text, style);
  int n = 0;  // characters + 1
  const PangoLogAttr* attrs = pango_layout_get_log_attrs_readonly(h.layout, &n);
  const char* s = text.c_str();
  const long chars = n - 1;
  const long ci = std::min<long>(g_utf8_pointer_to_offset(s, s + std::min(offset, text.size())), chars);
  auto bytes = [&](long a, long b) {
    return std::pair<size_t, size_t>{static_cast<size_t>(g_utf8_offset_to_pointer(s, a) - s),
                                     static_cast<size_t>(g_utf8_offset_to_pointer(s, b) - s)};
  };
  // Inside a word: the nearest word start at or before ci with no word end between (ci itself may be
  // the word's end, when the click fell on the trailing half of its last character).
  for (long j = ci; j >= 0; --j) {
    if (attrs[j].is_word_start) {
      long e = j + 1;
      while (e < chars && !attrs[e].is_word_end) ++e;
      if (e >= ci) return bytes(j, e);
      break;
    }
    if (attrs[j].is_word_end && j < ci) break;
  }
  // Otherwise the grapheme at ci (the last one past the end).
  long a = std::min(ci, chars - 1);
  while (a > 0 && !attrs[a].is_cursor_position) --a;
  long b = a + 1;
  while (b < chars && !attrs[b].is_cursor_position) ++b;
  return bytes(a, b);
}

void RunCache::clear() {
  for (auto& [key, h] : hits_) g_object_unref(h.layout);
  hits_.clear();
  hits_lru_.clear();
  widths_.clear();
  for (auto& [key, e] : map_) {
    delete_texture(e.run.texture);
    delete_texture(e.run.color_texture);
  }
  map_.clear();
  lru_.clear();
  bytes_ = 0;
}

}  // namespace imgui_accesskit::text
