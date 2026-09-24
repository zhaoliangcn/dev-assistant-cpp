#include "agent/agent.hpp"

#include <cstdio>

#include "agent/identity.hpp"
#include "persist/journal.hpp"
#include "prompt.hpp"
#include "tools/file_tools.hpp"
#include "tools/subagent.hpp"

namespace da {

static ToolContext make_ctx(SecurityPolicy& sec) {
  ToolContext ctx;
  ctx.security = &sec;
  ctx.workspace = sec.workspace();
  return ctx;
}

int Agent::run(const std::string& user_input, bool interactive,
               const DeltaCallback& on_delta) {
  // start_turn：system prompt + 用户消息
  if (history_.empty())
    history_.push_back({"system", build_system_prompt(nullptr), "", {}});
  history_.push_back({"user", user_input, "", {}});

  // loop step：直到无 tool_calls 或超轮次
  for (int turn = 0; turn < max_turns_; turn++) {
    if (!step(interactive, on_delta)) break;
  }
  return 0;
}

bool Agent::step(bool interactive, const DeltaCallback& on_delta) {
  ChatRequest req;
  req.messages = history_;
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
    history_.push_back({"assistant", resp.content, "", {}});
    return false;
  }

  // 有工具调用：先入 assistant 消息（含 tool_calls），逐个执行
  history_.push_back({"assistant", resp.content, "", resp.tool_calls});
  for (auto& tc : resp.tool_calls) {
    ToolResult r = execute_tool(tc, interactive);
    history_.push_back({"tool", r.output, tc.id, {}});
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

    // 受限工具集：按身份默认工具过滤（子代理不给 spawn_subagent，防递归派生）
    Identity role = identity_from_string(args.value("role", "general"));
    ToolRegistry sub_tools;
    for (const auto& name : identity_default_tools(role))
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
  if (!def) return {false, "未知工具: " + tc.name};

  // 安全顺序：静态校验 → 审批 → 执行
  nlohmann::json args = nlohmann::json::parse(tc.arguments, nullptr, false);
  if (args.is_discarded()) return {false, "工具参数 JSON 解析失败"};

  // 路径类参数校验
  if (def->path_check != PathCheck::None) {
    std::string path = args.value("path", "");
    std::string reason;
    if (!path.empty() &&
        !security_.validate_path(path, def->path_check, reason))
      return {false, "安全策略拒绝: " + reason};
  }

  // 审批（写类 / 执行类）
  if (def->needs_approval && interactive) {
    std::string title = tc.name + ": " +
                        args.value("path", args.value("command", ""));
    if (!approval_.request(title, ApprovalScope::Tool, tc.name))
      return {false, "用户拒绝执行"};
  }

  ToolContext ctx = make_ctx(security_);
  ToolResult r = tools_.dispatch(tc.name, args, ctx);
  return r;
}

}  // namespace da
