#include "imgui_accesskit/line_edit.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <pango/pangocairo.h>

#include "imgui_internal.h"
#include "imgui_accesskit/run_cache.h"

namespace imgui_accesskit::text {

// Own context on the shared font map: the layout lives across frames and must not see other callers'
// base-direction changes on the cache's context.
LineEdit::LineEdit(RunCache& cache, std::string placeholder)
    : context_(pango_font_map_create_context(pango_context_get_font_map(cache.context()))),
      placeholder_(std::move(placeholder)),
      cache_(cache) {
  cairo_font_options_t* options = cairo_font_options_create();
  cairo_font_options_set_antialias(options, CAIRO_ANTIALIAS_GRAY);
  cairo_font_options_set_hint_style(options, CAIRO_HINT_STYLE_SLIGHT);
  cairo_font_options_set_hint_metrics(options, CAIRO_HINT_METRICS_ON);
  pango_cairo_context_set_font_options(context_, options);
  cairo_font_options_destroy(options);
}

LineEdit::~LineEdit() {
  delete_texture(texture_);
  if (layout_) g_object_unref(layout_);
  g_object_unref(context_);
}

void LineEdit::set_font(const std::string& family, float size_px) {
  if (family == family_ && size_px == size_) return;
  family_ = family;
  size_ = size_px;
  font_dirty_ = dirty_ = true;
}

void LineEdit::ensure_layout(float scale) {
  if (scale <= 0) scale = scale_ > 0 ? scale_ : 1.0f;
  if (!dirty_ && scale == scale_ && layout_) return;
  const bool rescale = scale != scale_;
  scale_ = scale;
  if (!layout_) {
    layout_ = pango_layout_new(context_);
    pango_layout_set_auto_dir(layout_, TRUE);
    pango_layout_set_single_paragraph_mode(layout_, TRUE);
  }
  if (font_dirty_ || rescale) {
    PangoFontDescription* desc = pango_font_description_new();
    pango_font_description_set_family(desc, family_.c_str());
    pango_font_description_set_absolute_size(desc, double(size_ * scale) * PANGO_SCALE);
    pango_layout_set_font_description(layout_, desc);
    // Field height from the primary font, so typing a script with a taller fallback face does not
    // make the field jump; the run is aligned on its baseline.
    PangoFontMetrics* m = pango_context_get_metrics(context_, desc, nullptr);
    ascent_ = float(pango_font_metrics_get_ascent(m)) / PANGO_SCALE / scale;
    line_h_ = ascent_ + float(pango_font_metrics_get_descent(m)) / PANGO_SCALE / scale;
    pango_font_metrics_unref(m);
    pango_font_description_free(desc);
    font_dirty_ = false;
  }
  std::string display = text_;
  if (masked_) {  // same byte length per character: '*' (1), U+00B7 (2), U+2022 (3), U+1F784 (4)
    display.clear();
    for (size_t i = 0; i < text_.size();) {
      const auto c = static_cast<unsigned char>(text_[i]);
      const size_t n = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
      display += n == 1 ? "*" : n == 2 ? "\u00B7" : n == 3 ? "\u2022" : "\U0001F784";
      i += n;
    }
  }
  if (!preedit_.empty()) display.insert(size_t(cursor_), preedit_);
  pango_layout_set_text(layout_, display.c_str(), static_cast<int>(display.size()));
  PangoAttrList* attrs = pango_attr_list_new();
  if (!preedit_.empty()) {  // IME composition is underlined, like GTK
    PangoAttribute* u = pango_attr_underline_new(PANGO_UNDERLINE_SINGLE);
    u->start_index = guint(cursor_);
    u->end_index = guint(cursor_ + static_cast<int>(preedit_.size()));
    pango_attr_list_insert(attrs, u);
  }
  pango_layout_set_attributes(layout_, attrs);
  pango_attr_list_unref(attrs);
  PangoRectangle logical;
  pango_layout_get_pixel_extents(layout_, nullptr, &logical);
  baseline_ = (float(pango_layout_get_baseline(layout_)) / PANGO_SCALE - float(logical.y)) / scale;
  const int w = std::max(1, logical.width);
  const int h = std::max(1, logical.height);
  cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_A8, w, h);
  cairo_t* cr = cairo_create(surface);
  cairo_set_source_rgba(cr, 0, 0, 0, 1);
  cairo_move_to(cr, -logical.x, -logical.y);
  pango_cairo_show_layout(cr, layout_);
  cairo_destroy(cr);
  cairo_surface_flush(surface);
  texture_ = upload_alpha_texture(cairo_image_surface_get_data(surface), w, h, cairo_image_surface_get_stride(surface),
                                  texture_);
  cairo_surface_destroy(surface);
  tex_w_ = w;
  tex_h_ = h;
  dirty_ = false;
}

int LineEdit::display_index(int m) const {
  return (!preedit_.empty() && m >= cursor_) ? m + static_cast<int>(preedit_.size()) : m;
}
int LineEdit::model_index(int d) const {
  if (preedit_.empty() || d <= cursor_) return d;
  const int len = static_cast<int>(preedit_.size());
  return d < cursor_ + len ? cursor_ : d - len;
}

int LineEdit::next_cursor_pos(int byte_index, int dir) {
  ensure_layout(0);
  int n = 0;
  const PangoLogAttr* attrs = pango_layout_get_log_attrs_readonly(layout_, &n);
  const char* s = text_.c_str();
  long ch = g_utf8_pointer_to_offset(s, s + byte_index);
  const long total = g_utf8_strlen(s, -1);
  do ch += dir;
  while (ch > 0 && ch < total && ch < n && !attrs[ch].is_cursor_position);
  ch = std::clamp(ch, 0L, total);
  return static_cast<int>(g_utf8_offset_to_pointer(s, ch) - s);
}

int LineEdit::word_boundary(int byte_index, int dir) {
  ensure_layout(0);
  int n = 0;
  const PangoLogAttr* attrs = pango_layout_get_log_attrs_readonly(layout_, &n);
  const char* s = text_.c_str();
  long ch = g_utf8_pointer_to_offset(s, s + byte_index);
  const long total = g_utf8_strlen(s, -1);
  if (dir < 0) {
    do --ch;
    while (ch > 0 && ch < n && !attrs[ch].is_word_start);
  } else {
    do ++ch;
    while (ch < total && ch < n && !attrs[ch].is_word_end);
  }
  ch = std::clamp(ch, 0L, total);
  return static_cast<int>(g_utf8_offset_to_pointer(s, ch) - s);
}

void LineEdit::set_text(const std::string& text) {
  text_ = text;
  preedit_.clear();
  cursor_ = anchor_ = static_cast<int>(text_.size());
  changed();
}

std::string LineEdit::selected_text() const {
  const int a = std::min(anchor_, cursor_);
  const int b = std::max(anchor_, cursor_);
  return text_.substr(size_t(a), size_t(b - a));
}

void LineEdit::insert(const std::string& s) {
  if (anchor_ != cursor_) delete_range(anchor_, cursor_);
  text_.insert(size_t(cursor_), s);
  cursor_ += static_cast<int>(s.size());
  anchor_ = cursor_;
  changed();
}

void LineEdit::delete_range(int a, int b) {
  if (a > b) std::swap(a, b);
  text_.erase(size_t(a), size_t(b - a));
  cursor_ = anchor_ = a;
  changed();
}

// GTK behaviour: a whole grapheme cluster, except where Pango sets backspace_deletes_character (Indic
// conjuncts and similar), where only the last character of the decomposed cluster goes.
void LineEdit::backspace() {
  const int prev = next_cursor_pos(cursor_, -1);
  if (prev >= cursor_) return;
  int n = 0;
  const PangoLogAttr* attrs = pango_layout_get_log_attrs_readonly(layout_, &n);
  const char* s = text_.c_str();
  const long ch = g_utf8_pointer_to_offset(s, s + cursor_);
  if (ch < n && attrs[ch].backspace_deletes_character) {
    const std::string cluster = text_.substr(size_t(prev), size_t(cursor_ - prev));
    char* nfd = g_utf8_normalize(cluster.c_str(), -1, G_NORMALIZE_NFD);
    if (nfd) {
      const long len = g_utf8_strlen(nfd, -1);
      const std::string keep(nfd, size_t(g_utf8_offset_to_pointer(nfd, len - 1) - nfd));
      g_free(nfd);
      delete_range(prev, cursor_);
      if (!keep.empty()) insert(keep);
      return;
    }
  }
  delete_range(prev, cursor_);
}

void LineEdit::move_visual(int dir, bool extend) {
  ensure_layout(0);
  int trailing = 0;
  int idx = 0;
  pango_layout_move_cursor_visually(layout_, TRUE, cursor_, 0, dir, &idx, &trailing);
  if (idx < 0 || idx == G_MAXINT) return;  // already at the visual edge
  const char* s = text_.c_str();
  while (trailing-- > 0 && idx < static_cast<int>(text_.size())) idx = static_cast<int>(g_utf8_next_char(s + idx) - s);
  cursor_ = idx;
  if (!extend) anchor_ = cursor_;
}

void LineEdit::collapse_or_move(int dir, bool extend) {
  if (extend || anchor_ == cursor_) return move_visual(dir, extend);
  ensure_layout(0);
  PangoRectangle a;
  PangoRectangle c;
  pango_layout_get_cursor_pos(layout_, anchor_, &a, nullptr);
  pango_layout_get_cursor_pos(layout_, cursor_, &c, nullptr);
  const int target = ((dir < 0) == (a.x < c.x)) ? anchor_ : cursor_;
  cursor_ = anchor_ = target;
}

int LineEdit::hit_test(float window_x) {
  ensure_layout(0);
  const int x = static_cast<int>((window_x - text_origin_.x) * scale_ * PANGO_SCALE);
  const int y = static_cast<int>(baseline_ * scale_ * PANGO_SCALE);
  int idx = 0;
  int trailing = 0;
  pango_layout_xy_to_index(layout_, x, y, &idx, &trailing);
  const char* s = pango_layout_get_text(layout_);
  const int len = static_cast<int>(std::strlen(s));
  while (trailing-- > 0 && idx < len) idx = static_cast<int>(g_utf8_next_char(s + idx) - s);
  return model_index(idx);
}

bool LineEdit::base_rtl() {
  ensure_layout(0);
  PangoLayoutLine* line = pango_layout_get_line_readonly(layout_, 0);
  return line && pango_layout_line_get_resolved_direction(line) == PANGO_DIRECTION_RTL;
}

bool LineEdit::handle_event(const SDL_Event& e) {
  auto inside = [&](float x, float y) {
    return x >= field_.x && y >= field_.y && x < field_.x + field_.z && y < field_.y + field_.w;
  };
  switch (e.type) {
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
      if (e.button.button != SDL_BUTTON_LEFT) return false;
      if (!inside(e.button.x, e.button.y)) {
        focused_ = false;
        return false;
      }
      focused_ = true;
      cursor_ = hit_test(e.button.x);
      if (e.button.clicks == 2) {
        anchor_ = word_boundary(next_cursor_pos(cursor_, +1), -1);
        cursor_ = word_boundary(anchor_, +1);
      } else if (!(SDL_GetModState() & SDL_KMOD_SHIFT)) {
        anchor_ = cursor_;
      }
      dragging_ = true;
      return true;
    case SDL_EVENT_MOUSE_MOTION:
      if (!dragging_) return false;
      cursor_ = hit_test(e.motion.x);
      return true;
    case SDL_EVENT_MOUSE_BUTTON_UP:
      if (!dragging_) return false;
      dragging_ = false;
      return true;
    default: break;
  }
  if (!focused_) return false;
  switch (e.type) {
    case SDL_EVENT_TEXT_EDITING: {
      if (masked_) return true;  // a passphrase's composition is never drawn
      preedit_ = e.edit.text ? e.edit.text : "";
      const long len = g_utf8_strlen(preedit_.c_str(), -1);
      preedit_cursor_chars_ = static_cast<int>(e.edit.start < 0 ? len : std::min<long>(e.edit.start, len));
      if (!preedit_.empty() && anchor_ != cursor_) delete_range(anchor_, cursor_);
      dirty_ = true;
      return true;
    }
    case SDL_EVENT_TEXT_INPUT:
      preedit_.clear();
      insert(e.text.text ? e.text.text : "");
      return true;
    case SDL_EVENT_KEY_DOWN: {
      if (!preedit_.empty()) return true;  // the IME owns the keyboard while composing
      const bool ctrl = e.key.mod & SDL_KMOD_CTRL;
      const bool shift = e.key.mod & SDL_KMOD_SHIFT;
      const int len = static_cast<int>(text_.size());
      switch (e.key.key) {
        case SDLK_LEFT:
        case SDLK_RIGHT: {
          const int dir = e.key.key == SDLK_LEFT ? -1 : 1;
          if (ctrl) {  // words move logically; flipped in RTL paragraphs so Ctrl+Left goes left
            cursor_ = word_boundary(cursor_, base_rtl() ? -dir : dir);
            if (!shift) anchor_ = cursor_;
          } else {
            collapse_or_move(dir, shift);
          }
          return true;
        }
        case SDLK_HOME:
          cursor_ = 0;
          if (!shift) anchor_ = cursor_;
          return true;
        case SDLK_END:
          cursor_ = len;
          if (!shift) anchor_ = cursor_;
          return true;
        case SDLK_BACKSPACE:
          if (anchor_ != cursor_) delete_range(anchor_, cursor_);
          else if (ctrl) delete_range(word_boundary(cursor_, -1), cursor_);
          else backspace();
          return true;
        case SDLK_DELETE:
          if (anchor_ != cursor_) delete_range(anchor_, cursor_);
          else delete_range(cursor_, next_cursor_pos(cursor_, +1));
          return true;
        case SDLK_A:
          if (!ctrl) return false;
          anchor_ = 0;
          cursor_ = len;
          return true;
        case SDLK_C:
        case SDLK_X:
          if (!ctrl) return false;
          if (anchor_ != cursor_ && !masked_) {
            SDL_SetClipboardText(selected_text().c_str());
            if (e.key.key == SDLK_X) delete_range(anchor_, cursor_);
          }
          return true;
        case SDLK_V: {
          if (!ctrl) return false;
          char* clip = SDL_GetClipboardText();
          std::string s = clip ? clip : "";
          SDL_free(clip);
          std::replace(s.begin(), s.end(), '\n', ' ');
          s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
          insert(s);
          return true;
        }
        default: return false;
      }
    }
    default: return false;
  }
}

void LineEdit::draw(const char* id, float width, float height, const LineEditColors& colors) {
  ImGuiContext& g = *GImGui;
  ImGuiWindow* window = ImGui::GetCurrentWindow();
  const ImGuiID gid = window->GetID(id);
  ensure_layout(ImGui::GetIO().DisplayFramebufferScale.x);
  const ImVec2 pos = window->DC.CursorPos;
  const ImRect bb(pos, ImVec2(pos.x + width, pos.y + height));
  ImGui::ItemSize(bb);
  if (!ImGui::ItemAdd(bb, gid)) return;
  if (ImGui::ItemHoverable(bb, gid, 0)) ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
  if (focused_) {  // keep ImGui navigation from consuming arrows, Home and End while editing
    if (g.ActiveId != gid) ImGui::SetActiveID(gid, window);
    ImGui::SetFocusID(gid, window);
    ImGui::SetActiveIdUsingAllKeyboardKeys();
  } else if (g.ActiveId == gid) {
    ImGui::ClearActiveID();
  }
  field_ = ImVec4(bb.Min.x, bb.Min.y, bb.GetWidth(), bb.GetHeight());

  ImDrawList* dl = window->DrawList;
  dl->AddRectFilled(bb.Min, bb.Max, colors.field, 3.0f);
  dl->AddRect(bb.Min, bb.Max, focused_ ? colors.focus_ring : colors.border, 3.0f, focused_ ? 2.0f : 1.0f);

  const float pad = 8.0f;
  const float tw = float(tex_w_) / scale_;
  const float avail = width - pad * 2;
  PangoRectangle strong;
  PangoRectangle weak;
  int caret_index = display_index(cursor_);
  if (!preedit_.empty())
    caret_index = cursor_ + static_cast<int>(g_utf8_offset_to_pointer(preedit_.c_str(), preedit_cursor_chars_) -
                                             preedit_.c_str());
  pango_layout_get_cursor_pos(layout_, caret_index, &strong, &weak);
  const float strong_x = float(strong.x) / PANGO_SCALE / scale_;
  const float weak_x = float(weak.x) / PANGO_SCALE / scale_;
  float align = 0;
  if (tw <= avail) {
    scroll_x_ = 0;
    if (base_rtl()) align = avail - tw;  // RTL paragraphs start at the right edge, like GtkEntry
  } else {
    if (strong_x - scroll_x_ > avail - 2) scroll_x_ = strong_x - avail + 2;
    if (strong_x - scroll_x_ < 0) scroll_x_ = strong_x;
    scroll_x_ = std::clamp(scroll_x_, 0.0f, tw - avail + 2);
  }
  text_origin_ = ImVec2(bb.Min.x + pad + align - scroll_x_, bb.Min.y + (height - line_h_) * 0.5f);
  const ImVec2 o = text_origin_;
  dl->PushClipRect(ImVec2(bb.Min.x + 1, bb.Min.y + 1), ImVec2(bb.Max.x - 1, bb.Max.y - 1), true);
  if (text_.empty() && preedit_.empty() && !placeholder_.empty()) {
    RunStyle ps;
    ps.family = family_;
    ps.size = size_;
    ps.scale = scale_;
    const TextRun& run = cache_.get(placeholder_, ps);
    run.draw(dl, ImVec2(bb.Min.x + pad, o.y + ascent_ - run.baseline), colors.placeholder);
  }
  if (anchor_ != cursor_ && preedit_.empty()) {  // a bidi selection may be several visual ranges
    PangoLayoutLine* line = pango_layout_get_line_readonly(layout_, 0);
    int* ranges = nullptr;
    int n = 0;
    pango_layout_line_get_x_ranges(line, std::min(anchor_, cursor_), std::max(anchor_, cursor_), &ranges, &n);
    for (int i = 0; i < n; ++i) {
      const float x0 = float(ranges[2 * i]) / PANGO_SCALE / scale_;
      const float x1 = float(ranges[2 * i + 1]) / PANGO_SCALE / scale_;
      dl->AddRectFilled(ImVec2(o.x + x0, o.y), ImVec2(o.x + x1, o.y + line_h_), colors.selection);
    }
    g_free(ranges);
  }
  const float img_y = o.y + ascent_ - baseline_;
  dl->AddImage(ImTextureRef(ImTextureID(texture_)), ImVec2(o.x, img_y),
               ImVec2(o.x + tw, img_y + float(tex_h_) / scale_), ImVec2(0, 0), ImVec2(1, 1), colors.text);
  if (focused_) {
    dl->AddRectFilled(ImVec2(o.x + strong_x, o.y), ImVec2(o.x + strong_x + 1.5f, o.y + line_h_), colors.caret);
    if (std::fabs(weak_x - strong_x) > 0.5f)  // split caret at a bidi boundary (GTK draws both)
      dl->AddRectFilled(ImVec2(o.x + weak_x, o.y + line_h_ * 0.5f), ImVec2(o.x + weak_x + 1.0f, o.y + line_h_),
                        colors.caret);
    ImGuiPlatformImeData* ime = &g.PlatformImeData;
    ime->WantVisible = ime->WantTextInput = true;
    ime->InputPos = ImVec2(o.x + strong_x, o.y);
    ime->InputLineHeight = line_h_;
    ime->ViewportId = window->Viewport->ID;
  }
  dl->PopClipRect();
}

}  // namespace imgui_accesskit::text
