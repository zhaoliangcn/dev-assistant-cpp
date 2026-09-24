#include "llm/client.hpp"

#include <cstdlib>

#include <thread>
#include <chrono>

#include "llm/provider/anthropic.hpp"
#include "llm/provider/ollama.hpp"
#include "llm/provider/openai.hpp"

namespace da {

// C5：仅传输错误（无状态码）与 429/5xx 值得重试；
// 400/401/403/404 等参数/鉴权错误重试无意义（白等 15s），立即失败。
// 注意 "http_error: HTTP ..." 冒号后有空格，须先剥离再判定
// （此前 substr(11) 残留空格导致全部误判为传输错误而重试）。
bool retryable_http_error(const std::string& finish_reason) {
  if (finish_reason.rfind("http_error:", 0) != 0) return false;
  std::string rest = finish_reason.substr(11);  // strlen("http_error:")
  size_t nb = rest.find_first_not_of(" \t");
  if (nb == std::string::npos) return true;  // 只有前缀，按传输错误处理
  rest = rest.substr(nb);
  if (rest.rfind("HTTP ", 0) != 0) return true;  // 传输层错误（无状态码）
  int code = std::atoi(rest.c_str() + 5);
  return code == 429 || (code >= 500 && code <= 599);
}

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
  // 重试：指数退避（最多 5 次）；条件 = 传输错误 / 429 / 5xx（C5）
  LlmResponse resp;
  for (int attempt = 0; attempt < 5; attempt++) {
    if (attempt > 0) {
      int delay_ms = 500 * (1 << attempt);  // 1s, 2s, 4s, 8s
      std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    }
    resp = provider_->chat(config_.current_model(), req, on_delta);
    if (resp.finish_reason.rfind("http_error:", 0) != 0) return resp;
    if (!retryable_http_error(resp.finish_reason)) return resp;  // 4xx 立即失败
  }
  return resp;
}

}  // namespace da
