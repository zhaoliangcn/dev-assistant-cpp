#include "llm/provider/anthropic.hpp"

#include <nlohmann/json.hpp>

#include "llm/http.hpp"

namespace da {

using json = nlohmann::json;

// Anthropic 必填 max_tokens；配置未指定输出上限时用此安全默认（对齐 Rust 版）
static constexpr int kAnthropicDefaultMaxTokens = 4096;

std::string normalize_anthropic_url(const std::string& api_url) {
  std::string url = api_url;
  while (!url.empty() && url.back() == '/') url.pop_back();
  if (url.rfind("/v1/messages") == std::string::npos) url += "/v1/messages";
  return url;
}

void build_anthropic_payload(const ModelConfig& cfg, const ChatRequest& req,
                             json& body, std::string& system) {
  body = json::object();
  body["model"] = cfg.model;
  body["max_tokens"] = req.max_tokens > 0 ? req.max_tokens
                                          : kAnthropicDefaultMaxTokens;
  if (req.temperature >= 0) body["temperature"] = req.temperature;

  json msgs = json::array();
  for (auto& m : req.messages) {
    if (m.role == "system") {
      system = m.content;  // Anthropic 的 system 是独立顶层字段
      continue;
    }
    if (m.role == "tool") {
      // 工具结果 → user 角色的 tool_result 内容块
      msgs.push_back({{"role", "user"},
                      {"content",
                       json::array({{"type", "tool_result"},
                                    {"tool_use_id", m.tool_call_id},
                                    {"content", m.content}})}});
      continue;
    }
    json api_msg{{"role", m.role}};
    if (!m.tool_calls.empty()) {
      // assistant 携带 tool_calls → content 数组（text + tool_use 块）
      json parts = json::array();
      if (!m.content.empty()) parts.push_back({{"type", "text"}, {"text", m.content}});
      for (auto& tc : m.tool_calls) {
        json input = json::parse(tc.arguments, nullptr, false);
        if (input.is_discarded()) input = json::object();
        parts.push_back({{"type", "tool_use"},
                         {"id", tc.id},
                         {"name", tc.name},
                         {"input", input}});
      }
      api_msg["content"] = parts;
    } else {
      api_msg["content"] = m.content;
    }
    msgs.push_back(api_msg);
  }
  body["messages"] = msgs;
  if (!system.empty()) body["system"] = system;

  if (!req.tools_json.empty()) {
    // OpenAI tools 格式 → Anthropic {name, description, input_schema}
    json tools = json::parse(req.tools_json, nullptr, false);
    if (tools.is_array()) {
      json atools = json::array();
      for (auto& t : tools) {
        json fn = t.value("function", json::object());
        json input_schema = fn.value("parameters", json::object());
        if (!input_schema.is_object()) input_schema = json::object();
        atools.push_back({{"name", fn.value("name", "")},
                          {"description", fn.value("description", "")},
                          {"input_schema", input_schema}});
      }
      if (!atools.empty()) body["tools"] = atools;
    }
  }
}

void apply_anthropic_event(const json& data, LlmResponse& resp, bool& saw_text,
                           const DeltaCallback& on_delta) {
  std::string type = data.value("type", "");
  if (type == "content_block_start") {
    json cb = data.value("content_block", json::object());
    if (cb.value("type", "") == "tool_use") {
      int idx = data.value("index", 0);
      if ((int)resp.tool_calls.size() <= idx) resp.tool_calls.resize(idx + 1);
      resp.tool_calls[idx].id = cb.value("id", "");
      resp.tool_calls[idx].name = cb.value("name", "");
    }
  } else if (type == "content_block_delta") {
    json delta = data.value("delta", json::object());
    std::string dtype = delta.value("type", "");
    if (dtype == "text_delta") {
      std::string piece = delta.value("text", "");
      resp.content += piece;
      if (!piece.empty()) {
        saw_text = true;
        if (on_delta) on_delta(piece);
      }
    } else if (dtype == "thinking_delta") {
      // thinking 块：不计入正式 content（对齐"推理流不兜底已产出文本"语义）
      if (!saw_text && on_delta) {
        std::string piece = delta.value("thinking", "");
        if (!piece.empty()) on_delta(piece);
      }
    } else if (dtype == "input_json_delta") {
      int idx = data.value("index", 0);
      if ((int)resp.tool_calls.size() <= idx) resp.tool_calls.resize(idx + 1);
      resp.tool_calls[idx].arguments += delta.value("partial_json", "");
    }
  } else if (type == "message_delta") {
    json delta = data.value("delta", json::object());
    std::string stop = delta.value("stop_reason", "");
    if (!stop.empty()) {
      // Anthropic stop_reason → OpenAI 风格 finish_reason
      resp.finish_reason = stop == "max_tokens" ? "length"
                          : stop == "tool_use"  ? "tool_calls"
                                                : "stop";
    }
    if (data.contains("usage") && data["usage"].is_object())
      resp.completion_tokens = data["usage"].value("output_tokens", 0);
  } else if (type == "message_start") {
    if (data.contains("message") && data["message"].is_object() &&
        data["message"].contains("usage") &&
        data["message"]["usage"].is_object()) {
      json usage = data["message"]["usage"];
      resp.prompt_tokens = usage.value("input_tokens", 0);
    }
  }
}

LlmResponse AnthropicProvider::chat(const ModelConfig& cfg,
                                    const ChatRequest& req,
                                    const DeltaCallback& on_delta) {
  json body;
  std::string system;
  build_anthropic_payload(cfg, req, body, system);

  std::map<std::string, std::string> headers{
      {"Content-Type", "application/json"},
      {"x-api-key", cfg.api_key},
      {"anthropic-version", "2023-06-01"}};

  LlmResponse resp;
  bool saw_text = false;

  auto on_line = [&](const std::string& line) {
    if (line.rfind("data:", 0) != 0) return;
    std::string payload = line.substr(5);
    if (!payload.empty() && payload[0] == ' ') payload.erase(0, 1);
    json data = json::parse(payload, nullptr, false);
    if (data.is_discarded()) return;
    apply_anthropic_event(data, resp, saw_text, on_delta);
  };

  std::string url = normalize_anthropic_url(cfg.api_url);
  auto [status, err] = http_post(url, headers, body.dump(-1, ' ', false), 120, on_line);
  if (status == -1 || status < 200 || status >= 300) {
    LlmResponse fail;
    fail.finish_reason = "http_error: " +
                         (status == -1
                              ? err
                              : "HTTP " + std::to_string(status) + " " + err);
    return fail;
  }
  return resp;
}

}  // namespace da
