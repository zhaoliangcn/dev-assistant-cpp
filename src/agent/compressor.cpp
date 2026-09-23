#include "agent/compressor.hpp"

#include "agent/token_counter.hpp"

namespace da {

size_t Compressor::history_tokens(const std::vector<ChatMessage>& history) {
  size_t total = 0;
  for (const auto& m : history) {
    total += estimate_tokens(m.role) + estimate_tokens(m.content);
    for (const auto& tc : m.tool_calls)
      total += estimate_tokens(tc.name) + estimate_tokens(tc.arguments);
  }
  return total;
}

bool Compressor::maybe_compress(std::vector<ChatMessage>& history) {
  if (history_tokens(history) <= threshold_) return false;

  // 分离 system 消息（保留）与对话消息
  std::vector<ChatMessage> kept;  // system + 最近 N 轮
  std::vector<ChatMessage> rest;  // 被压缩部分
  size_t sys_end = 0;
  while (sys_end < history.size() && history[sys_end].role == "system")
    sys_end++;
  // system 段
  for (size_t i = 0; i < sys_end; i++) kept.push_back(history[i]);

  // 对话段：保留最近 keep_ 条完整消息（按 tool 配对边界不细拆，
  // 简化实现：从尾部往前收 keep_ 条，若截断在 tool 消息中段则多收至 assistant）
  size_t conv = history.size() - sys_end;
  size_t keep_n = std::min(keep_, conv);
  size_t cut = history.size() - keep_n;
  // 对齐边界：cut 不能落在 tool 消息上（否则 tool_call_id 无主）
  while (cut > sys_end && history[cut].role == "tool") cut--;
  if (cut > sys_end && !history[cut].tool_calls.empty()) cut++;  // assistant 带 tool_calls 的与其 tool 结果成对保留

  for (size_t i = sys_end; i < cut; i++) rest.push_back(history[i]);
  for (size_t i = cut; i < history.size(); i++) kept.push_back(history[i]);

  // 生成摘要占位：列出被压缩消息的角色与首行，保上下文线索
  std::string summary = "[早期对话已压缩] 被压缩消息概要:\n";
  for (const auto& m : rest) {
    std::string first_line = m.content;
    size_t nl = first_line.find('\n');
    if (nl != std::string::npos) first_line = first_line.substr(0, nl);
    if (first_line.size() > 120) first_line = first_line.substr(0, 120) + "…";
    summary += "- " + m.role + ": " + first_line + "\n";
  }

  ChatMessage placeholder{"user", summary, "", {}};
  // 插在 system 之后
  kept.insert(kept.begin() + sys_end, placeholder);
  history = std::move(kept);
  return true;
}

}  // namespace da
