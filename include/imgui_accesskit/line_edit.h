// Pango single-line editor. The text stays UTF-8;
// Pango does everything script-dependent: shaping, bidi, cursor positions by grapheme cluster, visual
// caret movement, split carets, selection ranges and hit testing. SDL text and IME composition events
// are consumed directly; the caret rectangle is published through ImGui's PlatformImeData so the SDL3
// backend positions the IME window. ImGui InputText is never used for user text.
#pragma once

#include <string>

#include <SDL3/SDL.h>

#include "imgui.h"

using PangoContext = struct _PangoContext;  // NOLINT(bugprone-reserved-identifier)
using PangoLayout = struct _PangoLayout;    // NOLINT(bugprone-reserved-identifier)

namespace imgui_accesskit::text {

class RunCache;

struct LineEditColors {
  ImU32 text, placeholder, field, border, focus_ring, selection, caret, preedit_underline;
};

class LineEdit {
 public:
  LineEdit(RunCache& cache, std::string placeholder);
  ~LineEdit();
  LineEdit(const LineEdit&) = delete;
  LineEdit& operator=(const LineEdit&) = delete;

  const std::string& text() const { return text_; }
  void set_text(const std::string& text);
  int cursor() const { return cursor_; }  // byte offsets into text()
  int anchor() const { return anchor_; }
  std::string selected_text() const;
  void set_focused(bool on) { focused_ = on; }
  bool focused() const { return focused_; }
  void set_font(const std::string& family, float size_px);
  // Passphrase field: every character is drawn as a bullet of the same UTF-8 length (so Pango's indices stay the
  // model's), IME composition is not shown, and copy/cut do nothing. set_text("") wipes the old text.
  void set_masked(bool on) {
    masked_ = on;
    dirty_ = true;
  }
  // Bumped on every text change (callers re-filter when it moves).
  unsigned revision() const { return revision_; }

  // Consumes key, text, IME and pointer events aimed at the field; true if consumed.
  bool handle_event(const SDL_Event& e);
  // Lays out the field at the ImGui cursor (width in logical px). Inside an ImGui window.
  void draw(const char* id, float width, float height, const LineEditColors& colors);
  ImVec4 field_rect() const { return field_; }  // x, y, w, h in window coordinates

 private:
  void ensure_layout(float scale);
  int display_index(int model_index) const;
  int model_index(int display_index) const;
  int next_cursor_pos(int byte_index, int dir);
  int word_boundary(int byte_index, int dir);
  void move_visual(int dir, bool extend);
  void collapse_or_move(int dir, bool extend);
  void insert(const std::string& s);
  void delete_range(int a, int b);
  void backspace();
  int hit_test(float window_x);
  bool base_rtl();
  void changed() {
    dirty_ = true;
    ++revision_;
  }

  PangoContext* context_ = nullptr;
  PangoLayout* layout_ = nullptr;
  std::string family_ = "Noto Sans", placeholder_;
  float size_ = 14;
  std::string text_, preedit_;
  int cursor_ = 0, anchor_ = 0, preedit_cursor_chars_ = 0;
  bool masked_ = false;
  bool focused_ = false, dirty_ = true, font_dirty_ = true, dragging_ = false;
  unsigned revision_ = 0;
  float scale_ = 0, scroll_x_ = 0, line_h_ = 0, ascent_ = 0, baseline_ = 0;
  unsigned texture_ = 0;
  int tex_w_ = 0, tex_h_ = 0;
  RunCache& cache_;
  ImVec2 text_origin_{0, 0};
  ImVec4 field_{0, 0, 0, 0};
};

}  // namespace imgui_accesskit::text
