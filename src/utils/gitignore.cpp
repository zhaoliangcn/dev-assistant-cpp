#include "utils/gitignore.hpp"

#include <algorithm>

namespace da {

bool GitignoreMatcher::is_default_ignored(const std::string& path) {
  if (path.rfind(".git/", 0) == 0 || path == ".git") return true;
  const char* prefixes[] = {"target/", "build/", "node_modules/", ".cache/"};
  for (auto* p : prefixes) {
    size_t pos = path.find(p);
    if (pos == 0 || (pos != std::string::npos && path[pos - 1] == '/'))
      return true;
  }
  return false;
}

void GitignoreMatcher::add_rules(const std::string& content,
                                 const std::string& dir) {
  size_t i = 0;
  while (i <= content.size()) {
    size_t nl = content.find('\n', i);
    std::string line = content.substr(
        i, nl == std::string::npos ? std::string::npos : nl - i);
    if (nl == std::string::npos) i = content.size() + 1; else i = nl + 1;

    // 去掉行尾 \r 与首尾空白
    while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
      line.pop_back();
    size_t b = 0;
    while (b < line.size() && line[b] == ' ') b++;
    line = line.substr(b);
    if (line.empty() || line[0] == '#') continue;

    Rule r;
    r.dir = dir;
    if (line[0] == '!') {
      r.negated = true;
      line = line.substr(1);
    }
    if (!line.empty() && line.back() == '/') {
      r.dir_only = true;
      line.pop_back();
    }
    if (line.empty()) continue;
    // 领头斜杠：锚定到规则目录
    if (line[0] == '/') line = line.substr(1);
    // 领头 **/ 即任意层级，保留原样
    r.pattern = line;
    rules_.push_back(std::move(r));
  }
}

bool GitignoreMatcher::match_glob(const std::string& pat, size_t pi,
                                  const std::string& str, size_t si) {
  while (pi < pat.size()) {
    char pc = pat[pi];
    if (pc == '*') {
      bool globstar = (pi + 1 < pat.size() && pat[pi + 1] == '*');
      if (globstar) {
        pi += 2;
        if (pi < pat.size() && pat[pi] == '/') pi++;  // **/ 
        // ** 匹配零段或多段
        for (size_t k = si; k <= str.size(); k++) {
          if (match_glob(pat, pi, str, k)) return true;
          while (k < str.size() && str[k] != '/') k++;  // 跳到下一段
        }
        return pi >= pat.size() || match_glob(pat, pi, str, str.size());
      }
      // 单 * 不跨段
      size_t seg_end = str.find('/', si);
      if (seg_end == std::string::npos) seg_end = str.size();
      for (size_t k = si; k <= seg_end; k++)
        if (match_glob(pat, pi + 1, str, k)) return true;
      return false;
    }
    if (pc == '?') {
      if (si >= str.size() || str[si] == '/') return false;
      pi++; si++;
      continue;
    }
    if (pc == '[') {  // 简单字符类 [abc] / [a-z]
      size_t close = pat.find(']', pi + 1);
      if (close == std::string::npos) return false;
      bool hit = false, neg = false;
      size_t j = pi + 1;
      if (j < close && (pat[j] == '!' || pat[j] == '^')) { neg = true; j++; }
      for (char c = str[si < str.size() ? si : 0]; j < close && si < str.size();) {
        if (j + 2 < close && pat[j + 1] == '-') {
          if (pat[j] <= c && c <= pat[j + 2]) hit = true;
          j += 3;
        } else {
          if (pat[j] == c) hit = true;
          j++;
        }
      }
      if (si >= str.size() || hit == neg) return false;
      pi = close + 1; si++;
      continue;
    }
    if (si >= str.size() || str[si] != pc) return false;
    pi++; si++;
  }
  return si == str.size();
}

bool GitignoreMatcher::path_matches(const Rule& r, const std::string& path) {
  // 规则目录前缀
  std::string rel = path;
  if (!r.dir.empty()) {
    if (rel.rfind(r.dir + "/", 0) != 0) return false;
    rel = rel.substr(r.dir.size() + 1);
  }
  bool anchored = r.pattern.find('/') != std::string::npos;
  if (anchored) {
    // 全路径匹配
    if (match_glob(r.pattern, 0, rel, 0)) return true;
    // 目录规则：路径在其目录下
    if (r.dir_only && rel.rfind(r.pattern + "/", 0) == 0) return true;
    return false;
  }
  // 非锚定：任一路径段匹配
  size_t start = 0;
  while (start <= rel.size()) {
    size_t slash = rel.find('/', start);
    size_t seg_end = slash == std::string::npos ? rel.size() : slash;
    std::string seg = rel.substr(start, seg_end - start);
    if (match_glob(r.pattern, 0, seg, 0)) {
      if (!r.dir_only || seg_end < rel.size()) return true;
    }
    if (r.dir_only) {
      // 目录规则也匹配“位于该目录下”的路径
      std::string prefix = rel.substr(0, seg_end);
      if (match_glob(r.pattern, 0, prefix, 0) && seg_end < rel.size())
        return true;
    }
    if (slash == std::string::npos) break;
    start = slash + 1;
  }
  return false;
}

bool GitignoreMatcher::is_ignored(const std::string& path) const {
  if (is_default_ignored(path)) return true;
  bool ignored = false;
  for (const auto& r : rules_) {
    if (path_matches(r, path)) ignored = !r.negated;
  }
  return ignored;
}

}  // namespace da
