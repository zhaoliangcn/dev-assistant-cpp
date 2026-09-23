#pragma once
#include <string>

#include <nlohmann/json.hpp>

#include "llm/types.hpp"

namespace da {

// Anthropic messages API URL 兼容：缺 /v1/messages 时自动补全
std::string normalize_anthropic_url(const std::string& api_url);

// OpenAI 风格消息列表 → Anthropic 请求体字段（system 独立 / tool_result 归 user /
// tool_calls 转 content 数组）。供实现与单元测试共用。
void build_anthropic_payload(const ModelConfig& cfg, const ChatRequest& req,
                             nlohmann::json& body, std::string& system);

// 解析 Anthropic SSE data 行（content_block_delta / input_json_delta /
// message_delta 等），累积到 resp 并触发 on_delta。供实现与单元测试共用。
void apply_anthropic_event(const nlohmann::json& data, LlmResponse& resp,
                           bool& saw_text, const DeltaCallback& on_delta);

class AnthropicProvider : public Provider {
public:
  LlmResponse chat(const ModelConfig& cfg, const ChatRequest& req,
                   const DeltaCallback& on_delta) override;
};

}  // namespace da
