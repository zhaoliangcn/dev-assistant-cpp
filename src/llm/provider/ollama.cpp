#include "llm/provider/ollama.hpp"

#include <nlohmann/json.hpp>

#include "llm/http.hpp"

namespace da {

using json = nlohmann::json;

std::string normalize_ollama_url(const std::string& api_url) {
  std::string url = api_url;
  while (!url.empty() && url.back() == '/') url.pop_back();
  if (url.rfind("/api/chat") == std::string::npos) url += "/api/chat";
  return url;
}

json build_ollama_payload(const ModelConfig& cfg, const ChatRequest& req,
                          bool stream) {
  json msgs = json::array();
  for (auto& m : req.messages) {
    json jm{{"role", m.role}, {"content", m.content}};
    msgs.push_back(jm);
  }
  json body{{"model", cfg.model},
            {"messages", msgs},
            {"stream", stream},
            {"options", json{{"temperature", req.temperature >= 0 ? req.temperature : 0.6}}}};
  if (!req.tools_json.empty()) {
    json tools = json::parse(req.tools_json, nullptr, false);
    if (tools.is_array() && !tools.empty()) body["tools"] = tools;
  }
  return body;
}

void apply_ollama_chunk(const json& data, LlmResponse& resp,
                        const DeltaCallback& on_delta) {
  if (data.contains("message") && data["message"].is_object()) {
    auto& msg = data["message"];
    if (msg.contains("content") && msg["content"].is_string()) {
      std::string piece = msg["content"].get<std::string>();
      if (!piece.empty()) {
        resp.content += piece;
        if (on_delta) on_delta(piece);
      }
    }
    if (msg.contains("tool_calls") && msg["tool_calls"].is_array()) {
      for (auto& tc : msg["tool_calls"]) {
        json fn = tc.value("function", json::object());
        ChatMessage::ToolCall call;
        call.id = tc.value("id", "");
        call.name = fn.value("name", "");
        json args = fn.value("arguments", json::object());
        call.arguments =
            args.is_string() ? args.get<std::string>() : args.dump();
        resp.tool_calls.push_back(call);
      }
    }
  }
  if (data.contains("done") && data["done"].is_boolean() && data["done"].get<bool>())
    resp.finish_reason = "stop";
  if (data.contains("prompt_eval_count") && data["prompt_eval_count"].is_number())
    resp.prompt_tokens = data["prompt_eval_count"].get<int>();
  if (data.contains("eval_count") && data["eval_count"].is_number())
    resp.completion_tokens = data["eval_count"].get<int>();
}

LlmResponse OllamaProvider::chat(const ModelConfig& cfg, const ChatRequest& req,
                                 const DeltaCallback& on_delta) {
  json body = build_ollama_payload(cfg, req, true);

  std::map<std::string, std::string> headers{
      {"Content-Type", "application/json"}};

  LlmResponse resp;

  auto on_line = [&](const std::string& line) {
    if (line.empty()) return;
    json data = json::parse(line, nullptr, false);
    if (data.is_discarded()) return;
    apply_ollama_chunk(data, resp, on_delta);
  };

  std::string url = normalize_ollama_url(cfg.api_url);
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
