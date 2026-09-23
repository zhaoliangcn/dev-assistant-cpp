#pragma once
// .gitignore 子集匹配（对应 Rust ignore crate 的常用场景）
// 支持：`*` / `**` / `?` / `!` 取反 / 目录规则（尾斜杠）/ 注释
#include <string>
#include <vector>

namespace da {

class GitignoreMatcher {
public:
  // 逐行加载 .gitignore 内容；dir 为文件所在目录（相对于仓库根，"" 表示根）
  void add_rules(const std::string& content, const std::string& dir = "");

  // 返回 true 表示路径被忽略；path 为相对仓库根的 POSIX 风格路径
  bool is_ignored(const std::string& path) const;

  // 内置默认忽略：.git/、构建目录
  static bool is_default_ignored(const std::string& path);

private:
  struct Rule {
    std::string pattern;  // 归一化后的模式
    std::string dir;      // 规则来源目录
    bool negated = false;
    bool dir_only = false;
  };

  static bool match_glob(const std::string& pat, size_t pi,
                         const std::string& str, size_t si);
  static bool path_matches(const Rule& r, const std::string& path);

  std::vector<Rule> rules_;
};

}  // namespace da
