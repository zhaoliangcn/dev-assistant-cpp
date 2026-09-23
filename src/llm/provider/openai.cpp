#include "llm/provider/openai.hpp"

#include <nlohmann/json.hpp>

#include "llm/http.hpp"

namespace da {

using json = nlohmann::json;

void apply_stream_delta(const json& delta, StreamAccum& acc,
                        const DeltaCallback& on_delta) {
  if (delta.contains("content") && delta["content"].is_string()) {
    std::string piece = delta["content"].get<std::string>();
    acc.content += piece;
    if (!piece.empty()) {
      acc.saw_content = true;
      if (on_delta) on_delta(piece);
    }
  } else if (delta.contains("reasoning_content") &&
             delta["reasoning_content"].is_string()) {
    // 推理型模型：正式 content 缺席时，推理流实时展示并累积兜底
    std::string piece = delta["reasoning_content"].get<std::string>();
    if (!acc.saw_content) {
      acc.reasoning += piece;
      if (on_delta && !piece.empty()) on_delta(piece);
    }
  }
  if (delta.contains("tool_calls") && delta["tool_calls"].is_array()) {
    for (auto& tc : delta["tool_calls"]) {
      int idx = tc.value("index", 0);
      auto& slot = acc.pending_calls[idx];
      if (tc.contains("id") && tc["id"].is_string() &&
          !tc["id"].get<std::string>().empty())
        slot.id = tc["id"].get<std::string>();
      if (tc.contains("function")) {
        auto& fn = tc["function"];
        if (fn.contains("name") && fn["name"].is_string() &&
            !fn["name"].get<std::string>().empty())
          slot.name += fn["name"].get<std::string>();
        if (fn.contains("arguments") && fn["arguments"].is_string())
          slot.arguments += fn["arguments"].get<std::string>();
      }
    }
  }
}

std::string effective_content(const StreamAccum& acc) {
  return acc.content.empty() ? acc.reasoning : acc.content;
}

std::string normalize_chat_url(const std::string& api_url) {
  std::string url = api_url;
  if (url.rfind("/chat/completions") == std::string::npos) {
    if (!url.empty() && url.back() != '/') url += "/";
    url += "chat/completions";
  }
  return url;
}

std::string http_error_reason(int status, const std::string& err) {
  return "http_error: " +
         (status == -1 ? err : "HTTP " + std::to_string(status) + " " + err);
}

static json msg_to_json(const ChatMessage& m) {
  json j;
  j["role"] = m.role;
  if (m.role == "tool") {
    j["content"] = m.content;
    j["tool_call_id"] = m.tool_call_id;
    return j;
  }
  j["content"] = m.content;
  if (!m.tool_calls.empty()) {
    json arr = json::array();
    for (auto& tc : m.tool_calls) {
      arr.push_back({{"id", tc.id},
                     {"type", "function"},
                     {"function", {{"name", tc.name}, {"arguments", tc.arguments}}}});
    }
    j["tool_calls"] = arr;
    if (m.content.empty()) j["content"] = nullptr;
  }
  return j;
}

LlmResponse OpenAiProvider::chat(const ModelConfig& cfg, const ChatRequest& req,
                                 const DeltaCallback& on_delta) {
  json body;
  body["model"] = cfg.model;
  json msgs = json::array();
  for (auto& m : req.messages) msgs.push_back(msg_to_json(m));
  body["messages"] = msgs;
  body["stream"] = true;
  if (req.temperature >= 0) body["temperature"] = req.temperature;
  if (req.max_tokens > 0) body["max_tokens"] = req.max_tokens;
  if (!req.tools_json.empty()) body["tools"] = json::parse(req.tools_json);

  std::map<std::string, std::string> headers{
      {"Content-Type", "application/json"}};
  if (!cfg.api_key.empty()) headers["Authorization"] = "Bearer " + cfg.api_key;

  LlmResponse resp;
  StreamAccum acc;

  auto on_line = [&](const std::string& line) {
    if (line.rfind("data:", 0) != 0) return;
    std::string payload = line.substr(5);
    if (!payload.empty() && payload[0] == ' ') payload.erase(0, 1);
    if (payload == "[DONE]") return;
    json chunk = json::parse(payload, nullptr, false);
    if (chunk.is_discarded()) return;
    if (chunk.contains("usage") && chunk["usage"].is_object()) {
      auto& u = chunk["usage"];
      acc.prompt_tokens = u.value("prompt_tokens", 0);
      acc.completion_tokens = u.value("completion_tokens", 0);
    }
    if (!chunk.contains("choices") || !chunk["choices"].is_array() ||
        chunk["choices"].empty())
      return;
    auto& choice = chunk["choices"][0];
    acc.finish_reason = choice.value("finish_reason", "");
    if (!choice.contains("delta")) return;
    apply_stream_delta(choice["delta"], acc, on_delta);
  };

  // api_url 兼容两种写法：完整 chat/completions 地址，或基础地址（自动补全）
  std::string url = normalize_chat_url(cfg.api_url);

  auto [status, err] = http_post(url, headers, body.dump(), 120, on_line);
  if (status == -1 || status < 200 || status >= 300) {
    LlmResponse fail;
    fail.finish_reason = http_error_reason(status, err);
    return fail;
  }
  // 推理型模型：未产出正式 content 时，以 reasoning_content 作为回复内容
  resp.content = effective_content(acc);
  resp.finish_reason = acc.finish_reason;
  resp.prompt_tokens = acc.prompt_tokens;
  resp.completion_tokens = acc.completion_tokens;
  for (auto& [idx, tc] : acc.pending_calls) resp.tool_calls.push_back(tc);
  return resp;
}

}  // namespace da
