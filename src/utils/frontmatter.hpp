#pragma once
// YAML frontmatter 子集解析器（对应 Rust utils/frontmatter.rs）
// 支持：标量 / "- item" 数组 / 一层嵌套 map（缩进 2 空格）
#include <map>
#include <string>
#include <variant>
#include <vector>

namespace da {

using YamlValue = std::variant<std::string, std::vector<std::string>,
                               std::map<std::string, std::string>>;

struct Frontmatter {
  YamlValue get(const std::string& key) const {
    auto it = fields.find(key);
    if (it == fields.end()) return YamlValue{std::string{}};
    return it->second;
  }
  std::string get_string(const std::string& key) const {
    auto it = fields.find(key);
    if (it == fields.end() || !std::holds_alternative<std::string>(it->second))
      return {};
    return std::get<std::string>(it->second);
  }
  std::vector<std::string> get_array(const std::string& key) const {
    auto it = fields.find(key);
    if (it == fields.end()) return {};
    if (auto* a = std::get_if<std::vector<std::string>>(&it->second)) return *a;
    if (auto* s = std::get_if<std::string>(&it->second)) return {*s};
    return {};
  }
  bool has(const std::string& key) const { return fields.count(key) > 0; }

  std::map<std::string, YamlValue> fields;
};

// 解析 "---\n...\n---\n" 包裹的 frontmatter；body 为剩余正文
// 解析失败返回 false（frontmatter 非必需）
bool parse_frontmatter(const std::string& text, Frontmatter& fm,
                       std::string& body);

}  // namespace da
