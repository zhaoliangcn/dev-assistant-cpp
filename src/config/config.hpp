#pragma once
// 配置：.dev-assistant-models.toml 加载 + ${VAR} 展开（对应 Rust config/）
#include <string>
#include <vector>

#include "config/toml.hpp"

namespace da {

struct ModelConfig {
  std::string name;      // 显示名
  std::string provider;  // provider 类型：openai-compatible / anthropic / ollama 等
  std::string api_url;   // 完整 chat completions URL
  std::string api_key;   // ${VAR} 已展开
  std::string model;     // 模型 ID
  int max_output_tokens = 0;  // D2：单次响应输出上限（发 API；0 = provider 自决）
  double temperature = -1;    // D2：<0 = 不传，provider 自决
};

struct AppConfig {
  std::vector<ModelConfig> models;
  int current = 0;
  int max_turns = 40;
  bool no_approval = false;

  bool load(const std::string& path);
  bool loaded() const { return !models.empty(); }
  const ModelConfig& current_model() const { return models[current]; }
};

// ${VAR} / ${VAR:-default} 展开：先环境变量，后 .env（不覆盖已有）
std::string expand_vars(const std::string& s);

// 加载 .env（工作目录 / 可执行目录），不覆盖已有环境变量
void load_dotenv();

}  // namespace da
