#pragma once
// Agent 主循环（对应 Rust agent/mod.rs 的 start_turn/step/run）
#include <optional>
#include <string>
#include <vector>

#include "llm/client.hpp"
#include "security/approval.hpp"
#include "security/policy.hpp"
#include "tools/registry.hpp"

namespace da {

// 工具执行安全门：路径校验 → 危险命令硬拦 → 审批（与 interactive 解耦）。
// 返回 std::nullopt = 放行；否则为拒绝原因。供 Agent::execute_tool 与单测共用。
// approval 须非 const：Session 级授权会写入 grants_
std::optional<std::string> tool_gate(const ToolDefinition& def,
                                     const nlohmann::json& args,
                                     const SecurityPolicy& security,
                                     ApprovalManager& approval,
                                     bool interactive);

class Agent {
public:
  Agent(LlmClient& llm, ToolRegistry& tools, SecurityPolicy& security,
        ApprovalManager& approval)
      : llm_(llm), tools_(tools), security_(security), approval_(approval) {}

  // 子代理深度（父代理为 0，spawn_subagent 传递 depth+1）
  void set_depth(int d) { depth_ = d; }

  // 执行一次完整交互：start_turn → loop step → 结束
  // 返回 0 成功；on_delta 可选：每个 token 增量回调（流式转发给调用方）
  int run(const std::string& user_input, bool interactive,
          const DeltaCallback& on_delta = {});

  const std::vector<ChatMessage>& history() const { return history_; }
  void clear_history() { history_.clear(); }
  // --resume：外部（App）重建历史后注入
  void set_history(std::vector<ChatMessage> h) { history_ = std::move(h); }

private:
  bool step(bool interactive, const DeltaCallback& on_delta);  // 一次 LLM 调用 + 工具执行；返回是否继续
  ToolResult execute_tool(const ChatMessage::ToolCall& tc, bool interactive);

  LlmClient& llm_;
  ToolRegistry& tools_;
  SecurityPolicy& security_;
  ApprovalManager& approval_;
  std::vector<ChatMessage> history_;
  int max_turns_ = 40;
  int depth_ = 0;  // 子代理深度 ≤3
};

}  // namespace da
