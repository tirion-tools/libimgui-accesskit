// CEF accessibility mirror: validation limits and tree repair.
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "imgui_accesskit/cef_tree.h"

namespace {

using imgui_accesskit::CefKind;
using imgui_accesskit::CefTreeLimits;
using imgui_accesskit::CefTreeMirror;

const char* kDoc = R"J({"updates":[{"has_tree_data":true,"tree_data":{"focus_id":1},"root_id":1,"nodes":[
 {"id":1,"role":"rootWebArea","child_ids":[2,3],"location":{"x":0,"y":0,"width":800,"height":600},
  "attributes":{"name":"Doc","scrollY":100}},
 {"id":2,"role":"heading","child_ids":[4],"offset_container_id":1,"location":{"x":10,"y":120,"width":300,"height":30},
  "attributes":{"hierarchicalLevel":2,"name":"Title \u00e9"}},
 {"id":3,"role":"link","attributes":{"name":"click","url":"javascript:alert(1)"},"location":{"x":1,"y":2,"width":3,"height":4}},
 {"id":4,"role":"staticText","attributes":{"name":"Title"}}]}]})J";

TEST(CefTree, MirrorsAndExportsWellFormedTree) {
  CefTreeMirror m;
  m.reset(7);
  ASSERT_TRUE(m.apply(7, kDoc, false)) << m.last_reject();
  const auto out = m.export_tree();
  ASSERT_EQ(out.size(), 4u);
  EXPECT_EQ(out[0].kind, CefKind::kDocument);
  EXPECT_EQ(out[1].kind, CefKind::kHeading);
  EXPECT_EQ(out[1].level, 2);
  EXPECT_EQ(out[1].name, "Title \xC3\xA9");
  EXPECT_FLOAT_EQ(out[1].y, 20.0f) << "offset container + root scroll";
  EXPECT_EQ(out[3].kind, CefKind::kLink);
  EXPECT_TRUE(out[3].url.empty()) << "javascript: URL dropped";
  EXPECT_EQ(m.focus(), 1);
}

TEST(CefTree, IgnoresOtherGenerations) {
  CefTreeMirror m;
  m.reset(7);
  EXPECT_FALSE(m.apply(6, kDoc, false));
  EXPECT_EQ(m.node_count(), 0u);
  EXPECT_TRUE(m.last_reject().empty());
}

TEST(CefTree, RejectsMalformedAndOverLimit) {
  for (const char* bad : {"{", R"J({"updates":[{"nodes":[{"role":"x"}]}]})J", R"J({"updates":[{"nodes":[{"id":1e400}]}]})J",
                          R"J({"updates":"x"})J", "[1,]", R"J({"updates":[{"nodes":[{"id":1,"child_ids":[0]}]}]})J"}) {
    CefTreeMirror m;
    m.reset(1);
    EXPECT_FALSE(m.apply(1, bad, false)) << bad;
    EXPECT_FALSE(m.valid());
  }
  CefTreeLimits l;
  l.max_nodes = 2;
  CefTreeMirror m(l);
  m.reset(1);
  EXPECT_FALSE(m.apply(1, kDoc, false));
  EXPECT_EQ(m.last_reject(), "update has more nodes than max_nodes");
  std::string deep;
  for (int i = 0; i < 100; ++i) deep += "[";
  CefTreeMirror d;
  d.reset(1);
  EXPECT_FALSE(d.apply(1, deep, false));
}

TEST(CefTree, CyclesAndSecondParentsRepaired) {
  CefTreeMirror m;
  m.reset(1);
  ASSERT_TRUE(m.apply(1, R"J({"updates":[{"root_id":1,"nodes":[
    {"id":1,"role":"rootWebArea","child_ids":[2,3]},{"id":2,"role":"paragraph","child_ids":[1,3]},
    {"id":3,"role":"staticText","attributes":{"name":"x"}}]}]})J", false));
  const auto out = m.export_tree();
  ASSERT_EQ(out.size(), 3u);
  // Depth-first: 2 claims 3 first, so the root's claim on 3 and 2's claim on the root are dropped.
  EXPECT_EQ(out[0].children, std::vector<int32_t>{2});
  EXPECT_EQ(out[1].children, std::vector<int32_t>{3});
  EXPECT_TRUE(out[2].children.empty());
}

TEST(CefTree, DepthCutAndStringLimit) {
  CefTreeLimits l;
  l.max_depth = 3;
  l.max_string = 5;
  CefTreeMirror m(l);
  m.reset(1);
  ASSERT_TRUE(m.apply(1, R"J({"updates":[{"root_id":1,"nodes":[
    {"id":1,"role":"rootWebArea","child_ids":[2]},{"id":2,"child_ids":[3]},{"id":3,"child_ids":[4]},
    {"id":4,"role":"staticText","attributes":{"name":"\u00e9\u00e9\u00e9"}}]}]})J", false));
  const auto out = m.export_tree();
  EXPECT_EQ(out.size(), 3u);
  CefTreeMirror s(l);
  s.reset(1);
  ASSERT_TRUE(s.apply(1, R"J({"updates":[{"root_id":4,"nodes":[{"id":4,"role":"staticText","attributes":{"name":"\u00e9\u00e9\u00e9"}}]}]})J", false));
  EXPECT_EQ(s.export_tree()[0].name, "\xC3\xA9\xC3\xA9") << "cut at a UTF-8 boundary";
}

TEST(CefTree, LocationUpdate) {
  CefTreeMirror m;
  m.reset(2);
  ASSERT_TRUE(m.apply(2, kDoc, false));
  ASSERT_TRUE(m.apply(2, R"J([{"id":3,"new_location":{"bounds":{"x":50,"y":60,"width":5,"height":6}}}])J", true));
  EXPECT_FLOAT_EQ(m.export_tree()[3].x, 50.0f);
}

}  // namespace
