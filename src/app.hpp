#pragma once
// App 组装层（对应 Rust app.rs）：装配 Config / LlmClient / ToolRegistry /
// SecurityPolicy / ApprovalManager / Skills / Dream / Hooks
#include <string>
#include <vector>

#include "dream/dream.hpp"
#include "hooks/hooks.hpp"
#include "llm/client.hpp"
#include "security/approval.hpp"
#include "security/policy.hpp"
#include "skills/skills.hpp"
#include "tools/registry.hpp"

namespace da {

class Agent;

class App {
public:
  // 加载配置 + 注册工具 + 扫描技能/钩子 + 打开会话日志；
  // config_path 为空时用工作目录下的 .dev-assistant-models.toml（对应 --config）
  bool init(const std::string& config_path = "");
  // 一次完整交互（REPL 或 -m 模式共用）；结束后触发 dream.ingest 与 session 钩子
  int process_message(const std::string& input);

  LlmClient& llm() { return llm_; }
  ToolRegistry& tools() { return tools_; }
  SecurityPolicy& security() { return security_; }
  ApprovalManager& approval() { return approval_; }
  SkillRegistry& skills() { return skills_; }
  DreamStore& dream() { return dream_; }
  HookManager& hooks() { return hooks_; }
  bool no_approval() const { return no_approval_; }
  void set_no_approval(bool v) { no_approval_ = v; }

  // 会话收尾：session_end 钩子 + 经验入记忆库
  void finish_session();

  // 最近一轮对话历史（供 REPL /history 查看；对应 Rust history_messages）
  const std::vector<ChatMessage>& last_history() const { return last_history_; }

  // ── 上下文预算（对应 Rust ContextBudget）──
  void set_max_tokens(long v) { max_tokens_ = v > 0 ? v : 262144; }
  long max_tokens() const { return max_tokens_; }

  struct BudgetReport {
    long system_tokens = 0;     // 系统提示词估算
    long history_tokens = 0;    // 对话历史估算
    long tool_schema_tokens = 0;// 工具 schema 估算
    long total_tokens = 0;
    long max_tokens = 0;
    double utilization = 0.0;   // 0.0 ~ 1.0
    long estimated_room = 0;
    // 压力等级：0 正常 / 1 提示 / 2 临界 / 3 爆满
    int pressure = 0;
  };
  BudgetReport budget_report() const;

  // --resume：从最近会话的 events.jsonl 重建对话历史（返回恢复消息数）
  int resume_last_session();

private:
  bool load_config(const std::string& path);
  LlmClient llm_;
  ToolRegistry tools_;
  SecurityPolicy security_;
  ApprovalManager approval_{false};
  SkillRegistry skills_;
  DreamStore dream_{".dev-assistant/memories"};
  HookManager hooks_;
  bool no_approval_ = false;
  std::string last_session_dir_;
  std::vector<ChatMessage> last_history_;
  long max_tokens_ = 262144;  // 上下文预算（--max-tokens；不发 API）
};

}  // namespace da
