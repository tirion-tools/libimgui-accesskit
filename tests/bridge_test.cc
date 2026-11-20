// Bridge frame protocol without an AccessKit adapter (no D-Bus): rects, ids, relation filtering and
// the AccessKit tree built from a published frame.
#include "imgui_accesskit/bridge.h"

#include <cstdlib>
#include <set>
#include <string>

#include <accesskit.h>
#include <gtest/gtest.h>

namespace imgui_accesskit {
namespace {

const Node* find(const Bridge& b, const std::string& key) {
  for (const Node& n : b.last_frame())
    if (n.key == key) return &n;
  return nullptr;
}

std::string debug_string(const accesskit_tree_update* update) {
  char* s = accesskit_tree_update_debug(update);
  std::string out = s != nullptr ? s : "";
  accesskit_string_free(s);
  return out;
}

TEST(A11yBridge, RootCoversWindowInPhysicalPixels) {
  Bridge b;
  b.begin_frame(2, 800, 600);
  b.end_frame(false);
  ASSERT_EQ(b.last_frame().size(), 1u);
  const Node& root = b.last_frame()[0];
  EXPECT_EQ(root.id, 1u);
  EXPECT_EQ(root.role, Role::kWindow);
  EXPECT_FLOAT_EQ(root.x1, 1600);
  EXPECT_FLOAT_EQ(root.y1, 1200);
}

TEST(A11yBridge, ContainerRectCoversChildren) {
  Bridge b;
  b.begin_frame(2, 800, 600);
  b.push(Bridge::id_of("outer"), Role::kGroup, "outer", "Outer");
  b.push(Bridge::id_of("inner"), Role::kToolbar, "inner", "Inner");
  b.set_rect(b.add(Bridge::id_of("a"), Role::kButton, "a", "A", Bridge::kClick), 10, 10, 20, 20);
  b.set_rect(b.add(Bridge::id_of("b"), Role::kButton, "b", "B", Bridge::kClick), 30, 5, 40, 15);
  b.add(Bridge::id_of("empty"), Role::kLabel, "empty", "No rect");  // ignored by the union
  b.pop();
  b.set_rect(b.add(Bridge::id_of("c"), Role::kLabel, "c", "C"), 0, 50, 5, 60);
  b.pop();
  Node& fixed = b.push(Bridge::id_of("fixed"), Role::kGroup, "fixed", "Fixed");
  b.set_rect(fixed, 100, 100, 200, 200);  // an explicit rect wins over the children's union
  b.set_rect(b.add(Bridge::id_of("d"), Role::kLabel, "d", "D"), 0, 0, 1, 1);
  b.pop();
  b.end_frame(false);

  const Node* inner = find(b, "inner");
  ASSERT_NE(inner, nullptr);
  EXPECT_FLOAT_EQ(inner->x0, 20);
  EXPECT_FLOAT_EQ(inner->y0, 10);
  EXPECT_FLOAT_EQ(inner->x1, 80);
  EXPECT_FLOAT_EQ(inner->y1, 40);
  const Node* outer = find(b, "outer");
  ASSERT_NE(outer, nullptr);
  EXPECT_FLOAT_EQ(outer->x0, 0);
  EXPECT_FLOAT_EQ(outer->y0, 10);
  EXPECT_FLOAT_EQ(outer->x1, 80);
  EXPECT_FLOAT_EQ(outer->y1, 120);
  ASSERT_EQ(outer->children.size(), 2u);
  const Node* fx = find(b, "fixed");
  ASSERT_NE(fx, nullptr);
  EXPECT_FLOAT_EQ(fx->x0, 200);
  EXPECT_FLOAT_EQ(fx->y1, 400);
  EXPECT_EQ(b.last_frame()[0].children.size(), 2u);
  EXPECT_EQ(b.duplicate_ids(), 0);
}

TEST(A11yBridge, DuplicateKeyGetsDerivedUniqueId) {
  Bridge b;
  b.begin_frame(1, 100, 100);
  const uint64_t id = Bridge::id_of("row");
  const uint64_t first = b.add(id, Role::kRow, "row", "Row 1").id;
  const uint64_t second = b.add(id, Role::kRow, "row", "Row 2").id;
  b.end_frame(false);
  EXPECT_EQ(first, id);
  EXPECT_NE(second, id);
  EXPECT_NE(second, 0u);
  EXPECT_NE(second, 1u);
  EXPECT_EQ(b.duplicate_ids(), 1);
  const std::vector<uint64_t>& kids = b.last_frame()[0].children;
  EXPECT_EQ(std::set<uint64_t>(kids.begin(), kids.end()).size(), kids.size());
}

TEST(A11yBridge, IdOfIsStableAndNeverZeroOrRoot) {
  EXPECT_EQ(Bridge::id_of("message_list"), Bridge::id_of("message_list"));
  EXPECT_NE(Bridge::id_of("message_list"), Bridge::id_of("folder_tree"));
  std::set<uint64_t> seen;
  for (int i = 0; i < 10000; ++i) {
    const uint64_t id = Bridge::id_of("row/" + std::to_string(i));
    EXPECT_GT(id, 1u);
    seen.insert(id);
  }
  EXPECT_EQ(seen.size(), 10000u);
  EXPECT_GT(Bridge::id_of(""), 1u);
}

TEST(A11yBridge, DanglingRelationsAreDropped) {
  Bridge b;
  b.begin_frame(1, 100, 100);
  const uint64_t popup = Bridge::id_of("popup");
  const uint64_t missing = Bridge::id_of("closed_popup");
  Node& search = b.add(Bridge::id_of("search"), Role::kSearchInput, "search", "Search");
  search.controls = {popup, missing};
  search.described_by = {missing};
  search.labelled_by = {1};
  search.active_descendant = missing;
  b.add(popup, Role::kList, "popup", "Suggestions");
  b.end_frame(false);
  const Node* s = find(b, "search");
  ASSERT_NE(s, nullptr);
  EXPECT_EQ(s->controls, std::vector<uint64_t>{popup});
  EXPECT_TRUE(s->described_by.empty());
  EXPECT_EQ(s->labelled_by, std::vector<uint64_t>{1});
  EXPECT_EQ(s->active_descendant, 0u);
}

TEST(A11yBridge, BuildsAccessKitTreeForNestedFrame) {
  Bridge b;
  // Before the first frame: a root-only tree.
  accesskit_tree_update* empty = b.build_full_tree();
  ASSERT_NE(empty, nullptr);
  accesskit_tree_update_free(empty);

  b.begin_frame(1, 1280, 800);
  b.push(Bridge::id_of("tree"), Role::kTree, "tree", "Folders");
  Node& inbox = b.add(Bridge::id_of("inbox"), Role::kTreeItem, "inbox", "Inbox", Bridge::kClick | Bridge::kFocus);
  inbox.level = 1;
  inbox.expanded = 1;
  inbox.selected = 1;
  inbox.description = "12 unread";
  b.pop();
  b.push(Bridge::id_of("list"), Role::kTable, "list", "Messages").row_count = 2;
  Node& row = b.push(Bridge::id_of("row0"), Role::kRow, "row0", "Unread, Ann, Hello");
  row.row_index = 0;
  row.controls = {Bridge::id_of("reader"), Bridge::id_of("gone")};
  b.add(Bridge::id_of("cell0"), Role::kCell, "cell0", "Ann");
  b.pop();
  b.pop();
  Node& split = b.add(Bridge::id_of("split"), Role::kSplitter, "split", "Folder pane width");
  split.numeric = 240;
  split.numeric_min = 120;
  split.numeric_max = 480;
  split.orientation = 2;
  Node& more = b.add(Bridge::id_of("more"), Role::kButton, "more", "More options", Bridge::kClick);
  more.has_popup = 1;
  more.toggled = 0;
  b.add(Bridge::id_of("reader"), Role::kDocument, "reader", "Message content (from the sender)");
  Node& alert = b.add(Bridge::id_of("alert"), Role::kAlert, "alert", "Warning");
  alert.live = 2;
  alert.labelled_by = {Bridge::id_of("alert")};
  Node& h = b.add(Bridge::id_of("subject"), Role::kHeading, "subject", "Hello");
  h.level = 1;
  b.focus(Bridge::id_of("not_in_frame"));  // falls back to the root
  b.end_frame(true);
  EXPECT_EQ(b.duplicate_ids(), 0);

  accesskit_tree_update* update = b.build_full_tree();
  ASSERT_NE(update, nullptr);
  const std::string dump = debug_string(update);
  accesskit_tree_update_free(update);
  // Levels reach AccessKit zero-based.
  EXPECT_NE(dump.find("role: Heading, label: \"Hello\", level: 0"), std::string::npos) << dump;
  EXPECT_NE(dump.find("actions: [Click, Focus], label: \"Inbox\", description: \"12 unread\", level: 0, is_expanded: true"), std::string::npos) << dump;
  EXPECT_NE(dump.find("row_index: 0"), std::string::npos) << dump;
  EXPECT_NE(dump.find("row_count: 2"), std::string::npos) << dump;
  // Only the relation whose target is in the frame survives.
  EXPECT_NE(dump.find("controls: [#" + std::to_string(Bridge::id_of("reader")) + "]"), std::string::npos) << dump;
  EXPECT_EQ(dump.find(std::to_string(Bridge::id_of("gone"))), std::string::npos) << dump;
  EXPECT_NE(dump.find("focus: #1 }"), std::string::npos) << dump;
}

TEST(A11yBridge, TextSelectionNamesTextRunsBelowItsNode) {
  Bridge b;
  b.begin_frame(1, 400, 300);
  const uint64_t line0 = Bridge::id_of("doc.p0.l0");
  const uint64_t line1 = Bridge::id_of("doc.p0.l1");
  b.push(Bridge::id_of("doc"), Role::kDocument, "doc", "Message content (from the sender)");
  b.push(Bridge::id_of("doc.p0"), Role::kParagraph, "doc.p0", "Hi\nthere");
  Node& r0 = b.add(line0, Role::kTextRun, "doc.p0.l0", "");
  r0.value = "Hi\n";
  r0.character_lengths = {1, 1, 1};
  Node& r1 = b.add(line1, Role::kTextRun, "doc.p0.l1", "");
  r1.value = "there";
  r1.character_lengths = {1, 1, 1, 1, 1};
  b.pop();
  Node& doc = b.top();
  doc.selection_anchor = {line0, 1};
  doc.selection_focus = {line1, 3};
  b.pop();
  // Invalid: a run outside the node, a non-TextRun target, a run missing from the frame.
  Node& other = b.add(Bridge::id_of("other"), Role::kDocument, "other", "Other");
  other.selection_anchor = {line0, 0};
  other.selection_focus = {line0, 2};
  Node& label = b.push(Bridge::id_of("label"), Role::kLabel, "label", "Label");
  label.selection_anchor = {Bridge::id_of("label"), 0};
  label.selection_focus = {Bridge::id_of("gone"), 0};
  b.pop();
  b.end_frame(false);
  const Node* d = find(b, "doc");
  ASSERT_NE(d, nullptr);
  EXPECT_EQ(d->selection_anchor, (TextPosition{line0, 1}));
  EXPECT_EQ(d->selection_focus, (TextPosition{line1, 3}));
  EXPECT_EQ(find(b, "other")->selection_anchor.node, 0u);
  EXPECT_EQ(find(b, "label")->selection_focus.node, 0u);

  accesskit_tree_update* update = b.build_full_tree();
  ASSERT_NE(update, nullptr);
  const std::string dump = debug_string(update);
  accesskit_tree_update_free(update);
  EXPECT_NE(dump.find("TextRun"), std::string::npos) << dump;
  EXPECT_NE(dump.find("character_lengths: [1, 1, 1, 1, 1]"), std::string::npos) << dump;
  EXPECT_NE(dump.find("character_index: 3"), std::string::npos) << dump;
}

TEST(A11yBridge, EmptyTextRunKeepsAnEmptyValue) {
  Bridge b;
  b.begin_frame(1, 100, 100);
  b.push(Bridge::id_of("doc"), Role::kDocument, "doc", "Doc");
  b.add(Bridge::id_of("doc.empty"), Role::kTextRun, "doc.empty", "");
  b.pop();
  b.end_frame(false);
  accesskit_tree_update* update = b.build_full_tree();
  ASSERT_NE(update, nullptr);
  const std::string dump = debug_string(update);
  accesskit_tree_update_free(update);
  EXPECT_NE(dump.find("role: TextRun, value: \"\""), std::string::npos) << dump;
}

TEST(A11yBridge, IgnoresForeignEvents) {
  Bridge b;
  SDL_Event e;
  SDL_zero(e);
  e.type = SDL_EVENT_WINDOW_FOCUS_GAINED;
  EXPECT_FALSE(b.handle_event(e));
  EXPECT_FALSE(b.take_click(1));
}

TEST(A11yBridge, FormRolesInvalidStateAndLanguage) {
  Bridge b;
  b.begin_frame(1, 800, 600);
  b.add(Bridge::id_of("cb"), Role::kCheckBox, "cb", "Remember me").checked = 1;
  b.add(Bridge::id_of("rb"), Role::kRadioButton, "rb", "Plain text").checked = 0;
  b.add(Bridge::id_of("combo"), Role::kComboBox, "combo", "From").has_popup = 2;
  b.add(Bridge::id_of("side"), Role::kComplementary, "side", "Details");
  b.add(Bridge::id_of("note"), Role::kNote, "note", "Replies go to bob");
  Node& to = b.add(Bridge::id_of("to"), Role::kTextInput, "to", "To");
  to.invalid = true;
  to.description = "Not an email address";
  b.add(Bridge::id_of("p"), Role::kParagraph, "p", "Bonjour").language = "fr";
  b.end_frame(true);
  accesskit_tree_update* update = b.build_full_tree();
  ASSERT_NE(update, nullptr);
  const std::string dump = debug_string(update);
  accesskit_tree_update_free(update);
  for (const char* want : {"role: CheckBox", "role: RadioButton", "role: ComboBox", "role: Complementary", "role: Note",
                           "invalid: True", "language: \"fr\""})
    EXPECT_NE(dump.find(want), std::string::npos) << want << "\n" << dump;
}

}  // namespace
}  // namespace imgui_accesskit
