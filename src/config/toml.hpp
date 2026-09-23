#pragma once
// TOML 子集解析器（对应 Rust toml crate 用法）
// 支持：key = "value" / 整数 / 浮点 / 布尔 / 字符串数组 / [table] / [table.sub]
//       # 注释、${VAR} 在加载侧展开（见 config.cpp）
#include <map>
#include <string>
#include <vector>

namespace da {

struct TomlValue {
  enum class Type { String, Int, Double, Bool, Array };
  Type type = Type::String;
  std::string s;
  long long i = 0;
  double d = 0.0;
  bool b = false;
  std::vector<TomlValue> arr;

  std::string as_string() const {
    switch (type) {
      case Type::String: return s;
      case Type::Int: return std::to_string(i);
      case Type::Double: return std::to_string(d);
      case Type::Bool: return b ? "true" : "false";
      case Type::Array: return arr.empty() ? "" : arr[0].as_string();
    }
    return {};
  }
};

using TomlTable = std::map<std::string, TomlValue>;

class Toml {
public:
  // 解析文本；root 为根表（含所有 [table] 的键以 "table.key" 形式合入，
  // 同时 table 也单独存放在 tables 中）
  static bool parse(const std::string& text, TomlTable& root,
                    std::map<std::string, TomlTable>& tables);

  // 便捷读取：root 直接键，或 "table.key" 复合键
  static const TomlValue* get(const TomlTable& root,
                              const std::map<std::string, TomlTable>& tables,
                              const std::string& dotted_key);
};

}  // namespace da
