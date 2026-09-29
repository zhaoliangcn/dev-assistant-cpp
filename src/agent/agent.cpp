#include "agent/agent.hpp"

#include <sys/stat.h>

#include <cstdio>
#include <ctime>

#include "agent/identity.hpp"
#include "persist/journal.hpp"
#include "prompt.hpp"
#include "utils/utf8.hpp"
#include "tools/file_tools.hpp"
#include "tools/subagent.hpp"

namespace da {

static ToolContext make_ctx(SecurityPolicy& sec) {
  ToolContext ctx;
  ctx.security = &sec;
  ctx.workspace = sec.workspace();
  return ctx;
}

namespace {
// S4：审计日志——每次工具调用（含 --no-approval 放行与各类拒绝）追加
// <workspace>/.dev-assistant/audit.jsonl（0600）。参数摘要截断并脱敏。
void audit_tool_call(const SecurityPolicy& sec, const std::string& tool,
                     const std::string& args_raw, bool allowed,
                     const std::string& reason) {
  std::string dir = sec.workspace() + "/.dev-assistant";
  ::mkdir(dir.c_str(), 0700);  // 已存在时无害
  nlohmann::json entry{
      {"ts", (long)std::time(nullptr)},
      {"tool", tool},
      {"args", redact_secrets(args_raw.size() > 512
                                  ? args_raw.substr(0, 512) + "...(截断)"
                                  : args_raw)},
      {"decision", allowed ? "allowed" : "denied"}};
  if (!reason.empty()) entry["reason"] = reason;
  std::string path = dir + "/audit.jsonl";
  std::FILE* f = std::fopen(path.c_str(), "a");
  if (!f) return;
  ::chmod(path.c_str(), 0600);
  std::fputs(entry.dump(-1, ' ', false).c_str(), f);
  std::fputc('\n', f);
  std::fclose(f);
}
}  // namespace

int Agent::run(const std::string& user_input, bool interactive,
               const DeltaCallback& on_delta) {
  // start_turn：system prompt + 用户消息
  if (history_.empty())
    history_.push_back(
        {"system", sanitize_utf8(build_system_prompt(nullptr)), "", {}});
  history_.push_back({"user", sanitize_utf8(user_input), "", {}});
  // B3：真写用户消息（脱敏后落盘，供 --resume 重建）
  if (journal_)
    journal_->append("user_message",
                     {{"content", sanitize_utf8(redact_secrets(user_input))}});

  // loop step：直到无 tool_calls 或超轮次
  for (int turn = 0; turn < max_turns_; turn++) {
    compressor_.maybe_compress(history_);  // B1：超阈值先压缩再请求
    if (!step(interactive, on_delta)) break;
  }
  return 0;
}

bool Agent::step(bool interactive, const DeltaCallback& on_delta) {
  ChatRequest req;
  req.messages = history_;
  // D2：输出上限/温度来自当前模型配置（0/<0 = provider 自决）
  const ModelConfig& mc = llm_.config().current_model();
  req.max_tokens = mc.max_output_tokens;
  req.temperature = mc.temperature;
  req.tools_json = tools_.to_openai_schema().dump();

  LlmResponse resp;
  bool saw_output = false;   // 本 step 是否有流式可见输出（决定收尾换行）
  bool printed_any = false;  // 已输出非空白内容（过滤模型前导空行/空白 delta）
  resp = llm_.chat(req, [interactive, &on_delta, &saw_output, &printed_any](
                            const std::string& delta) {
        // 模型常以前导 "\n" 开头（思维链/正文均是，甚至混在同一 delta 内）；
        // 首个可见输出前丢弃纯空白 delta，并对首个混合 delta 剥离前导空白
        if (!printed_any) {
          size_t first = delta.find_first_not_of("\n\r \t");
          if (first == std::string::npos) {
            if (on_delta) on_delta(delta);  // WS 侧转发原文，仅本地不显示
            return;
          }
          printed_any = true;
          if (interactive && first > 0) {
            std::fputs(delta.c_str() + first, stdout);  // 剥离前导空白后输出
            std::fflush(stdout);
            saw_output = true;
            if (on_delta) on_delta(delta);
            return;
          }
        }
        if (interactive) {
          std::fputs(delta.c_str(), stdout);
          std::fflush(stdout);
          saw_output = true;
        }
        if (on_delta) on_delta(delta);  // 流式转发（Web WS 推送）
      });
  // 仅在有可见输出的 step 收尾换行（工具调用轮无输出时不再产生空行）
  if (interactive && saw_output) std::fputs("\n", stdout);

  if (resp.finish_reason.rfind("http_error:", 0) == 0) {
    std::fprintf(stderr, "LLM 请求失败: %s\n", resp.finish_reason.c_str());
    return false;
  }

  // 无工具调用 → 本轮结束
  if (resp.tool_calls.empty()) {
    history_.push_back({"assistant", sanitize_utf8(resp.content), "", {}});
    if (journal_)
      journal_->append("assistant_message",
                       {{"content", sanitize_utf8(redact_secrets(resp.content))}});
    return false;
  }

  // 有工具调用：先入 assistant 消息（含 tool_calls），逐个执行
  history_.push_back({"assistant", sanitize_utf8(resp.content), "",
                       resp.tool_calls});
  if (journal_) {
    nlohmann::json tcs = nlohmann::json::array();
    for (auto& tc : resp.tool_calls)
      tcs.push_back({{"id", tc.id}, {"name", tc.name},
                     {"arguments", tc.arguments}});
    journal_->append("assistant_message",
                     {{"content", sanitize_utf8(redact_secrets(resp.content))},
                      {"tool_calls", tcs}});
  }
  for (auto& tc : resp.tool_calls) {
    ToolResult r = execute_tool(tc, interactive);
    history_.push_back({"tool", sanitize_utf8(r.output), tc.id, {}});
    if (journal_)
      journal_->append("tool_result", {{"tool", tc.name},
                                       {"tool_call_id", tc.id},
                             {"content", sanitize_utf8(redact_secrets(r.output))}});
  }
  return true;  // 继续下一 step（让模型消化工具结果）
}

ToolResult Agent::execute_tool(const ChatMessage::ToolCall& tc,
                               bool interactive) {
  // spawn_subagent 由 Agent 层直接处理：构造受限 registry + 独立上下文
  if (tc.name == "spawn_subagent") {
    nlohmann::json args = nlohmann::json::parse(tc.arguments, nullptr, false);
    if (args.is_discarded()) return {false, "工具参数 JSON 解析失败"};
    int child_depth = depth_ + 1;
    if (child_depth > kMaxSubagentDepth)
      return {false, "子代理深度超限（最大 " + std::to_string(kMaxSubagentDepth) + "）"};
    std::string prompt = args.value("prompt", "");
    if (prompt.empty()) return {false, "prompt 不能为空"};

    // 受限工具集：按身份默认工具过滤（D1：显式剔除 spawn_subagent，
    // 防递归派生——不再依赖 Agent 层特判与 registry handler 的隐式双路径）
    Identity role = identity_from_string(args.value("role", "general"));
    ToolRegistry sub_tools;
    for (const auto& name : identity_default_tools(role))
      if (name != "spawn_subagent")
        if (const ToolDefinition* def = tools_.find(name))
          sub_tools.register_tool(*def);

    Agent child(llm_, sub_tools, security_, approval_);
    child.set_depth(child_depth);
    child.run(prompt, /*interactive=*/false);
    // 取最后一条 assistant 文本作为子代理结果
    for (auto it = child.history().rbegin(); it != child.history().rend(); ++it) {
      if (it->role == "assistant" && !it->tool_calls.empty()) continue;
      if (it->role == "assistant") return {true, it->content};
    }
    return {false, "子代理未产出文本结果"};
  }

  const ToolDefinition* def = tools_.find(tc.name);
  if (!def) {
    audit_tool_call(security_, tc.name, tc.arguments, false, "未知工具");
    return {false, "未知工具: " + tc.name};
  }

  // 安全顺序：静态校验 → 危险命令硬拦 → 审批 → 执行（tool_gate 供单测共用）
  nlohmann::json args = nlohmann::json::parse(tc.arguments, nullptr, false);
  if (args.is_discarded()) {
    audit_tool_call(security_, tc.name, tc.arguments, false,
                    "工具参数 JSON 解析失败");
    return {false, "工具参数 JSON 解析失败"};
  }

  if (auto deny = tool_gate(*def, args, security_, approval_, interactive)) {
    audit_tool_call(security_, tc.name, tc.arguments, false, *deny);
    return {false, *deny};
  }

  ToolContext ctx = make_ctx(security_);
  ToolResult r = tools_.dispatch(tc.name, args, ctx);
  // S4：放行也记录（含 --no-approval 模式——审计不随模式放宽）
  audit_tool_call(security_, tc.name, tc.arguments, r.ok,
                  r.ok ? "" : "工具执行失败");
  return r;
}

}  // namespace da
