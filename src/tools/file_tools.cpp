#include "tools/file_tools.hpp"

#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <fstream>
#include <sstream>

#include "utils/atomic_write.hpp"
#include "utils/gitignore.hpp"

namespace da {

namespace {

// 参数宽容解析（对应 Rust tools/common.rs）
std::string arg_str(const nlohmann::json& args, const char* key,
                    const std::string& def = "") {
  if (!args.contains(key)) return def;
  const auto& v = args[key];
  if (v.is_string()) return v.get<std::string>();
  if (v.is_number_integer()) return std::to_string(v.get<long long>());
  if (v.is_number_float()) return std::to_string(v.get<double>());
  return def;
}

long arg_long(const nlohmann::json& args, const char* key, long def) {
  if (!args.contains(key)) return def;
  const auto& v = args[key];
  if (v.is_number_integer()) return v.get<long>();
  if (v.is_string()) try { return std::stol(v.get<std::string>()); } catch (...) {}
  return def;
}

std::string resolve_path(ToolContext& ctx, const std::string& p) {
  if (!p.empty() && p[0] == '/') return p;
  return ctx.workspace + "/" + p;
}

ToolResult read_file_tool(const nlohmann::json& args, ToolContext& ctx) {
  std::string path = resolve_path(ctx, arg_str(args, "path"));
  std::string reason;
  if (!ctx.security->validate_path(path, PathCheck::ReadOnly, reason))
    return {false, "路径被拒绝: " + reason};

  std::ifstream f(path, std::ios::binary);
  if (!f) return {false, "无法读取文件: " + path};
  std::ostringstream ss;
  ss << f.rdbuf();
  std::string content = ss.str();

  long offset = arg_long(args, "offset", 0);
  long limit = arg_long(args, "limit", 2000);
  if (offset > 0 || limit > 0) {
    // 按行切片
    std::vector<std::string> lines;
    size_t pos = 0;
    while (pos <= content.size()) {
      size_t nl = content.find('\n', pos);
      lines.push_back(content.substr(
          pos, nl == std::string::npos ? std::string::npos : nl - pos));
      if (nl == std::string::npos) break;
      pos = nl + 1;
    }
    std::ostringstream out;
    long end = limit > 0 ? std::min(offset + limit, (long)lines.size())
                         : (long)lines.size();
    for (long i = offset; i < end; i++)
      out << (i + 1) << "\t" << lines[i] << "\n";
    return {true, out.str()};
  }
  return {true, content};
}

ToolResult write_file_tool(const nlohmann::json& args, ToolContext& ctx) {
  std::string path = resolve_path(ctx, arg_str(args, "path"));
  std::string content = arg_str(args, "content");
  std::string reason;
  if (!ctx.security->validate_path(path, PathCheck::Workspace, reason))
    return {false, "路径被拒绝: " + reason};
  if (ctx.security->is_sensitive_file(path))
    return {false, "拒绝写入敏感文件: " + path};
  auto st = atomic_write(path, content);
  if (!st.ok()) return {false, st.message()};
  return {true, "已写入 " + path + "（" + std::to_string(content.size()) + " 字节）"};
}

ToolResult edit_file_tool(const nlohmann::json& args, ToolContext& ctx) {
  std::string path = resolve_path(ctx, arg_str(args, "path"));
  std::string old_s = arg_str(args, "old_string");
  std::string new_s = arg_str(args, "new_string");
  std::string reason;
  if (!ctx.security->validate_path(path, PathCheck::Workspace, reason))
    return {false, "路径被拒绝: " + reason};
  if (ctx.security->is_sensitive_file(path))
    return {false, "拒绝编辑敏感文件: " + path};
  if (old_s.empty()) return {false, "old_string 不能为空"};

  std::ifstream f(path, std::ios::binary);
  if (!f) return {false, "无法打开文件: " + path};
  std::ostringstream ss;
  ss << f.rdbuf();
  std::string content = ss.str();

  bool replace_all = args.value("replace_all", false);
  size_t count = 0;
  size_t pos = 0;
  while ((pos = content.find(old_s, pos)) != std::string::npos) {
    content.replace(pos, old_s.size(), new_s);
    pos += new_s.size();
    count++;
    if (!replace_all) break;
  }
  if (count == 0) return {false, "old_string 未找到"};
  auto st = atomic_write(path, content);
  if (!st.ok()) return {false, st.message()};
  return {true, "已替换 " + std::to_string(count) + " 处"};
}

// 递归收集文件（带 gitignore 过滤）
static void walk(const std::string& dir, const std::string& rel,
                 const GitignoreMatcher& gi, std::vector<std::string>& out,
                 int depth) {
  if (depth > 16) return;
  DIR* d = ::opendir(dir.c_str());
  if (!d) return;
  struct dirent* e;
  while ((e = ::readdir(d)) != nullptr) {
    std::string name = e->d_name;
    if (name == "." || name == "..") continue;
    std::string rpath = rel.empty() ? name : rel + "/" + name;
    std::string fpath = dir + "/" + name;
    struct stat st;
    if (::lstat(fpath.c_str(), &st) != 0) continue;
    if (S_ISDIR(st.st_mode)) {
      if (!gi.is_ignored(rpath + "/")) walk(fpath, rpath, gi, out, depth + 1);
    } else if (S_ISREG(st.st_mode)) {
      if (!gi.is_ignored(rpath)) out.push_back(rpath);
    }
  }
  ::closedir(d);
}

// 简单 glob：仅支持 prefix* / *.suffix / 精确名 三种形态 + ** 递归
static bool name_matches(const std::string& pattern,
                         const std::string& filename) {
  if (pattern == filename) return true;
  // * 通配（不跨段）
  size_t star = pattern.find('*');
  if (star == std::string::npos) return false;
  std::string pre = pattern.substr(0, star);
  std::string suf = pattern.substr(star + 1);
  if (filename.size() < pre.size() + suf.size()) return false;
  if (filename.compare(0, pre.size(), pre) != 0) return false;
  if (filename.compare(filename.size() - suf.size(), suf.size(), suf) != 0)
    return false;
  return true;
}

ToolResult glob_tool(const nlohmann::json& args, ToolContext& ctx) {
  std::string pattern = arg_str(args, "pattern");
  if (pattern.empty()) return {false, "pattern 不能为空"};
  std::string dir_prefix = ".";
  // pattern 带目录前缀时分离
  size_t slash = pattern.rfind('/');
  std::string fname = pattern;
  if (slash != std::string::npos) {
    dir_prefix = pattern.substr(0, slash);
    fname = pattern.substr(slash + 1);
    if (fname == "**") { fname = "*"; dir_prefix = pattern; }
  }

  GitignoreMatcher gi;
  std::vector<std::string> files;
  walk(ctx.workspace, "", gi, files, 0);

  nlohmann::json arr = nlohmann::json::array();
  int limit = 200;
  for (const auto& f : files) {
    // 匹配目录部分
    bool dir_ok = (dir_prefix == ".");
    if (!dir_ok) {
      if (dir_prefix.find("**") != std::string::npos) {
        std::string pre = dir_prefix.substr(0, dir_prefix.find("**"));
        dir_ok = f.rfind(pre, 0) == 0;
      } else {
        dir_ok = f.rfind(dir_prefix + "/", 0) == 0;
      }
    }
    if (!dir_ok) continue;
    std::string base = f.substr(f.find_last_of('/') + 1);
    if (name_matches(fname, base)) {
      arr.push_back(f);
      if (--limit <= 0) break;
    }
  }
  return {true, arr.dump()};
}

ToolResult grep_tool(const nlohmann::json& args, ToolContext& ctx) {
  std::string needle = arg_str(args, "pattern");
  if (needle.empty()) return {false, "pattern 不能为空"};
  std::string path = resolve_path(ctx, arg_str(args, "path", "."));

  std::vector<std::string> files;
  struct stat st;
  if (::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
    files.push_back(path);
  } else {
    GitignoreMatcher gi;
    walk(ctx.workspace, "", gi, files, 0);
  }

  nlohmann::json results = nlohmann::json::array();
  int max_results = (int)arg_long(args, "max_results", 50);
  int hits = 0;
  for (const auto& f : files) {
    std::ifstream in(f, std::ios::binary);
    if (!in) continue;
    std::string line;
    int lineno = 0;
    while (std::getline(in, line) && hits < max_results) {
      lineno++;
      if (line.find(needle) != std::string::npos) {
        results.push_back({{"file", f}, {"line", lineno}, {"text", line}});
        hits++;
      }
    }
    if (hits >= max_results) break;
  }
  nlohmann::json out;
  out["matches"] = results;
  out["total"] = hits;
  return {true, out.dump()};
}

}  // namespace

void register_file_tools(ToolRegistry& reg) {
  {
    ToolDefinition t;
    t.name = "read_file";
    t.description = "读取文件内容，可指定 offset/limit 按行切片";
    t.parameters = {{"type", "object"},
                    {"properties",
                     {{"path", {{"type", "string"}, {"description", "文件路径"}}},
                      {"offset", {{"type", "integer"}, {"description", "起始行(0基)"}}},
                      {"limit", {{"type", "integer"}, {"description", "行数上限"}}}}},
                    {"required", {"path"}}};
    t.path_check = PathCheck::ReadOnly;
    t.handler = read_file_tool;
    reg.register_tool(std::move(t));
  }
  {
    ToolDefinition t;
    t.name = "write_file";
    t.description = "写入文件（原子写：临时文件+rename）";
    t.parameters = {{"type", "object"},
                    {"properties",
                     {{"path", {{"type", "string"}}},
                      {"content", {{"type", "string"}}}}},
                    {"required", {"path", "content"}}};
    t.path_check = PathCheck::Workspace;
    t.needs_approval = true;
    t.handler = write_file_tool;
    reg.register_tool(std::move(t));
  }
  {
    ToolDefinition t;
    t.name = "edit_file";
    t.description = "精确文本替换（old_string→new_string）";
    t.parameters = {{"type", "object"},
                    {"properties",
                     {{"path", {{"type", "string"}}},
                      {"old_string", {{"type", "string"}}},
                      {"new_string", {{"type", "string"}}},
                      {"replace_all", {{"type", "boolean"}}}}},
                    {"required", {"path", "old_string", "new_string"}}};
    t.path_check = PathCheck::Workspace;
    t.needs_approval = true;
    t.handler = edit_file_tool;
    reg.register_tool(std::move(t));
  }
  {
    ToolDefinition t;
    t.name = "glob";
    t.description = "按模式查找文件（支持 * 与 ** 目录递归）";
    t.parameters = {{"type", "object"},
                    {"properties", {{"pattern", {{"type", "string"}}}}},
                    {"required", {"pattern"}}};
    t.path_check = PathCheck::ReadOnly;
    t.handler = glob_tool;
    reg.register_tool(std::move(t));
  }
  {
    ToolDefinition t;
    t.name = "grep";
    t.description = "在文件/目录中按字面量搜索文本";
    t.parameters = {{"type", "object"},
                    {"properties",
                     {{"pattern", {{"type", "string"}}},
                      {"path", {{"type", "string"}}},
                      {"max_results", {{"type", "integer"}}}}},
                    {"required", {"pattern"}}};
    t.path_check = PathCheck::ReadOnly;
    t.handler = grep_tool;
    reg.register_tool(std::move(t));
  }
}

}  // namespace da
