#include "imgui_accesskit/cef_tree.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <unordered_set>
#include <utility>

namespace imgui_accesskit::cef {
namespace {

// ---- bounded JSON (RFC 8259 subset CefWriteJSON emits; strict, no extensions)
struct Json {
  enum class T : uint8_t { kNull, kBool, kNum, kStr, kArr, kObj };
  T t = T::kNull;
  bool b = false;
  double n = 0;
  std::string s;
  std::vector<Json> a;               // array items, or object values (with keys)
  std::vector<std::string> keys;     // object keys, parallel to a
  const Json* get(std::string_view k) const {
    if (t != T::kObj) return nullptr;
    for (size_t i = 0; i < keys.size(); ++i)
      if (keys[i] == k) return &a[i];
    return nullptr;
  }
};

class Parser {
 public:
  Parser(std::string_view in, size_t max_depth) : p_(in.data()), end_(in.data() + in.size()), max_depth_(max_depth) {}
  bool parse(Json* out) {
    if (!value(out, 0)) return false;
    ws();
    return p_ == end_;
  }

 private:
  void ws() {
    while (p_ < end_ && (*p_ == ' ' || *p_ == '\n' || *p_ == '\r' || *p_ == '\t')) ++p_;
  }
  bool lit(const char* s) {
    for (; *s; ++s, ++p_)
      if (p_ >= end_ || *p_ != *s) return false;
    return true;
  }
  static void put_utf8(std::string* s, uint32_t cp) {
    if (cp < 0x80) {
      s->push_back(char(cp));
    } else if (cp < 0x800) {
      s->push_back(char(0xC0 | (cp >> 6)));
      s->push_back(char(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      s->push_back(char(0xE0 | (cp >> 12)));
      s->push_back(char(0x80 | ((cp >> 6) & 0x3F)));
      s->push_back(char(0x80 | (cp & 0x3F)));
    } else {
      s->push_back(char(0xF0 | (cp >> 18)));
      s->push_back(char(0x80 | ((cp >> 12) & 0x3F)));
      s->push_back(char(0x80 | ((cp >> 6) & 0x3F)));
      s->push_back(char(0x80 | (cp & 0x3F)));
    }
  }
  bool hex4(uint32_t* v) {
    *v = 0;
    for (int i = 0; i < 4; ++i, ++p_) {
      if (p_ >= end_) return false;
      const char c = *p_;
      const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
      if (d < 0) return false;
      *v = (*v << 4) | uint32_t(d);
    }
    return true;
  }
  bool str(std::string* s) {
    if (p_ >= end_ || *p_ != '"') return false;
    ++p_;
    while (p_ < end_) {
      const auto c = static_cast<unsigned char>(*p_);
      if (c == '"') {
        ++p_;
        return true;
      }
      if (c < 0x20) return false;
      if (c != '\\') {
        s->push_back(char(c));
        ++p_;
        continue;
      }
      if (++p_ >= end_) return false;
      const char e = *p_++;
      switch (e) {
        case '"': s->push_back('"'); break;
        case '\\': s->push_back('\\'); break;
        case '/': s->push_back('/'); break;
        case 'b': s->push_back('\b'); break;
        case 'f': s->push_back('\f'); break;
        case 'n': s->push_back('\n'); break;
        case 'r': s->push_back('\r'); break;
        case 't': s->push_back('\t'); break;
        case 'u': {
          uint32_t cp = 0;
          if (!hex4(&cp)) return false;
          if (cp >= 0xD800 && cp <= 0xDBFF) {
            uint32_t lo = 0;
            if (!lit("\\u") || !hex4(&lo) || lo < 0xDC00 || lo > 0xDFFF) return false;
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            return false;
          }
          if (cp == 0) return false;
          put_utf8(s, cp);
          break;
        }
        default: return false;
      }
    }
    return false;
  }
  bool num(double* v) {
    const char* start = p_;
    if (p_ < end_ && *p_ == '-') ++p_;
    bool digits = false;
    while (p_ < end_ && ((*p_ >= '0' && *p_ <= '9') || *p_ == '.' || *p_ == 'e' || *p_ == 'E' || *p_ == '+' || *p_ == '-')) {
      digits |= *p_ >= '0' && *p_ <= '9';
      ++p_;
    }
    if (!digits || p_ - start > 40) return false;
    const std::string tmp(start, p_);
    char* e = nullptr;
    *v = std::strtod(tmp.c_str(), &e);
    return e == tmp.c_str() + tmp.size() && std::isfinite(*v);
  }
  bool value(Json* v, size_t depth) {
    if (depth > max_depth_) return false;
    ws();
    if (p_ >= end_) return false;
    switch (*p_) {
      case 'n': v->t = Json::T::kNull; return lit("null");
      case 't': v->t = Json::T::kBool; v->b = true; return lit("true");
      case 'f': v->t = Json::T::kBool; v->b = false; return lit("false");
      case '"': v->t = Json::T::kStr; return str(&v->s);
      case '[': {
        v->t = Json::T::kArr;
        ++p_;
        ws();
        if (p_ < end_ && *p_ == ']') {
          ++p_;
          return true;
        }
        for (;;) {
          v->a.emplace_back();
          if (!value(&v->a.back(), depth + 1)) return false;
          ws();
          if (p_ >= end_) return false;
          if (*p_ == ']') {
            ++p_;
            return true;
          }
          if (*p_++ != ',') return false;
        }
      }
      case '{': {
        v->t = Json::T::kObj;
        ++p_;
        ws();
        if (p_ < end_ && *p_ == '}') {
          ++p_;
          return true;
        }
        for (;;) {
          ws();
          std::string k;
          if (!str(&k)) return false;
          ws();
          if (p_ >= end_ || *p_ != ':') return false;
          ++p_;
          v->keys.push_back(std::move(k));
          v->a.emplace_back();
          if (!value(&v->a.back(), depth + 1)) return false;
          ws();
          if (p_ >= end_) return false;
          if (*p_ == '}') {
            ++p_;
            return true;
          }
          if (*p_++ != ',') return false;
        }
      }
      default: v->t = Json::T::kNum; return num(&v->n);
    }
  }
  const char* p_;
  const char* end_;
  size_t max_depth_;
};

bool get_i32(const Json* j, int32_t* out) {
  if (!j || j->t != Json::T::kNum || j->n != std::floor(j->n) || j->n < -2147483648.0 || j->n > 2147483647.0) return false;
  *out = int32_t(j->n);
  return true;
}

bool get_f(const Json* j, float* out) {
  if (!j || j->t != Json::T::kNum || std::fabs(j->n) > 1e7) return false;
  *out = float(j->n);
  return true;
}

// Cut at a UTF-8 boundary; the parser produced valid UTF-8 from escapes but raw bytes are unchecked:
// invalid sequences are replaced so AccessKit only gets valid UTF-8.
std::string clean(const std::string& s, size_t max) {
  std::string out;
  out.reserve(std::min(s.size(), max));
  for (size_t i = 0; i < s.size();) {
    const auto c = static_cast<unsigned char>(s[i]);
    size_t n = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
    bool ok = n != 0 && i + n <= s.size() && (n != 1 || c >= 0x20 || c == '\n' || c == '\t');
    for (size_t k = 1; ok && k < n; ++k) ok = (static_cast<unsigned char>(s[i + k]) & 0xC0) == 0x80;
    if (ok && n == 2 && c < 0xC2) ok = false;
    if (!ok) n = 1;
    if (out.size() + (ok ? n : 3) > max) break;
    if (ok) {
      out.append(s, i, n);
    } else {
      out.append("\xEF\xBF\xBD");
    }
    i += n;
  }
  return out;
}

CefKind kind_of(const std::string& role) {
  static const std::pair<const char*, CefKind> kMap[] = {
      {"rootWebArea", CefKind::kDocument},   {"paragraph", CefKind::kParagraph},   {"heading", CefKind::kHeading},
      {"link", CefKind::kLink},              {"list", CefKind::kList},             {"listItem", CefKind::kListItem},
      {"descriptionList", CefKind::kList},   {"term", CefKind::kListItem},         {"definition", CefKind::kListItem},
      {"table", CefKind::kTable},            {"row", CefKind::kRow},               {"cell", CefKind::kCell},
      {"gridCell", CefKind::kCell},          {"columnHeader", CefKind::kColumnHeader}, {"rowHeader", CefKind::kRowHeader},
      {"caption", CefKind::kCaption},        {"staticText", CefKind::kStaticText}, {"inlineTextBox", CefKind::kInlineTextBox},
      {"image", CefKind::kImage},            {"listMarker", CefKind::kIgnored},
  };
  for (const auto& [n, k] : kMap)
    if (role == n) return k;
  return CefKind::kGeneric;
}

bool url_ok(const std::string& u) {
  return u.rfind("http://", 0) == 0 || u.rfind("https://", 0) == 0 || u.rfind("mailto:", 0) == 0;
}

// A BCP 47-shaped language tag (RFC 5646 syntax, not the registry): an alphabetic primary subtag of 2-8 letters, then
// alphanumeric subtags of 1-8 joined by '-'; at most 35 characters (RFC 5646 §4.4.1).
bool language_ok(const std::string& tag) {
  if (tag.empty() || tag.size() > 35) return false;
  size_t start = 0;
  for (size_t i = 0; i <= tag.size(); ++i) {
    if (i < tag.size() && tag[i] != '-') {
      const char c = tag[i];
      const bool alpha = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
      if (!alpha && (start == 0 || c < '0' || c > '9')) return false;
      continue;
    }
    const size_t len = i - start;
    if (len < (start == 0 ? 2u : 1u) || len > 8) return false;
    start = i + 1;
  }
  return true;
}

}  // namespace

void CefTreeMirror::reset(uint64_t generation) {
  generation_ = generation;
  valid_ = true;
  root_ = focus_ = 0;
  nodes_.clear();
  last_reject_.clear();
}

bool CefTreeMirror::reject(const std::string& why) {
  valid_ = false;
  last_reject_ = why;
  nodes_.clear();
  return false;
}

bool CefTreeMirror::apply(uint64_t generation, std::string_view json, bool location_only) {
  if (generation != generation_ || generation == 0) return false;  // another document: ignored
  if (!valid_) return false;
  if (json.size() > limits_.max_json_bytes) return reject("update too large");
  Json j;
  if (!Parser(json, limits_.max_json_depth).parse(&j)) return reject("malformed JSON");
  if (location_only) {
    const Json* list = j.get("changes");
    if (j.t == Json::T::kArr) list = &j;
    if (!list || list->t != Json::T::kArr) return true;
    for (const Json& c : list->a) {
      int32_t id = 0;
      if (!get_i32(c.get("id"), &id)) continue;
      auto it = nodes_.find(id);
      if (it == nodes_.end()) continue;
      const Json* nl = c.get("new_location");
      if (!nl) continue;
      const Json* b = nl->get("bounds") ? nl->get("bounds") : nl;
      float x = 0;
      float y = 0;
      float w = 0;
      float h = 0;
      if (get_f(b->get("x"), &x) && get_f(b->get("y"), &y) && get_f(b->get("width"), &w) && get_f(b->get("height"), &h) &&
          w >= 0 && h >= 0) {
        it->second.x = x;
        it->second.y = y;
        it->second.w = w;
        it->second.h = h;
      }
      int32_t oc = 0;
      if (get_i32(nl->get("offset_container_id"), &oc)) it->second.offset_container = oc;
    }
    return true;
  }
  const Json* ups = j.get("updates");
  if (!ups || ups->t != Json::T::kArr) return reject("no updates");
  if (ups->a.size() > limits_.max_updates) return reject("too many updates");
  for (const Json& u : ups->a) {
    int32_t v = 0;
    if (get_i32(u.get("node_id_to_clear"), &v) && v != 0) {
      // Drop the subtree's nodes below v (kept itself; re-sent nodes replace it).
      std::vector<int32_t> stack;
      if (auto it = nodes_.find(v); it != nodes_.end()) stack = it->second.children;
      std::unordered_set<int32_t> seen;
      while (!stack.empty()) {
        const int32_t id = stack.back();
        stack.pop_back();
        if (!seen.insert(id).second) continue;
        auto it = nodes_.find(id);
        if (it == nodes_.end()) continue;
        stack.insert(stack.end(), it->second.children.begin(), it->second.children.end());
        nodes_.erase(it);
      }
    }
    if (get_i32(u.get("root_id"), &v) && v != 0) root_ = v;
    if (const Json* td = u.get("tree_data")) get_i32(td->get("focus_id"), &focus_);
    const Json* ns = u.get("nodes");
    if (!ns) continue;
    if (ns->t != Json::T::kArr) return reject("nodes not an array");
    for (const Json& nj : ns->a) {
      CefNode n;
      if (nj.t != Json::T::kObj || !get_i32(nj.get("id"), &n.id) || n.id == 0) return reject("node without id");
      const Json* role = nj.get("role");
      n.kind = role && role->t == Json::T::kStr ? kind_of(role->s) : CefKind::kGeneric;
      if (const Json* ch = nj.get("child_ids")) {
        if (ch->t != Json::T::kArr || ch->a.size() > limits_.max_children) return reject("child_ids");
        for (const Json& c : ch->a) {
          int32_t cid = 0;
          if (!get_i32(&c, &cid) || cid == 0) return reject("child id");
          n.children.push_back(cid);
        }
      }
      get_i32(nj.get("offset_container_id"), &n.offset_container);
      if (const Json* loc = nj.get("location")) {
        float x = 0;
        float y = 0;
        float w = 0;
        float h = 0;
        if (get_f(loc->get("x"), &x) && get_f(loc->get("y"), &y) && get_f(loc->get("width"), &w) &&
            get_f(loc->get("height"), &h) && w >= 0 && h >= 0) {
          n.x = x;
          n.y = y;
          n.w = w;
          n.h = h;
        }
      }
      if (const Json* st = nj.get("state"); st && st->t == Json::T::kArr) {
        for (const Json& s : st->a) {
          if (s.t != Json::T::kStr) continue;
          if (s.s == "focusable") n.focusable = true;
          if (s.s == "invisible" || s.s == "ignored") n.invisible = true;
        }
      }
      if (const Json* at = nj.get("attributes"); at && at->t == Json::T::kObj) {
        if (const Json* s = at->get("name"); s && s->t == Json::T::kStr) n.name = clean(s->s, limits_.max_string);
        if (const Json* s = at->get("value"); s && s->t == Json::T::kStr) n.value = clean(s->s, limits_.max_string);
        if (const Json* s = at->get("url"); s && s->t == Json::T::kStr && s->s.size() <= limits_.max_url && url_ok(s->s))
          n.url = clean(s->s, limits_.max_url);
        if (const Json* s = at->get("language"); s && s->t == Json::T::kStr && language_ok(s->s)) n.language = s->s;
        int32_t level = 0;
        if (get_i32(at->get("hierarchicalLevel"), &level)) n.level = std::max(0, std::min(level, 9));
        get_f(at->get("scrollX"), &n.scroll_x);
        get_f(at->get("scrollY"), &n.scroll_y);
      }
      const bool is_new = nodes_.count(n.id) == 0;
      if (is_new && nodes_.size() >= limits_.max_nodes) return reject("update has more nodes than max_nodes");
      nodes_[n.id] = std::move(n);
    }
  }
  // Reachability: drop nodes the root no longer reaches (bounds memory across updates).
  std::unordered_set<int32_t> reach;
  std::vector<int32_t> stack;
  if (nodes_.count(root_) != 0) stack.push_back(root_);
  while (!stack.empty()) {
    const int32_t id = stack.back();
    stack.pop_back();
    if (!reach.insert(id).second) continue;
    auto it = nodes_.find(id);
    if (it == nodes_.end()) continue;
    for (int32_t c : it->second.children) stack.push_back(c);
  }
  for (auto it = nodes_.begin(); it != nodes_.end();) it = reach.count(it->first) ? std::next(it) : nodes_.erase(it);
  return true;
}

std::vector<CefExport> CefTreeMirror::export_tree() const {
  std::vector<CefExport> out;
  if (!valid()) return out;
  std::unordered_set<int32_t> placed;
  size_t text = 0;
  const CefNode& root = nodes_.at(root_);
  // Absolute position: offsets accumulate along the offset-container chain (bounded by depth).
  auto absolute = [&](const CefNode& n, float* x, float* y) {
    *x = n.x;
    *y = n.y;
    int32_t oc = n.offset_container;
    for (size_t hops = 0; oc != 0 && hops < limits_.max_depth; ++hops) {
      auto it = nodes_.find(oc);
      if (it == nodes_.end()) break;
      *x += it->second.x;
      *y += it->second.y;
      oc = it->second.offset_container;
    }
    *x -= root.scroll_x;
    *y -= root.scroll_y;
  };
  struct Item {
    int32_t id;
    int depth;
    size_t parent;  // index in out, SIZE_MAX for the root
  };
  std::vector<Item> stack{{root_, 0, SIZE_MAX}};
  while (!stack.empty()) {
    const Item it = stack.back();
    stack.pop_back();
    if (size_t(it.depth) >= limits_.max_depth || !placed.insert(it.id).second) continue;  // depth cut, second parent
    auto f = nodes_.find(it.id);
    if (f == nodes_.end()) continue;
    const CefNode& n = f->second;
    CefExport e;
    e.id = n.id;
    e.depth = it.depth;
    e.kind = n.kind;
    e.name = n.kind == CefKind::kStaticText || n.kind == CefKind::kInlineTextBox ? n.name : n.name.empty() ? n.value : n.name;
    if (text + e.name.size() > limits_.max_total_text) e.name.clear();
    text += e.name.size();
    e.url = n.url;
    e.language = n.language;
    e.level = n.level;
    absolute(n, &e.x, &e.y);
    e.w = n.w;
    e.h = n.h;
    const size_t index = out.size();
    if (it.parent != SIZE_MAX) out[it.parent].children.push_back(n.id);
    out.push_back(std::move(e));
    for (auto c = n.children.rbegin(); c != n.children.rend(); ++c) stack.push_back({*c, it.depth + 1, index});  // NOLINT(modernize-loop-convert): reverse order
  }
  // Children pushed for placed-elsewhere / missing ids are pruned to exported ones.
  std::unordered_set<int32_t> exported;
  for (const CefExport& e : out) exported.insert(e.id);
  for (CefExport& e : out) {
    std::vector<int32_t> keep;
    std::unordered_set<int32_t> seen;
    for (int32_t c : e.children)
      if (exported.count(c) && seen.insert(c).second) keep.push_back(c);
    e.children = std::move(keep);
  }
  return out;
}

}  // namespace imgui_accesskit::cef
