#pragma once
#include <string>

#include <nlohmann/json.hpp>

#include "llm/types.hpp"

namespace da {

// Ollama /api/chat URL 兼容：缺路径时自动补全
std::string normalize_ollama_url(const std::string& api_url);

// 构建 Ollama 请求体（role/content 透传 + options.temperature + tools）
// 供实现与单元测试共用。
nlohmann::json build_ollama_payload(const ModelConfig& cfg, const ChatRequest& req,
                                    bool stream);

// 解析 Ollama NDJSON 单行 JSON，累积到 resp 并触发 on_delta。
// 供实现与单元测试共用。
void apply_ollama_chunk(const nlohmann::json& data, LlmResponse& resp,
                        const DeltaCallback& on_delta);

class OllamaProvider : public Provider {
public:
  LlmResponse chat(const ModelConfig& cfg, const ChatRequest& req,
                   const DeltaCallback& on_delta) override;
};

}  // namespace da
