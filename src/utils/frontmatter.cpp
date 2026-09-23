#include "utils/frontmatter.hpp"

#include <cctype>

namespace da {

static std::string trim(const std::string& s) {
  size_t b = 0, e = s.size();
  while (b < e && std::isspace((unsigned char)s[b])) b++;
  while (e > b && std::isspace((unsigned char)s[e - 1])) e--;
  return s.substr(b, e - b);
}

// 去掉行内注释（仅处理 " #" 形式；引号内的 # 保留）
static std::string strip_comment(const std::string& s) {
  bool in_s = false, in_d = false;
  for (size_t i = 0; i < s.size(); i++) {
    char c = s[i];
    if (c == '\'' && !in_d) in_s = !in_s;
    else if (c == '"' && !in_s) in_d = !in_d;
    else if (c == '#' && !in_s && !in_d && i > 0 && s[i - 1] == ' ')
      return s.substr(0, i);
  }
  return s;
}

// 去除包裹引号
static std::string unquote(const std::string& s) {
  if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') ||
                        (s.front() == '\'' && s.back() == '\'')))
    return s.substr(1, s.size() - 2);
  return s;
}

bool parse_frontmatter(const std::string& text, Frontmatter& fm,
                       std::string& body) {
  fm.fields.clear();
  body.clear();

  // 定位头尾分隔线
  if (text.compare(0, 4, "---\n") != 0 && text.compare(0, 5, "---\r\n") != 0)
    return false;
  size_t pos = text.find('\n', 0) + 1;
  size_t end = std::string::npos;
  for (size_t i = pos; i + 3 < text.size() + 1; i++) {
    if (text.compare(i, 3, "---") == 0 &&
        (i + 3 == text.size() || text[i + 3] == '\n' || text[i + 3] == '\r')) {
      end = i;
      break;
    }
  }
  if (end == std::string::npos) return false;

  std::string fmtext = text.substr(pos, end - pos);
  body = text.substr(end + 3);
  if (!body.empty() && body[0] == '\r') body.erase(0, 1);
  if (!body.empty() && body[0] == '\n') body.erase(0, 1);

  // 逐行解析：一级 key 或 "- item"（归属上一个 key）
  std::string last_key;
  size_t i = 0;
  while (i < fmtext.size()) {
    size_t nl = fmtext.find('\n', i);
    std::string line =
        trim(strip_comment(fmtext.substr(i, nl == std::string::npos
                                                ? std::string::npos
                                                : nl - i)));
    i = (nl == std::string::npos) ? fmtext.size() : nl + 1;
    if (line.empty()) continue;

    if (line[0] == '-') {  // 数组项
      if (last_key.empty()) return false;
      std::string item = unquote(trim(line.substr(1)));
      auto& cur = fm.fields[last_key];
      if (std::holds_alternative<std::string>(cur)) {
        std::string prev = std::get<std::string>(cur);
        cur = prev.empty() ? std::vector<std::string>{item}
                           : std::vector<std::string>{prev, item};
      } else if (auto* a = std::get_if<std::vector<std::string>>(&cur)) {
        a->push_back(item);
      }
      continue;
    }

    size_t colon = line.find(':');
    if (colon == std::string::npos) return false;
    std::string key = trim(line.substr(0, colon));
    std::string val = trim(line.substr(colon + 1));
    if (key.empty()) return false;

    if (val.empty()) {  // 值在后续 "- item" 行里
      fm.fields[key] = std::string{};  // 占位；后续项会替换为数组
      last_key = key;
    } else if (val == "true") {
      fm.fields[key] = std::string{"true"};
      last_key = key;
    } else if (val == "false") {
      fm.fields[key] = std::string{"false"};
      last_key = key;
    } else {
      fm.fields[key] = unquote(val);
      last_key = key;
    }
  }
  return true;
}

}  // namespace da
