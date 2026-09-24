#pragma once
#include <map>
#include <string>

#include <nlohmann/json.hpp>

#include "llm/types.hpp"

namespace da {

// 流式增量累积状态（供单元测试与 chat() 共用）
struct StreamAccum {
  std::string content;
  std::string reasoning;   // reasoning_content 累积（正式 content 缺席时的兜底）
  bool saw_content = false;
  bool saw_reasoning = false;  // 已输出过思维流（首次输出前发区分标记）
  std::string finish_reason;
  int prompt_tokens = 0;
  int completion_tokens = 0;
  std::map<int, ChatMessage::ToolCall> pending_calls;
};

// 解析单个流式 delta（content / reasoning_content / tool_calls）
void apply_stream_delta(const nlohmann::json& delta, StreamAccum& acc,
                        const DeltaCallback& on_delta);

// 正式 content 缺席时以推理流兜底
std::string effective_content(const StreamAccum& acc);

// api_url 兼容：缺 /chat/completions 时自动补全
std::string normalize_chat_url(const std::string& api_url);

// 统一失败文案（传输错误 / 非 2xx 状态码 + 响应体预览）
std::string http_error_reason(int status, const std::string& err);

class OpenAiProvider : public Provider {
public:
  LlmResponse chat(const ModelConfig& cfg, const ChatRequest& req,
                   const DeltaCallback& on_delta) override;
};

}  // namespace da
