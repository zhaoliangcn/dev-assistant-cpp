#pragma once
// 上下文压缩（对应 Rust agent/compressor.rs）
// token 超阈值时保留最近 N 轮（默认 6，可配置），生成摘要占位
#include <string>
#include <vector>

#include "agent/token_counter.hpp"
#include "llm/types.hpp"

namespace da {

class Compressor {
public:
  explicit Compressor(size_t threshold_tokens = 60000,
                      size_t keep_recent_turns = 6)
      : threshold_(threshold_tokens), keep_(keep_recent_turns) {}

  // 检查历史是否超限；超限则原地压缩为
  // [system, 摘要占位 user, 最近 N 轮消息]
  // 返回是否发生了压缩
  bool maybe_compress(std::vector<ChatMessage>& history);

  void set_threshold(size_t tokens) { threshold_ = tokens; }
  void set_keep_recent(size_t n) { keep_ = n; }

  static size_t history_tokens(const std::vector<ChatMessage>& history);

private:
  size_t threshold_;
  size_t keep_;
};

}  // namespace da
