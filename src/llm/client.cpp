#include "llm/client.hpp"

#include <thread>
#include <chrono>

#include "llm/provider/anthropic.hpp"
#include "llm/provider/ollama.hpp"
#include "llm/provider/openai.hpp"

namespace da {

LlmClient::LlmClient() : provider_(std::make_unique<OpenAiProvider>()) {}

void LlmClient::set_config(const AppConfig& cfg) {
  config_ = cfg;
  // 按当前模型的 provider 字段选路（切模型时同步更新）：
  // anthropic/claude → Anthropic；ollama → Ollama；其余（openai*/shangtang 等
  // OpenAI 兼容服务）→ OpenAiProvider
  const std::string& p = config_.current_model().provider;
  if (p == "anthropic" || p == "claude")
    provider_ = std::make_unique<AnthropicProvider>();
  else if (p == "ollama")
    provider_ = std::make_unique<OllamaProvider>();
  else
    provider_ = std::make_unique<OpenAiProvider>();
}

bool LlmClient::switch_model(const std::string& name) {
  for (size_t i = 0; i < config_.models.size(); i++) {
    if (config_.models[i].name == name || config_.models[i].model == name) {
      config_.current = (int)i;
      // 重选 provider：不同模型可能属于不同 provider 家族
      AppConfig tmp = config_;
      set_config(tmp);
      return true;
    }
  }
  return false;
}

LlmResponse LlmClient::chat(const ChatRequest& req, const DeltaCallback& on_delta) {
  // 重试：指数退避；条件 = HTTP 429 或 502/503/504（最多 5 次）
  LlmResponse resp;
  for (int attempt = 0; attempt < 5; attempt++) {
    if (attempt > 0) {
      int delay_ms = 500 * (1 << attempt);  // 1s, 2s, 4s, 8s
      std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    }
    resp = provider_->chat(config_.current_model(), req, on_delta);
    if (resp.finish_reason.rfind("http_error:", 0) != 0) return resp;
    // HTTP 错误：所有传输失败都退避重试（简化；可细分 429/5xx）
  }
  return resp;
}

}  // namespace da
