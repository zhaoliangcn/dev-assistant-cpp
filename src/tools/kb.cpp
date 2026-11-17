#include "tools/kb.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <climits>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "utils/frontmatter.hpp"
#include "utils/sha256.hpp"
#include "utils/utf8.hpp"

namespace da {

namespace {

constexpr size_t kMaxContentBytes = 2000;  // 查询结果单条正文上限

// ---------------------------------------------------------------------------
// 索引结构（.kb/index.json，与 Rust 版本字段兼容）
// ---------------------------------------------------------------------------
struct KbEntry {
  std::string path;    // 相对 .kb/ 的路径
  std::string type;    // decision / interface / summary / issue
  std::string title;
  std::vector<std::string> tags;
  std::string status;  // proposed / accepted / deprecated / draft ...
  bool archived = false;
  // 条目所基于的源文件（相对项目根路径 + 创建时 sha256），用于过期检测
  std::vector<std::pair<std::string, std::string>> source_refs;
};

struct KbIndex {
  int version = 1;
  std::string updated;
  std::map<std::string, KbEntry> entries;
};

std::string trim(const std::string& s) {
  size_t b = 0, e = s.size();
  while (b < e && std::isspace((unsigned char)s[b])) b++;
  while (e > b && std::isspace((unsigned char)s[e - 1])) e--;
  return s.substr(b, e - b);
}

std::string arg_str(const nlohmann::json& args, const char* key) {
  if (!args.is_object()) return {};
  auto it = args.find(key);
  if (it == args.end() || !it->is_string()) return {};
  return it->get<std::string>();
}

bool arg_bool(const nlohmann::json& args, const char* key, bool def) {
  if (!args.is_object()) return def;
  auto it = args.find(key);
  if (it == args.end() || !it->is_boolean()) return def;
  return it->get<bool>();
}

std::string now_iso() {
  std::time_t t = std::time(nullptr);
  std::tm tm{};
  gmtime_r(&t, &tm);
  char buf[32];
  std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
  return buf;
}

std::string join_path(const std::string& a, const std::string& b) {
  if (a.empty()) return b;
  if (a.back() == '/') return a + b;
  return a + "/" + b;
}

bool ends_with(const std::string& s, const std::string& suffix) {
  return s.size() >= suffix.size() &&
         s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// 递归创建目录（mkdir -p）
bool ensure_dirs(const std::string& path) {
  std::string cur;
  size_t i = 0;
  if (!path.empty() && path[0] == '/') {
    cur = "/";
    i = 1;
  }
  while (i <= path.size()) {
    size_t slash = path.find('/', i);
    std::string part = path.substr(
        i, slash == std::string::npos ? std::string::npos : slash - i);
    if (!part.empty()) {
      cur = cur.empty() ? part : (cur.back() == '/' ? cur + part : cur + "/" + part);
      if (::mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST) return false;
    }
    if (slash == std::string::npos) break;
    i = slash + 1;
  }
  return true;
}

bool read_file(const std::string& path, std::string& out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  std::ostringstream ss;
  ss << f.rdbuf();
  out = ss.str();
  return true;
}

bool write_file(const std::string& path, const std::string& content) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) return false;
  f << content;
  return static_cast<bool>(f);
}

std::string file_stem(const std::string& path) {
  size_t slash = path.find_last_of('/');
  std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
  size_t dot = name.find_last_of('.');
  return dot == std::string::npos ? name : name.substr(0, dot);
}

// 从 frontmatter 取标签：兼容 "- item" 数组与行内 "[a, b]" 两种写法
std::vector<std::string> fm_tags(const Frontmatter& fm) {
  std::vector<std::string> tags = fm.get_array("tags");
  if (tags.size() != 1) return tags;
  std::string s = trim(tags[0]);
  if (s.size() < 2 || s.front() != '[' || s.back() != ']') return tags;
  std::vector<std::string> out;
  std::string item;
  for (char c : s.substr(1, s.size() - 2)) {
    if (c == ',') {
      std::string t = trim(item);
      if (!t.empty()) out.push_back(t);
      item.clear();
    } else {
      item += c;
    }
  }
  std::string t = trim(item);
  if (!t.empty()) out.push_back(t);
  for (auto& t2 : out)
    if (t2.size() >= 2 && ((t2.front() == '"' && t2.back() == '"') ||
                           (t2.front() == '\'' && t2.back() == '\'')))
      t2 = t2.substr(1, t2.size() - 2);
  return out;
}

bool load_index(const std::string& kb_root, KbIndex& index) {
  std::string raw;
  if (!read_file(join_path(kb_root, "index.json"), raw)) return false;
  nlohmann::json j = nlohmann::json::parse(raw, nullptr, false);
  if (j.is_discarded() || !j.is_object()) return false;
  index.version = j.value("version", 1);
  index.updated = j.value("updated", "");
  if (!j.contains("entries") || !j["entries"].is_object()) return true;
  for (auto it = j["entries"].begin(); it != j["entries"].end(); ++it) {
    const auto& v = it.value();
    KbEntry e;
    e.path = v.value("path", "");
    e.type = v.value("type", "unknown");
    e.title = v.value("title", it.key());
    e.status = v.value("status", "draft");
    e.archived = v.value("archived", false);
    if (v.contains("tags") && v["tags"].is_array())
      for (const auto& t : v["tags"])
        if (t.is_string()) e.tags.push_back(t.get<std::string>());
    if (v.contains("source_refs") && v["source_refs"].is_array())
      for (const auto& r : v["source_refs"])
        if (r.is_object() && r.contains("path") && r["path"].is_string() &&
            r.contains("sha256") && r["sha256"].is_string())
          e.source_refs.emplace_back(r["path"].get<std::string>(),
                                     r["sha256"].get<std::string>());
    index.entries[it.key()] = e;
  }
  return true;
}

bool save_index(const std::string& kb_root, const KbIndex& index) {
  nlohmann::json entries = nlohmann::json::object();
  for (const auto& [id, e] : index.entries) {
    nlohmann::json refs = nlohmann::json::array();
    for (const auto& [p, sha] : e.source_refs)
      refs.push_back({{"path", p}, {"sha256", sha}});
    entries[id] = {{"path", e.path},
                   {"type", e.type},
                   {"title", e.title},
                   {"tags", e.tags},
                   {"status", e.status},
                   {"archived", e.archived},
                   {"source_refs", refs}};
  }
  nlohmann::json j = {{"version", index.version},
                      {"updated", index.updated},
                      {"entries", entries}};
  return write_file(join_path(kb_root, "index.json"), j.dump(2));
}

// ---------------------------------------------------------------------------
// 检索：中英文分词（ASCII 单词 + 驼峰拆分 + CJK 单字）
// ---------------------------------------------------------------------------
bool is_cjk(unsigned cp) {
  return (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0x3400 && cp <= 0x4DBF) ||
         (cp >= 0x3000 && cp <= 0x303F) || (cp >= 0x3040 && cp <= 0x309F) ||
         (cp >= 0x30A0 && cp <= 0x30FF) || (cp >= 0xAC00 && cp <= 0xD7AF);
}

std::vector<std::string> tokenize(const std::string& text) {
  std::vector<std::string> tokens;
  std::string word;
  auto flush = [&]() {
    if (!word.empty()) {
      tokens.push_back(word);
      word.clear();
    }
  };
  for (size_t i = 0; i < text.size();) {
    unsigned char c = static_cast<unsigned char>(text[i]);
    unsigned cp = 0;
    size_t len = 1;
    if (c < 0x80) {
      cp = c;
    } else if ((c >> 5) == 0x6) {
      cp = c & 0x1F;
      len = 2;
    } else if ((c >> 4) == 0xE) {
      cp = c & 0x0F;
      len = 3;
    } else if ((c >> 3) == 0x1E) {
      cp = c & 0x07;
      len = 4;
    } else {
      i++;
      continue;
    }
    for (size_t k = 1; k < len && i + k < text.size(); k++)
      cp = (cp << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3F);

    if (is_cjk(cp)) {
      flush();
      tokens.push_back(text.substr(i, len));
    } else if (cp < 128 && std::isalnum(cp)) {
      // 驼峰边界：SyncTool → sync + tool
      if (std::isupper(cp) && !word.empty() &&
          (std::islower((unsigned char)word.back()) ||
           std::isdigit((unsigned char)word.back())))
        flush();
      word += static_cast<char>(std::tolower(cp));
    } else {
      flush();
    }
    i += len;
  }
  flush();
  return tokens;
}

std::string canonical_path(const std::string& p) {
  char buf[PATH_MAX];
  if (!realpath(p.c_str(), buf)) return {};
  return std::string(buf);
}

// 解析 frontmatter 的 source_paths（逗号分隔的相对路径），
// 仅保留位于项目根内的文件，并记录创建时 sha256
std::vector<std::pair<std::string, std::string>> build_source_refs(
    const Frontmatter& fm, const std::string& workspace) {
  std::vector<std::pair<std::string, std::string>> refs;
  std::string raw = fm.get_string("source_paths");
  if (raw.empty()) return refs;
  std::string root = canonical_path(workspace);
  std::string item;
  std::istringstream ss(raw);
  while (std::getline(ss, item, ',')) {
    std::string rel = trim(item);
    if (rel.empty() || rel[0] == '/') continue;
    if (rel.find("..") != std::string::npos) continue;
    std::string full = canonical_path(join_path(workspace, rel));
    if (full.empty()) continue;
    if (!root.empty() && full != root &&
        full.compare(0, root.size() + 1, root + "/") != 0)
      continue;  // 越出项目根，拒绝
    std::string content;
    if (!read_file(full, content)) continue;
    refs.emplace_back(rel, sha256_hex(content));
  }
  return refs;
}

// 统计过期引用：文件缺失或内容哈希变化
size_t stale_ref_count(const KbEntry& e, const std::string& workspace) {
  size_t n = 0;
  for (const auto& [rel, sha] : e.source_refs) {
    std::string content;
    if (!read_file(join_path(workspace, rel), content) ||
        sha256_hex(content) != sha)
      n++;
  }
  return n;
}

// 查询统计（.kb/query-stats.json）：为后续 dream 归档健康度提供依据
struct QueryStat {
  long count = 0;
  std::string last_query_at;
};

using QueryStats = std::map<std::string, QueryStat>;

QueryStats load_query_stats(const std::string& kb_root) {
  QueryStats stats;
  std::string raw;
  if (!read_file(join_path(kb_root, "query-stats.json"), raw)) return stats;
  nlohmann::json j = nlohmann::json::parse(raw, nullptr, false);
  if (j.is_discarded() || !j.is_object()) return stats;
  for (auto it = j.begin(); it != j.end(); ++it)
    if (it->is_object())
      stats[it.key()] = {
          it->value("query_count", 0L),
          it->value("last_query_at", std::string())};
  return stats;
}

bool save_query_stats(const std::string& kb_root, const QueryStats& stats) {
  nlohmann::json j = nlohmann::json::object();
  for (const auto& [id, s] : stats)
    j[id] = {{"query_count", s.count}, {"last_query_at", s.last_query_at}};
  return write_file(join_path(kb_root, "query-stats.json"), j.dump(2));
}

int field_score(const std::vector<std::string>& query_tokens,
                const std::vector<std::string>& field_tokens, int exact,
                int prefix) {
  int score = 0;
  for (const auto& qt : query_tokens)
    for (const auto& et : field_tokens) {
      if (et == qt)
        score += exact;
      else if (et.size() >= qt.size() && et.compare(0, qt.size(), qt) == 0)
        score += prefix;
    }
  return score;
}

struct QueryHit {
  std::string id;
  const KbEntry* entry = nullptr;
  int score = 0;
};

std::vector<QueryHit> search_entries(const KbIndex& index,
                                     const std::string& query,
                                     const std::string& type_filter,
                                     const std::vector<std::string>& tag_filter,
                                     size_t max_results, bool include_archived) {
  std::vector<std::string> query_tokens = tokenize(query);
  std::vector<QueryHit> scored;
  for (const auto& [id, entry] : index.entries) {
    if (entry.archived && !include_archived) continue;
    if (!type_filter.empty() && type_filter != "any" &&
        entry.type != type_filter)
      continue;
    int score = 0;
    if (!tag_filter.empty()) {
      bool hit = false;
      for (const auto& t : tag_filter)
        for (const auto& et : entry.tags)
          if (t == et) hit = true;
      if (!hit) continue;
      score += 10;
    }
    if (!query_tokens.empty()) {
      std::vector<std::string> tag_tokens;
      for (const auto& t : entry.tags)
        for (auto& tk : tokenize(t)) tag_tokens.push_back(tk);
      score += field_score(query_tokens, tokenize(entry.title), 5, 3);
      score += field_score(query_tokens, tokenize(id), 3, 2);
      score += field_score(query_tokens, tokenize(entry.path), 2, 1);
      score += field_score(query_tokens, tag_tokens, 2, 1);
    }
    if (entry.status == "accepted" || entry.status == "completed") score += 2;
    if (query.empty() && tag_filter.empty()) score = 1;
    scored.push_back({id, &entry, score});
  }
  std::stable_sort(scored.begin(), scored.end(),
                   [](const QueryHit& a, const QueryHit& b) {
                     return a.score > b.score;
                   });
  if (scored.size() > max_results) scored.resize(max_results);
  return scored;
}

std::string format_results(const std::vector<QueryHit>& hits,
                           const std::string& kb_root,
                           const std::string& workspace, bool include_content) {
  if (hits.empty()) return "未找到匹配的 KnowledgeBase 条目。";
  std::ostringstream out;
  out << "找到 " << hits.size() << " 个匹配条目：\n\n";
  int i = 0;
  for (const auto& h : hits) {
    const KbEntry& e = *h.entry;
    out << ++i << ". **" << h.id << "** — " << e.title << "\n"
        << "   - 类型: " << e.type << " | 状态: " << e.status
        << " | 分数: " << h.score << "\n"
        << "   - 路径: " << e.path << "\n";
    size_t stale = stale_ref_count(e, workspace);
    if (stale > 0)
      out << "   - 过期引用: " << stale << "（源文件已变更或缺失）\n";
    if (!e.tags.empty()) {
      out << "   - 标签: ";
      for (size_t k = 0; k < e.tags.size(); k++)
        out << (k ? ", " : "") << e.tags[k];
      out << "\n";
    }
    if (include_content) {
      std::string raw;
      if (read_file(join_path(kb_root, e.path), raw)) {
        Frontmatter fm;
        std::string body;
        if (!parse_frontmatter(raw, fm, body) || body.empty()) body = raw;
        body = trim(body);
        bool truncated = body.size() > kMaxContentBytes;
        out << "   - 内容: " << truncate_utf8(body, kMaxContentBytes);
        if (truncated) out << "...\n   [内容已截断]";
        out << "\n";
      } else {
        out << "   - 内容: (文件不存在或无法读取)\n";
      }
    }
  }
  return out.str();
}

// ---------------------------------------------------------------------------
// 工具处理函数
// ---------------------------------------------------------------------------
ToolResult kb_store_handler(const nlohmann::json& args, ToolContext& ctx) {
  std::string path = trim(arg_str(args, "path"));
  if (path.empty()) return {false, "kb_store: 'path' 必填"};
  if (path.find("..") != std::string::npos)
    return {false, "kb_store: 路径包含 '..'，拒绝写入"};
  if (!ends_with(path, ".md"))
    return {false, "kb_store: path 必须以 .md 结尾"};
  // 兼容 LLM 传入的完整路径 .kb/decisions/foo.md
  while (path.compare(0, 4, ".kb/") == 0) path.erase(0, 4);
  while (path.compare(0, 4, ".kb\\") == 0) path.erase(0, 4);
  if (path.empty() || path[0] == '/')
    return {false, "kb_store: path 必须是相对 .kb/ 的路径"};

  std::string content = arg_str(args, "content");
  if (content.empty()) return {false, "kb_store: 'content' 必填"};
  bool update_index = arg_bool(args, "update_index", true);

  std::string kb_root = join_path(ctx.workspace, ".kb");
  std::string file_path = join_path(kb_root, path);
  size_t slash = file_path.find_last_of('/');
  if (slash != std::string::npos &&
      !ensure_dirs(file_path.substr(0, slash)))
    return {false, "kb_store: 创建目录失败"};
  if (!write_file(file_path, content))
    return {false, "kb_store: 写入条目失败: " + file_path};

  if (update_index) {
    Frontmatter fm;
    std::string body;
    if (!parse_frontmatter(content, fm, body)) fm.fields.clear();
    std::string id = fm.get_string("id");
    if (id.empty()) id = file_stem(path);
    KbEntry entry;
    entry.path = path;
    entry.type = fm.get_string("type").empty() ? "unknown" : fm.get_string("type");
    entry.title = fm.get_string("title").empty() ? id : fm.get_string("title");
    entry.tags = fm_tags(fm);
    entry.status =
        fm.get_string("status").empty() ? "draft" : fm.get_string("status");
    entry.archived = fm.get_string("archived") == "true";
    entry.source_refs = build_source_refs(fm, ctx.workspace);

    KbIndex index;
    load_index(kb_root, index);
    index.entries[id] = entry;
    index.updated = now_iso();
    if (!save_index(kb_root, index))
      return {false, "kb_store: 更新索引失败"};
  }
  return {true, "[kb_store] 条目已保存: " + path + "\n路径: " + file_path};
}

ToolResult kb_query_handler(const nlohmann::json& args, ToolContext& ctx) {
  std::string query = arg_str(args, "query");
  std::string type_filter = arg_str(args, "type");
  std::vector<std::string> tag_filter;
  if (args.is_object() && args.contains("tags") && args["tags"].is_array())
    for (const auto& t : args["tags"])
      if (t.is_string()) tag_filter.push_back(t.get<std::string>());
  long max_results = 5;
  if (args.is_object() && args.contains("max_results") &&
      args["max_results"].is_number_integer())
    max_results = args["max_results"].get<long>();
  if (max_results <= 0) max_results = 5;
  bool include_content = arg_bool(args, "include_content", false);
  bool include_archived = arg_bool(args, "include_archived", false);

  std::string kb_root = join_path(ctx.workspace, ".kb");
  KbIndex index;
  if (!load_index(kb_root, index))
    return {true,
            "KnowledgeBase 为空（.kb/index.json 不存在）。"
            "请先使用 kb_store 创建条目。"};

  auto hits = search_entries(index, query, type_filter, tag_filter,
                             static_cast<size_t>(max_results), include_archived);
  // 记录查询命中（供 dream 健康度/归档策略使用）
  if (!query.empty() && !hits.empty()) {
    QueryStats stats = load_query_stats(kb_root);
    std::string now = now_iso();
    for (const auto& h : hits) {
      stats[h.id].count++;
      stats[h.id].last_query_at = now;
    }
    save_query_stats(kb_root, stats);
  }
  return {true, format_results(hits, kb_root, ctx.workspace, include_content)};
}

}  // namespace

void register_kb_tools(ToolRegistry& reg) {
  {
    ToolDefinition t;
    t.name = "kb_store";
    t.description =
        "创建或更新项目知识库条目（架构决策/接口定义/问题记录）。"
        "content 必须是 Markdown，建议包含 YAML frontmatter"
        "（id/type/title/tags/status）。写入 .kb/ 并更新 index.json。";
    t.parameters = {
        {"type", "object"},
        {"properties",
         {{"path",
           {{"type", "string"},
            {"description",
             "相对 .kb/ 的路径，如 decisions/ADR-001-use-ecs.md"}}},
          {"content",
           {{"type", "string"},
            {"description", "完整 Markdown 内容（含 YAML frontmatter）"}}},
          {"update_index",
           {{"type", "boolean"},
            {"description", "是否自动更新 index.json"},
            {"default", true}}}}},
        {"required", {"path", "content"}}};
    t.path_check = PathCheck::None;  // 路径在 handler 内按 .kb/ 根校验
    t.needs_approval = true;
    t.handler = kb_store_handler;
    reg.register_tool(std::move(t));
  }
  {
    ToolDefinition t;
    t.name = "kb_query";
    t.description =
        "检索项目知识库条目，支持关键词、类型与标签过滤；"
        "include_content=true 时返回正文（单条截断 2000 字节）。";
    t.parameters = {
        {"type", "object"},
        {"properties",
         {{"query", {{"type", "string"}, {"description", "搜索关键词"}}},
          {"type",
           {{"type", "string"},
            {"enum", {"decision", "interface", "summary", "issue", "any"}},
            {"description", "条目类型过滤"},
            {"default", "any"}}},
          {"tags",
           {{"type", "array"},
            {"items", {{"type", "string"}}},
            {"description", "标签过滤（任一命中）"}}},
          {"max_results",
           {{"type", "integer"}, {"description", "最大结果数"}, {"default", 5}}},
          {"include_content",
           {{"type", "boolean"},
            {"description", "是否返回正文"},
            {"default", false}}},
          {"include_archived",
           {{"type", "boolean"},
            {"description", "是否包含已归档条目"},
            {"default", false}}}}},
        {"required", nlohmann::json::array()}};
    t.path_check = PathCheck::None;
    t.handler = kb_query_handler;
    reg.register_tool(std::move(t));
  }
}

}  // namespace da
