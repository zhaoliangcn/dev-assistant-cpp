#pragma once
// LLM 层：请求/响应结构 + Provider 抽象（对应 Rust llm/）
#include <functional>
#include <string>
#include <vector>

#include "config/config.hpp"

namespace da {

struct ChatMessage {
  std::string role;     // system / user / assistant / tool
  std::string content;
  std::string tool_call_id;  // role==tool 时使用
  // assistant 消息携带的 tool_calls（原样回传）
  struct ToolCall {
    std::string id, name, arguments;  // arguments 为 JSON 文本
  };
  std::vector<ToolCall> tool_calls;
};

struct ChatRequest {
  std::vector<ChatMessage> messages;
  std::string tools_json;  // 预生成的 tools JSON 数组文本（空 = 不带工具）
  double temperature = -1; // <0 表示不传
  int max_tokens = 0;      // 0 表示不传
};

struct LlmResponse {
  std::string content;
  std::vector<ChatMessage::ToolCall> tool_calls;
  std::string finish_reason;
  int prompt_tokens = 0, completion_tokens = 0;
};

using DeltaCallback = std::function<void(const std::string& delta)>;

class Provider {
public:
  virtual ~Provider() = default;
  virtual LlmResponse chat(const ModelConfig& cfg,
                           const ChatRequest& req,
                           const DeltaCallback& on_delta) = 0;
};

}  // namespace da
