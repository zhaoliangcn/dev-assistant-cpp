#pragma once
#include <memory>

#include "config/config.hpp"
#include "llm/types.hpp"

namespace da {

class LlmClient {
public:
  LlmClient();
  void set_config(const AppConfig& cfg);
  // 按 name 或模型 ID 切换；找到返回 true（未匹配由调用方反馈，对齐 Rust 版）
  bool switch_model(const std::string& name);
  LlmResponse chat(const ChatRequest& req, const DeltaCallback& on_delta);
  const AppConfig& config() const { return config_; }

private:
  AppConfig config_;
  std::unique_ptr<Provider> provider_;
};

}  // namespace da
