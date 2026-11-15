// RunCache headless (RecordingTextures instead of GL): rasterising and caching, measuring, the cold
// budget and LRU eviction, and hit-testing (index_at, range_boxes, word_at). Fonts: Noto (the gate image).
#include "imgui_accesskit/run_cache.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "recording_textures.h"

namespace imgui_accesskit::text {
namespace {

using test_support::RecordingTextures;
using test_support::Texture;
using Range = std::pair<size_t, size_t>;

RunStyle sans(float scale = 1) {
  RunStyle s;
  s.family = "Noto Sans";
  s.size = 16;
  s.scale = scale;
  return s;
}

TEST(RunCache, GetRasterisesOnceAndCaches) {
  RecordingTextures tex;
  RunCache cache(tex.ops());
  const TextRun& run = cache.get("Hello", sans());
  EXPECT_NE(run.texture, 0u);
  EXPECT_EQ(run.color_texture, 0u);
  EXPECT_GT(run.px_w, 10);
  EXPECT_GT(run.px_h, 10);
  EXPECT_EQ(run.lines, 1);
  EXPECT_FALSE(run.rtl);
  EXPECT_GT(run.baseline, 0.0f);
  EXPECT_LT(run.baseline, run.size().y);
  const Texture* t = tex.find(run.texture);
  ASSERT_NE(t, nullptr);
  EXPECT_EQ(t->width, run.px_w);
  EXPECT_EQ(t->height, run.px_h);
  EXPECT_TRUE(std::any_of(t->pixels.begin(), t->pixels.end(), [](unsigned char a) { return a != 0; }));
  const size_t bytes = run.bytes();
  EXPECT_EQ(bytes, static_cast<size_t>(run.px_w) * static_cast<size_t>(run.px_h));

  cache.get("Hello", sans());
  EXPECT_EQ(tex.uploads(), 1);
  EXPECT_EQ(cache.size(), 1u);
  EXPECT_EQ(cache.texture_bytes(), bytes);
  const TextRun& hidpi = cache.get("Hello", sans(2));
  EXPECT_EQ(tex.uploads(), 2);
  EXPECT_EQ(cache.size(), 2u);
  EXPECT_NEAR(hidpi.size().x, run.size().x, 1.0f) << "the same logical size at twice the pixels";
  EXPECT_GT(hidpi.px_w, run.px_w);

  cache.clear();
  EXPECT_EQ(cache.size(), 0u);
  EXPECT_EQ(cache.texture_bytes(), 0u);
  EXPECT_EQ(tex.live(), 0u);
}

TEST(RunCache, MeasureMatchesTheRunWithoutRasterising) {
  RecordingTextures tex;
  RunCache cache(tex.ops());
  const float w = cache.measure("Hello, world", sans());
  EXPECT_EQ(tex.uploads(), 0);
  EXPECT_GT(w, 40.0f);
  EXPECT_GT(cache.measure("Hello, world!!", sans()), w);
  RunStyle wrapped = sans();
  wrapped.max_width = 20;
  wrapped.wrap = true;
  EXPECT_FLOAT_EQ(cache.measure("Hello, world", wrapped), w) << "always the unwrapped single-line width";
  EXPECT_FLOAT_EQ(cache.get("Hello, world", sans()).size().x, w);
  EXPECT_NEAR(cache.measure("Hello, world", sans(2)), w, 2.0f) << "logical px at any scale";
}

TEST(RunCache, MetricsAreLogicalPixels) {
  RecordingTextures tex;
  RunCache cache(tex.ops());
  const RunCache::Metrics m = cache.metrics(sans());
  EXPECT_GT(m.ascent, 10.0f);
  EXPECT_GT(m.descent, 2.0f);
  EXPECT_LT(m.ascent + m.descent, 30.0f);
  EXPECT_NEAR(cache.metrics(sans(2)).ascent, m.ascent, 0.5f);
}

TEST(RunCache, ColdBudgetDefersUncachedRuns) {
  RecordingTextures tex;
  RunCache cache(tex.ops());
  cache.get("cached", sans());
  cache.new_frame(0);
  EXPECT_EQ(cache.try_get("cold", sans()), nullptr);
  EXPECT_TRUE(cache.deferred_this_frame());
  EXPECT_EQ(cache.cold_this_frame(), 0);
  EXPECT_NE(cache.try_get("cached", sans()), nullptr) << "a cached run never waits";
  cache.new_frame(1000);
  EXPECT_FALSE(cache.deferred_this_frame());
  ASSERT_NE(cache.try_get("cold", sans()), nullptr);
  EXPECT_EQ(cache.cold_this_frame(), 1);
  EXPECT_GE(cache.cold_ms_this_frame(), 0.0);
  cache.new_frame(0);
  EXPECT_NE(cache.get("forced", sans()).texture, 0u) << "get() ignores the budget";
  EXPECT_EQ(cache.size(), 3u);
}

TEST(RunCache, EvictsLeastRecentlyUsedOverTheByteBudget) {
  RecordingTextures tex;
  RunCache cache(tex.ops());
  const unsigned alpha = cache.get("alpha", sans()).texture;
  const unsigned beta = cache.get("beta", sans()).texture;
  for (int i = 0; i < 3; ++i) cache.new_frame(2);
  EXPECT_EQ(cache.size(), 2u) << "under the default budget nothing is evicted";

  cache.byte_budget = 1;
  cache.get("alpha", sans());
  cache.get("beta", sans());  // beta is now the most recent
  cache.new_frame(2);
  EXPECT_EQ(cache.size(), 2u) << "runs drawn in the previous frame stay";
  cache.get("beta", sans());
  cache.new_frame(2);
  EXPECT_EQ(cache.size(), 1u);
  EXPECT_EQ(tex.destroyed(), std::vector<unsigned>{alpha});
  cache.new_frame(2);
  EXPECT_EQ(cache.size(), 0u);
  EXPECT_EQ(tex.destroyed(), (std::vector<unsigned>{alpha, beta}));
  EXPECT_EQ(cache.texture_bytes(), 0u);
}

TEST(RunCache, WrapsEllipsizesAndFits) {
  RecordingTextures tex;
  RunCache cache(tex.ops());
  const std::string text = "a long line of text that does not fit";
  RunStyle narrow = sans();
  narrow.max_width = 60;
  const TextRun& cut = cache.get(text, narrow);
  EXPECT_TRUE(cut.ellipsized);
  EXPECT_EQ(cut.lines, 1);
  EXPECT_LE(cut.size().x, 60.5f);
  narrow.wrap = true;
  const TextRun& wrapped = cache.get(text, narrow);
  EXPECT_FALSE(wrapped.ellipsized);
  EXPECT_GT(wrapped.lines, 2);
  EXPECT_GE(wrapped.size().x, 60.0f);

  RunStyle slot = sans();
  slot.fit_slot = 100;
  EXPECT_EQ(cache.get("Fit", slot).shrink, 1.0f);
  slot.fit_slot = 4;  // nothing fits: the smallest step, clipped by the caller
  EXPECT_EQ(cache.get("Fit", slot).shrink, 0.85f);
}

TEST(RunCache, ResolvesTheBaseDirection) {
  RecordingTextures tex;
  RunCache cache(tex.ops());
  EXPECT_TRUE(cache.get("\u05E9\u05DC\u05D5\u05DD", sans()).rtl);
  RunStyle ltr = sans();
  ltr.dir = Dir::kLtr;
  EXPECT_FALSE(cache.get("\u05E9\u05DC\u05D5\u05DD", ltr).rtl);
  RunStyle rtl = sans();
  rtl.dir = Dir::kRtl;
  EXPECT_TRUE(cache.get("abc", rtl).rtl);
}

TEST(RunCache, ColourGlyphsGetAnUntintedRgbaLayer) {
  RecordingTextures tex;
  RunCache cache(tex.ops());
  const TextRun& run = cache.get("ok \U0001F600", sans());
  ASSERT_NE(run.color_texture, 0u);
  const Texture* rgba = tex.find(run.color_texture);
  ASSERT_NE(rgba, nullptr);
  EXPECT_EQ(rgba->channels, 4);
  EXPECT_EQ(rgba->pixels.size(), static_cast<size_t>(run.px_w) * static_cast<size_t>(run.px_h) * 4);
  EXPECT_EQ(run.bytes(), static_cast<size_t>(run.px_w) * static_cast<size_t>(run.px_h) * 5);
  const Texture* mask = tex.find(run.texture);
  ASSERT_NE(mask, nullptr);
  EXPECT_TRUE(std::any_of(mask->pixels.begin(), mask->pixels.end(), [](unsigned char a) { return a != 0; }))
      << "the plain glyphs stay in the tinted mask";
  EXPECT_EQ(cache.get("ok", sans()).color_texture, 0u);
}

TEST(RunCache, IndexAtSnapsToGraphemeBoundaries) {
  RecordingTextures tex;
  RunCache cache(tex.ops());
  const RunStyle s = sans();
  const std::string text = "ab" "e\u0301" "c";  // the cluster e + U+0301 is bytes [2, 5)
  std::vector<TextBox> boxes;
  cache.range_boxes(text, s, 2, 5, false, 0, boxes);
  ASSERT_EQ(boxes.size(), 1u);
  const TextBox b = boxes[0];
  const float y = (b.y0 + b.y1) / 2;
  EXPECT_EQ(cache.index_at(text, s, {b.x0 + (b.x1 - b.x0) * 0.25f, y}), 2u);
  EXPECT_EQ(cache.index_at(text, s, {b.x0 + (b.x1 - b.x0) * 0.75f, y}), 5u);
  EXPECT_EQ(cache.index_at(text, s, {-50, y}), 0u);
  EXPECT_EQ(cache.index_at(text, s, {1000, y}), text.size());
  EXPECT_EQ(tex.uploads(), 0) << "hit-testing never rasterises";
}

TEST(RunCache, RangeBoxesPerLineWithSelectedLineBreaks) {
  RecordingTextures tex;
  RunCache cache(tex.ops());
  const RunStyle s = sans();
  const std::string text = "one\ntwo";
  std::vector<TextBox> boxes;
  cache.range_boxes(text, s, 0, 3, false, 5, boxes);
  ASSERT_EQ(boxes.size(), 1u) << "the break is not selected";
  EXPECT_NEAR(boxes[0].x0, 0.0f, 1.0f);
  const TextBox one = boxes[0];

  cache.range_boxes(text, s, 0, 7, false, 5, boxes);
  ASSERT_EQ(boxes.size(), 3u);  // "one", its line break, "two"
  EXPECT_FLOAT_EQ(boxes[0].x1, one.x1);
  EXPECT_NEAR(boxes[1].x0, one.x1, 0.5f);
  EXPECT_NEAR(boxes[1].x1 - boxes[1].x0, 5.0f, 0.5f);
  EXPECT_GE(boxes[2].y0, boxes[0].y1 - 0.5f);

  cache.range_boxes(text, s, 0, 7, true, 5, boxes);
  EXPECT_EQ(boxes.size(), 4u) << "through_end also marks the end of the last line";
  cache.range_boxes(text, s, 3, 3, false, 5, boxes);
  EXPECT_TRUE(boxes.empty());
  cache.range_boxes(text, s, 7, 7, true, 5, boxes);
  EXPECT_EQ(boxes.size(), 1u);
}

TEST(RunCache, RangeBoxesFollowBidiRuns) {
  RecordingTextures tex;
  RunCache cache(tex.ops());
  const RunStyle s = sans();
  const std::string text = "abc \u05D0\u05D1\u05D2";  // Hebrew alef bet gimel: bytes [4, 10)
  std::vector<TextBox> boxes;
  cache.range_boxes(text, s, 0, 3, false, 0, boxes);
  ASSERT_EQ(boxes.size(), 1u);
  const TextBox latin = boxes[0];
  cache.range_boxes(text, s, 4, 10, false, 0, boxes);
  ASSERT_EQ(boxes.size(), 1u);
  EXPECT_GT(boxes[0].x0, latin.x1);
  // "c", the space and alef: alef is drawn at the right end of the RTL run, so two boxes.
  cache.range_boxes(text, s, 2, 6, false, 0, boxes);
  EXPECT_EQ(boxes.size(), 2u);
}

TEST(RunCache, WordAtUsesUnicodeWordBoundaries) {
  RecordingTextures tex;
  RunCache cache(tex.ops());
  const RunStyle s = sans();
  const std::string text = "hello brave  world";
  EXPECT_EQ(cache.word_at(text, s, 7), (Range{6, 11}));
  EXPECT_EQ(cache.word_at(text, s, 0), (Range{0, 5}));
  EXPECT_EQ(cache.word_at(text, s, 5), (Range{0, 5})) << "the trailing half of a word's last character";
  EXPECT_EQ(cache.word_at(text, s, 12), (Range{12, 13})) << "between words: the grapheme";
  EXPECT_EQ(cache.word_at(text, s, 100), (Range{13, 18}));
  EXPECT_EQ(cache.word_at("", s, 0), (Range{0, 0}));
  EXPECT_EQ(cache.word_at("cafe\u0301 au lait", s, 1), (Range{0, 6}));
  EXPECT_EQ(cache.word_at("\u05E9\u05DC\u05D5\u05DD \u05E2\u05D5\u05DC\u05DD", s, 11), (Range{9, 17}));
}

}  // namespace
}  // namespace imgui_accesskit::text
