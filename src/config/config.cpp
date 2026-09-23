#include "config/config.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <unistd.h>

namespace da {

std::string expand_vars(const std::string& s) {
  std::string out;
  size_t i = 0;
  while (i < s.size()) {
    if (s[i] == '$' && i + 1 < s.size() && s[i + 1] == '{') {
      size_t close = s.find('}', i + 2);
      if (close != std::string::npos) {
        std::string inner = s.substr(i + 2, close - i - 2);
        std::string name = inner, def;
        size_t colon = inner.find(":-");
        if (colon != std::string::npos) {
          name = inner.substr(0, colon);
          def = inner.substr(colon + 2);
        }
        const char* v = std::getenv(name.c_str());
        if (v && *v) out += v;
        else out += def;
        i = close + 1;
        continue;
      }
    }
    out += s[i++];
  }
  return out;
}

void load_dotenv() {
  for (const char* path : {".env", ".dev-assistant.env"}) {
    std::ifstream f(path);
    if (!f) continue;
    std::string line;
    while (std::getline(f, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      size_t b = line.find_first_not_of(" \t");
      if (b == std::string::npos || line[b] == '#') continue;
      size_t eq = line.find('=', b);
      if (eq == std::string::npos) continue;
      std::string key = line.substr(b, eq - b);
      std::string val = line.substr(eq + 1);
      if (!val.empty() && val.front() == '"' && val.back() == '"')
        val = val.substr(1, val.size() - 2);
      if (!key.empty() && !std::getenv(key.c_str()))
        setenv(key.c_str(), val.c_str(), 0);  // 不覆盖
    }
  }
}

bool AppConfig::load(const std::string& path) {
  std::ifstream f(path);
  if (!f) return false;
  std::stringstream ss;
  ss << f.rdbuf();

  // ${VAR} 预展开
  std::string text = expand_vars(ss.str());

  TomlTable root;
  std::map<std::string, TomlTable> tables;
  if (!Toml::parse(text, root, tables)) return false;

  auto get_str = [&](const std::string& k) -> std::string {
    const TomlValue* v = Toml::get(root, tables, k);
    return v ? v->as_string() : "";
  };
  auto get_int = [&](const std::string& k, int def) -> int {
    const TomlValue* v = Toml::get(root, tables, k);
    return (v && v->type == TomlValue::Type::Int) ? (int)v->i : def;
  };

  // 单模型（根级）或多模型（[[models]] 简化为 [models] 数组 + 平行键不支持，
  // 这里按 Rust 版约定：根级三键为第一个模型，其余模型放 [model.<name>]）
  ModelConfig m0;
  m0.api_url = get_str("api_url");
  m0.api_key = get_str("api_key");
  m0.model = get_str("model");
  m0.name = m0.model;
  if (!m0.model.empty()) models.push_back(m0);

  for (auto& kv : tables) {
    const std::string& tname = kv.first;
    if (tname.rfind("model.", 0) != 0) continue;
    const TomlTable& t = kv.second;
    ModelConfig m;
    m.name = tname.substr(6);
    auto gs = [&](const std::string& k) {
      auto it = t.find(k);
      return it == t.end() ? "" : it->second.as_string();
    };
    m.api_url = gs("api_url");
    m.api_key = gs("api_key");
    m.model = gs("model");
    if (!m.model.empty()) models.push_back(m);
  }

  // Rust 版约定：[[models]] 数组表（models、models.1、...）→ 每个元素一个模型，
  // 按文件中出现顺序入列（"models" 为第 0 个，其余按数字后缀排序）
  {
    std::vector<std::pair<int, const TomlTable*>> ms;
    for (auto& kv : tables) {
      const std::string& tname = kv.first;
      int idx = -1;
      if (tname == "models") {
        idx = 0;
      } else if (tname.rfind("models.", 0) == 0) {
        try {
          idx = std::stoi(tname.substr(7));
        } catch (...) {
        }
      }
      if (idx < 0) continue;
      ms.emplace_back(idx, &kv.second);
    }
    std::sort(ms.begin(), ms.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    for (auto& kv : ms) {
      const TomlTable& t = *kv.second;
      auto gs = [&](const std::string& k) {
        auto it = t.find(k);
        return it == t.end() ? "" : it->second.as_string();
      };
      ModelConfig m;
      m.api_url = gs("api_url");
      m.api_key = gs("api_key");
      m.model = gs("model");
      m.name = gs("name");
      if (m.name.empty()) m.name = m.model;
      if (!m.model.empty()) models.push_back(m);
    }
  }

  max_turns = get_int("max_turns", 40);
  return !models.empty();
}

}  // namespace da
