// LineEdit headless (RecordingTextures instead of GL; SDL's dummy video driver for the clipboard):
// grapheme, word and visual (bidi) caret moves, deletion, selection, IME composition and masking.
#include "imgui_accesskit/line_edit.h"

#include <string>
#include <vector>

#include <SDL3/SDL.h>
#include <gtest/gtest.h>

#include "imgui_accesskit/run_cache.h"
#include "recording_textures.h"

namespace imgui_accesskit::text {
namespace {

using test_support::RecordingTextures;

constexpr SDL_Keymod kCtrl = SDL_KMOD_LCTRL;
constexpr SDL_Keymod kShift = SDL_KMOD_LSHIFT;

std::string clipboard() {
  char* s = SDL_GetClipboardText();
  std::string out = s != nullptr ? s : "";
  SDL_free(s);
  return out;
}

class LineEditTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() { ASSERT_TRUE(SDL_Init(SDL_INIT_VIDEO)) << SDL_GetError(); }
  static void TearDownTestSuite() { SDL_Quit(); }
  void SetUp() override { edit.set_focused(true); }

  bool press(SDL_Keycode key, SDL_Keymod mod = SDL_KMOD_NONE) {
    SDL_Event e;
    SDL_zero(e);
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.key = key;
    e.key.mod = mod;
    e.key.down = true;
    return edit.handle_event(e);
  }
  bool type(const char* s) {
    SDL_Event e;
    SDL_zero(e);
    e.type = SDL_EVENT_TEXT_INPUT;
    e.text.text = s;
    return edit.handle_event(e);
  }
  bool compose(const char* s, int start) {
    SDL_Event e;
    SDL_zero(e);
    e.type = SDL_EVENT_TEXT_EDITING;
    e.edit.text = s;
    e.edit.start = start;
    return edit.handle_event(e);
  }
  // The alpha raster of the field's current text.
  std::vector<unsigned char> raster() {
    press(SDLK_HOME);
    press(SDLK_LEFT);  // lays the field out (and uploads it) without changing the text
    const test_support::Texture* t = textures.find(textures.last_upload());
    return t != nullptr ? t->pixels : std::vector<unsigned char>{};
  }

  RecordingTextures textures;
  RunCache cache{textures.ops()};
  LineEdit edit{cache, "Search"};
};

TEST_F(LineEditTest, MovesAndDeletesWholeGraphemes) {
  EXPECT_TRUE(type("ab"));
  EXPECT_TRUE(type("e\u0301"));  // e + combining acute: one grapheme, bytes [2, 5)
  EXPECT_TRUE(type("c"));
  EXPECT_EQ(edit.text(), "abe\u0301c");
  EXPECT_EQ(edit.cursor(), 6);
  press(SDLK_LEFT);
  EXPECT_EQ(edit.cursor(), 5);
  press(SDLK_LEFT);
  EXPECT_EQ(edit.cursor(), 2);
  press(SDLK_RIGHT);
  EXPECT_EQ(edit.cursor(), 5);
  press(SDLK_BACKSPACE);  // after a Latin base: the whole cluster
  EXPECT_EQ(edit.text(), "abc");
  EXPECT_EQ(edit.cursor(), 2);
  press(SDLK_DELETE);
  EXPECT_EQ(edit.text(), "ab");

  edit.set_text("e\u0301x");
  press(SDLK_HOME);
  press(SDLK_DELETE);
  EXPECT_EQ(edit.text(), "x");
}

TEST_F(LineEditTest, BackspaceDeletesOneCharacterOfAnIndicCluster) {
  edit.set_text("\u0915\u093F");  // KA + vowel sign I: one grapheme
  press(SDLK_LEFT);
  EXPECT_EQ(edit.cursor(), 0);
  press(SDLK_END);
  press(SDLK_BACKSPACE);
  EXPECT_EQ(edit.text(), "\u0915");
}

TEST_F(LineEditTest, CtrlMovesAndDeletesByWord) {
  edit.set_text("hello brave world");
  press(SDLK_HOME);
  press(SDLK_RIGHT, kCtrl);
  EXPECT_EQ(edit.cursor(), 5);
  press(SDLK_RIGHT, kCtrl);
  EXPECT_EQ(edit.cursor(), 11);
  press(SDLK_LEFT, kCtrl);
  EXPECT_EQ(edit.cursor(), 6);
  EXPECT_EQ(edit.anchor(), 6);
  press(SDLK_END);
  press(SDLK_BACKSPACE, kCtrl);
  EXPECT_EQ(edit.text(), "hello brave ");

  edit.set_text("one two");
  press(SDLK_LEFT, static_cast<SDL_Keymod>(kCtrl | kShift));
  EXPECT_EQ(edit.anchor(), 7);
  EXPECT_EQ(edit.cursor(), 4);
  EXPECT_EQ(edit.selected_text(), "two");
}

TEST_F(LineEditTest, WordKeysFollowTheDirectionOfAnRtlParagraph) {
  edit.set_text("\u05E9\u05DC\u05D5\u05DD \u05E2\u05D5\u05DC\u05DD");  // two Hebrew words, 8 + 1 + 8 bytes
  press(SDLK_HOME);
  press(SDLK_LEFT, kCtrl);  // leftwards is forwards in RTL
  EXPECT_EQ(edit.cursor(), 8);
  press(SDLK_RIGHT, kCtrl);
  EXPECT_EQ(edit.cursor(), 0);
}

TEST_F(LineEditTest, ArrowsMoveVisuallyThroughBidiText) {
  edit.set_text("abc \u05D0\u05D1\u05D2");  // "abc " then an RTL run: bytes 4, 6, 8, 10
  press(SDLK_HOME);
  std::vector<int> seen{edit.cursor()};
  for (int i = 0; i < 12; ++i) {
    press(SDLK_RIGHT);
    if (edit.cursor() == seen.back()) break;
    seen.push_back(edit.cursor());
  }
  // Left to right on screen: inside the RTL run (drawn gimel bet alef) the offsets fall; the end of
  // the text is the right edge of the LTR paragraph.
  EXPECT_EQ(seen, (std::vector<int>{0, 1, 2, 3, 4, 8, 6, 10})) << ::testing::PrintToString(seen);
}

TEST_F(LineEditTest, SelectionCollapsesAndIsReplacedByTyping) {
  edit.set_text("hello");
  press(SDLK_LEFT, kShift);
  press(SDLK_LEFT, kShift);
  EXPECT_EQ(edit.anchor(), 5);
  EXPECT_EQ(edit.cursor(), 3);
  EXPECT_EQ(edit.selected_text(), "lo");
  press(SDLK_RIGHT);  // collapses to the selection's right edge
  EXPECT_EQ(edit.cursor(), 5);
  EXPECT_EQ(edit.anchor(), 5);
  press(SDLK_A, kCtrl);
  EXPECT_EQ(edit.selected_text(), "hello");
  const unsigned rev = edit.revision();
  type("X");
  EXPECT_EQ(edit.text(), "X");
  EXPECT_GT(edit.revision(), rev);
}

TEST_F(LineEditTest, CompositionOwnsTheKeyboardUntilCommitted) {
  type("ab");
  const unsigned rev = edit.revision();
  EXPECT_TRUE(compose("\u304B", 1));
  EXPECT_EQ(edit.text(), "ab") << "the preedit is not text";
  EXPECT_EQ(edit.revision(), rev);
  EXPECT_TRUE(press(SDLK_LEFT));
  EXPECT_TRUE(press(SDLK_BACKSPACE));
  EXPECT_EQ(edit.cursor(), 2);
  EXPECT_EQ(edit.text(), "ab");
  type("\u304B");
  EXPECT_EQ(edit.text(), "ab\u304B");
  EXPECT_EQ(edit.cursor(), 5);
}

TEST_F(LineEditTest, PasteFlattensLineBreaksAndCopyUsesTheSelection) {
  ASSERT_TRUE(SDL_SetClipboardText("one\r\ntwo\n"));
  press(SDLK_V, kCtrl);
  EXPECT_EQ(edit.text(), "one two ");
  press(SDLK_A, kCtrl);
  press(SDLK_C, kCtrl);
  EXPECT_EQ(clipboard(), "one two ");
  press(SDLK_X, kCtrl);
  EXPECT_EQ(edit.text(), "");
}

TEST_F(LineEditTest, MaskedFieldHidesTextAndRefusesCopy) {
  edit.set_masked(true);
  edit.set_text("abc");
  const std::vector<unsigned char> abc = raster();
  ASSERT_FALSE(abc.empty());
  edit.set_text("xyz");
  EXPECT_EQ(raster(), abc) << "every character is drawn as the same bullet";
  edit.set_masked(false);
  EXPECT_NE(raster(), abc);

  edit.set_masked(true);
  edit.set_text("\u00E9\u20AC\U0001F600");  // 2 + 3 + 4 bytes: the bullets keep the model's indices
  press(SDLK_LEFT);
  EXPECT_EQ(edit.cursor(), 5);
  press(SDLK_LEFT);
  EXPECT_EQ(edit.cursor(), 2);
  press(SDLK_LEFT);
  EXPECT_EQ(edit.cursor(), 0);

  ASSERT_TRUE(SDL_SetClipboardText("before"));
  press(SDLK_A, kCtrl);
  EXPECT_TRUE(press(SDLK_C, kCtrl));
  EXPECT_TRUE(press(SDLK_X, kCtrl));
  EXPECT_EQ(clipboard(), "before");
  EXPECT_EQ(edit.text(), "\u00E9\u20AC\U0001F600");
  const unsigned rev = edit.revision();
  EXPECT_TRUE(compose("\u304B", 1));
  EXPECT_EQ(edit.revision(), rev) << "a passphrase's composition is never shown";
  EXPECT_EQ(edit.selected_text(), "\u00E9\u20AC\U0001F600");
}

TEST_F(LineEditTest, UnfocusedFieldIgnoresKeysAndText) {
  edit.set_text("abc");
  edit.set_focused(false);
  EXPECT_FALSE(press(SDLK_LEFT));
  EXPECT_FALSE(type("x"));
  EXPECT_EQ(edit.text(), "abc");
  EXPECT_EQ(edit.cursor(), 3);
}

}  // namespace
}  // namespace imgui_accesskit::text
