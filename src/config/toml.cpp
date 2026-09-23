#include "config/toml.hpp"

#include <cctype>

namespace da {

static std::string trim(const std::string& s) {
  size_t b = 0, e = s.size();
  while (b < e && std::isspace((unsigned char)s[b])) b++;
  while (e > b && std::isspace((unsigned char)s[e - 1])) e--;
  return s.substr(b, e - b);
}

static std::string unquote(const std::string& s) {
  if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
    std::string out;
    for (size_t i = 1; i + 1 < s.size(); i++) {
      if (s[i] == '\\' && i + 2 < s.size()) {
        char c = s[++i];
        switch (c) {
          case 'n': out += '\n'; break;
          case 't': out += '\t'; break;
          case 'r': out += '\r'; break;
          case '"': out += '"'; break;
          case '\\': out += '\\'; break;
          default: out += c; break;
        }
      } else {
        out += s[i];
      }
    }
    return out;
  }
  return s;
}

// 去掉 " #" 与 "  #" 形式的行尾注释（引号外）
static std::string strip_comment(const std::string& s) {
  bool in_q = false;
  for (size_t i = 0; i < s.size(); i++) {
    char c = s[i];
    if (c == '"') in_q = !in_q;
    else if (c == '#' && !in_q) return s.substr(0, i);
  }
  return s;
}

static TomlValue parse_scalar(const std::string& raw) {
  TomlValue v;
  if (raw == "true") { v.type = TomlValue::Type::Bool; v.b = true; return v; }
  if (raw == "false") { v.type = TomlValue::Type::Bool; v.b = false; return v; }
  if (!raw.empty() && raw.front() == '"') { v.type = TomlValue::Type::String; v.s = unquote(raw); return v; }
  // 数组
  if (!raw.empty() && raw.front() == '[') {
    v.type = TomlValue::Type::Array;
    std::string inner = raw.substr(1, raw.size() >= 2 ? raw.size() - 2 : 0);
    bool in_q = false;
    std::string item;
    for (size_t i = 0; i < inner.size(); i++) {
      char c = inner[i];
      if (c == '"') { in_q = !in_q; item += c; }
      else if (c == ',' && !in_q) {
        std::string t = trim(item);
        if (!t.empty()) v.arr.push_back(parse_scalar(t));
        item.clear();
      } else {
        item += c;
      }
    }
    std::string t = trim(item);
    if (!t.empty()) v.arr.push_back(parse_scalar(t));
    return v;
  }
  // 数字
  char* endp = nullptr;
  long long i = std::strtoll(raw.c_str(), &endp, 10);
  if (endp && *endp == '\0' && !raw.empty()) {
    v.type = TomlValue::Type::Int; v.i = i; return v;
  }
  double d = std::strtod(raw.c_str(), &endp);
  if (endp && *endp == '\0' && !raw.empty()) {
    v.type = TomlValue::Type::Double; v.d = d; return v;
  }
  // 兜底当字符串
  v.type = TomlValue::Type::String; v.s = raw; return v;
}

bool Toml::parse(const std::string& text, TomlTable& root,
                 std::map<std::string, TomlTable>& tables) {
  root.clear();
  tables.clear();
  std::string current;  // 当前 [table] 名，"" 表示根

  size_t i = 0;
  while (i < text.size()) {
    size_t nl = text.find('\n', i);
    std::string line = trim(strip_comment(
        text.substr(i, nl == std::string::npos ? std::string::npos : nl - i)));
    i = (nl == std::string::npos) ? text.size() : nl + 1;
    if (line.empty()) continue;

    if (line.front() == '[') {  // [table]、[table.sub] 或 [[array-of-tables]]
      // [[name]] 数组表：重复出现时追加序号，保留全部条目（[[models]] → models, models.1, ...）
      bool is_array = line.size() >= 2 && line[1] == '[';
      size_t open = is_array ? 2 : 1;
      size_t close = line.find(']', open);
      if (close == std::string::npos) return false;
      std::string base = trim(line.substr(open, close - open));
      std::string key = base;
      int n = 0;
      while (tables.count(key)) key = base + "." + std::to_string(++n);
      current = key;
      tables[current] = TomlTable{};
      continue;
    }

    size_t eq = line.find('=');
    if (eq == std::string::npos) return false;
    std::string key = trim(line.substr(0, eq));
    std::string val = trim(line.substr(eq + 1));
    if (key.empty()) return false;

    TomlValue tv = parse_scalar(val);
    if (current.empty()) {
      root[key] = tv;
    } else {
      std::string full = current + "." + key;
      root[full] = tv;
      tables[current][key] = tv;
    }
  }
  return true;
}

const TomlValue* Toml::get(const TomlTable& root,
                           const std::map<std::string, TomlTable>& tables,
                           const std::string& dotted_key) {
  auto rit = root.find(dotted_key);
  if (rit != root.end()) return &rit->second;
  // "table.key" 查 tables
  size_t dot = dotted_key.find('.');
  if (dot != std::string::npos) {
    auto tit = tables.find(dotted_key.substr(0, dot));
    if (tit != tables.end()) {
      auto kit = tit->second.find(dotted_key.substr(dot + 1));
      if (kit != tit->second.end()) return &kit->second;
    }
  }
  return nullptr;
}

}  // namespace da
