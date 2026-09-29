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
#include "utils/utf8.hpp"

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

// C8: shared large-file guard. Reads through an open fd and checks the size with
// fstat() on that same fd, so the file cannot be replaced or grown between the
// check and the read. The previous stat(path) + ifstream form was racy: it
// measured one file and then read a possibly different one.
static bool read_capped(const std::string& path, size_t cap, std::string& out,
                        std::string& err) {
  int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) { err = "无法读取文件: " + path; return false; }
  struct stat st;
  if (::fstat(fd, &st) != 0) {
    ::close(fd);
    err = "无法读取文件: " + path;
    return false;
  }
  // 只读普通文件：FIFO/设备/procfs 上 read() 可能永久阻塞，且 st_size 不可信
  // （validate_path 只拒 symlink，不拒 FIFO），直接拒绝。
  if (!S_ISREG(st.st_mode)) {
    ::close(fd);
    err = "仅支持普通文件: " + path;
    return false;
  }
  const std::string cap_msg =
      "文件过大（>" + std::to_string(cap) + "B），拒绝读取: " + path;
  if (st.st_size > static_cast<off_t>(cap)) {
    ::close(fd);
    err = cap_msg;
    return false;
  }
  out.clear();
  out.reserve(static_cast<size_t>(st.st_size));
  char buf[65536];
  ssize_t n = 0;
  while ((n = ::read(fd, buf, sizeof buf)) > 0) {
    out.append(buf, static_cast<size_t>(n));
    // 兜底：文件在 open 与 read 之间被截断放大（同 inode 写入）时，
    // fstat 测得的 st_size 可能已过期，按实际读到的字节数再卡一次。
    if (out.size() > cap) {
      ::close(fd);
      err = cap_msg;
      return false;
    }
  }
  const bool failed = (n < 0);
  ::close(fd);
  if (failed) { err = "无法读取文件: " + path; return false; }
  // Raw file bytes may not be valid UTF-8; dump() would throw on them.
  // Raw file bytes may not be valid UTF-8; dump() would throw on them.
  out = sanitize_utf8(out);
  return true;
}

static const size_t kMaxReadBytes = 10LL * 1024 * 1024;  // 10MB

ToolResult read_file_tool(const nlohmann::json& args, ToolContext& ctx) {
  std::string path = resolve_path(ctx, arg_str(args, "path"));
  std::string read_err;
  std::string reason;
  if (!ctx.security->validate_path(path, PathCheck::ReadOnly, reason))
    return {false, "路径被拒绝: " + reason};


  std::string content;
  if (!read_capped(path, kMaxReadBytes, content, read_err))
    return {false, read_err};

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

// C1：代码态花括号深度统计——跳过字符串/字符字面量与 // /* */ 注释，
// 防止字面量或注释中的 { } 干扰函数边界判定。
// 返回 depth 归零（闭合）时的行下标；未闭合则到文件尾。
size_t find_code_block_end(const std::vector<std::string>& lines,
                           size_t start) {
  int depth = 0;
  bool opened = false;
  enum State { Code, LineComment, BlockComment, Str, Char } st = Code;
  for (size_t j = start; j < lines.size(); j++) {
    const std::string& l = lines[j];
    for (size_t k = 0; k < l.size(); k++) {
      char c = l[k];
      char next = k + 1 < l.size() ? l[k + 1] : '\0';
      if (st == LineComment) break;  // 行注释：跳过该行剩余
      switch (st) {
        case Code:
          if (c == '/' && next == '/') st = LineComment;
          else if (c == '/' && next == '*') { st = BlockComment; k++; }
          else if (c == '"') st = Str;
          else if (c == '\'') st = Char;
          else if (c == '{') { depth++; opened = true; }
          else if (c == '}') depth--;
          break;
        case BlockComment:
          if (c == '*' && next == '/') { st = Code; k++; }
          break;
        case Str:
          if (c == '\\') k++;
          else if (c == '"') st = Code;
          break;
        case Char:
          if (c == '\\') k++;
          else if (c == '\'') st = Code;
          break;
        case LineComment:
          break;  // 不可达（上方已处理）
      }
      if (st == Code && opened && depth <= 0) return j;
    }
    if (st == LineComment) st = Code;  // 行注释在行尾结束
  }
  return lines.size() - 1;  // 未闭合：到文件尾
}

ToolResult read_symbol_tool(const nlohmann::json& args, ToolContext& ctx) {
  std::string path = resolve_path(ctx, arg_str(args, "path"));
  std::string read_err;
  std::string symbol = arg_str(args, "symbol");
  if (symbol.empty()) return {false, "symbol 不能为空"};
  std::string reason;
  if (!ctx.security->validate_path(path, PathCheck::ReadOnly, reason))
    return {false, "路径被拒绝: " + reason};

  std::string content;
  if (!read_capped(path, kMaxReadBytes, content, read_err))
    return {false, read_err};

  // 轻量符号定位（零依赖，对齐 Rust tree-sitter 版的语义：返回该符号定义起的
  // 完整片段）。启发式：定义行 = 含符号名且带声明关键字；结束 = 顶层缩进回归。
  std::vector<std::string> lines;
  size_t pos = 0;
  while (pos <= content.size()) {
    size_t nl = content.find('\n', pos);
    lines.push_back(content.substr(
        pos, nl == std::string::npos ? std::string::npos : nl - pos));
    if (nl == std::string::npos) break;
    pos = nl + 1;
  }

  auto is_def_line = [&](const std::string& l) {
    if (l.find(symbol) == std::string::npos) return false;
    // C/C++/Java/JS/Go 系：函数或类型声明；Python：def/class
    return l.find("def " + symbol) != std::string::npos ||
           l.find("class " + symbol) != std::string::npos ||
           l.find("struct " + symbol) != std::string::npos ||
           l.find("enum " + symbol) != std::string::npos ||
           l.find("interface " + symbol) != std::string::npos ||
           l.find("func " + symbol) != std::string::npos ||
           (l.find(symbol + "(") != std::string::npos &&
            l.find(';') == std::string::npos);
  };

  for (size_t i = 0; i < lines.size(); i++) {
    if (!is_def_line(lines[i])) continue;
    // 确定起始缩进；花括号语言用状态机找闭合（C1：跳过字符串/注释），
    // 缩进式语言（Python 等）以缩进回归定界
    size_t indent = lines[i].find_first_not_of(" \t");
    bool opened = lines[i].find('{') != std::string::npos;
    size_t end;
    if (opened) {
      end = find_code_block_end(lines, i);
    } else {
      end = i;
      for (size_t j = i + 1; j < lines.size(); j++) {
        const std::string& l = lines[j];
        if (l.find_first_not_of(" \t\r") == std::string::npos) { end = j; continue; }
        size_t ind = l.find_first_not_of(" \t");
        if (ind <= indent) { end = j - 1; break; }
        end = j;
      }
    }
    std::ostringstream out;
    out << (i + 1) << "\t" << lines[i] << "\n";
    for (size_t k = i + 1; k <= end && k < lines.size(); k++)
      out << (k + 1) << "\t" << lines[k] << "\n";
    return {true, out.str()};
  }
  return {false, "未找到符号: " + symbol};
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
  std::string read_err;
  std::string old_s = arg_str(args, "old_string");
  std::string new_s = arg_str(args, "new_string");
  std::string reason;
  if (!ctx.security->validate_path(path, PathCheck::Workspace, reason))
    return {false, "路径被拒绝: " + reason};
  if (ctx.security->is_sensitive_file(path))
    return {false, "拒绝编辑敏感文件: " + path};
  if (old_s.empty()) return {false, "old_string 不能为空"};

  std::string content;
  if (!read_capped(path, kMaxReadBytes, content, read_err))
    return {false, read_err};

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
  // 路径校验：目录前缀须在 workspace 内（防 ../../etc 越界）
  if (dir_prefix != "." && !dir_prefix.empty()) {
    std::string reason;
    std::string abs = dir_prefix[0] == '/' ? dir_prefix : ctx.workspace + "/" + dir_prefix;
    if (!ctx.security->validate_path(abs, PathCheck::ReadOnly, reason))
      return {false, "路径被拒绝: " + reason};
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
  return {true, arr.dump(-1, ' ', false)};
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
    // C8：跳过 >1MB 文件（对齐 Rust run_grep 的 max_file_size）
    struct stat fst;
    if (::stat(f.c_str(), &fst) == 0 && fst.st_size > 1024 * 1024) continue;
    std::ifstream in(f, std::ios::binary);
    if (!in) continue;
    std::string line;
    int lineno = 0;
    while (std::getline(in, line) && hits < max_results) {
      lineno++;
      if (line.find(needle) != std::string::npos) {
        results.push_back(
            {{"file", sanitize_utf8(f)}, {"line", lineno}, {"text", sanitize_utf8(line)}});
        hits++;
      }
    }
    if (hits >= max_results) break;
  }
  nlohmann::json out;
  out["matches"] = results;
  out["total"] = hits;
  return {true, out.dump(-1, ' ', false)};
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
    // read_symbol：符号级读取（对齐 Rust tree-sitter 版；C++ 零依赖轻量实现）
    ToolDefinition t;
    t.name = "read_symbol";
    t.description =
        "读取文件中某个符号（函数/类/结构体/def）的定义片段，带行号。"
        "参数: path（文件路径）, symbol（符号名）";
    t.parameters = {{"type", "object"},
                    {"properties",
                     {{"path", {{"type", "string"}}},
                      {"symbol", {{"type", "string"}}}}},
                    {"required", {"path", "symbol"}}};
    t.path_check = PathCheck::ReadOnly;
    t.handler = read_symbol_tool;
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
