#pragma once
#include <memory>

#include "config/config.hpp"
#include "llm/types.hpp"

namespace da {

class LlmClient {
public:
  LlmClient();
  void set_config(const AppConfig& cfg);
  void switch_model(const std::string& name);
  LlmResponse chat(const ChatRequest& req, const DeltaCallback& on_delta);
  const AppConfig& config() const { return config_; }

private:
  AppConfig config_;
  std::unique_ptr<Provider> provider_;
};

}  // namespace da
