// 工具执行安全门（供 Agent::execute_tool 与单测共用）
// 顺序：路径校验 → 危险命令硬拦 → 审批（与 interactive 解耦）
#include <optional>

#include "agent/agent.hpp"

namespace da {

std::optional<std::string> tool_gate(const ToolDefinition& def,
                                     const nlohmann::json& args,
                                     const SecurityPolicy& security,
                                     ApprovalManager& approval,
                                     bool interactive) {
  // 1) 路径类参数校验
  if (def.path_check != PathCheck::None) {
    std::string path = args.value("path", "");
    std::string reason;
    if (!path.empty() && !security.validate_path(path, def.path_check, reason))
      return "安全策略拒绝: " + reason;
  }

  // 2) 危险命令硬拦（S2）：评估 command + args 拼接的完整命令行
  //    （防 "rm" + ["-rf", …] 拆分绕过）；High/Blocked 即使 --no-approval 也拒绝
  std::string cmd = args.value("command", "");
  if (args.contains("args") && args["args"].is_array())
    for (const auto& a : args["args"])
      if (a.is_string()) cmd += " " + a.get<std::string>();
  if (!cmd.empty()) {
    DangerLevel lvl = security.assess_command(cmd);
    if (lvl == DangerLevel::High || lvl == DangerLevel::Blocked)
      return "危险命令已拦截（安全策略）: " + cmd;
  }

  // 3) 审批（写类 / 执行类）——与 interactive 解耦：
  //    --no-approval 显式放行；交互模式弹询问；非交互且无审批通道默认拒绝。
  //    审批 target 按工具类型取"前缀粒度"（s = 本会话按前缀放行）：
  //    - exec_command → "exec:<命令>"（如 exec:git，允许后本会话所有 git 子命令）
  //    - 路径类写工具 → "<工具名>:"（允许后本会话该工具任意工作区路径）
  if (def.needs_approval && !approval.auto_approve_all()) {
    if (!interactive)
      return "非交互模式无审批通道，已拒绝执行工具: " + def.name +
             "（如需放行请用 --no-approval 启动）";
    std::string detail = args.value("path", args.value("command", ""));
    std::string target = def.name;
    if (def.name == "exec_command") {
      target = "exec:" + args.value("command", "");
    } else if (!detail.empty()) {
      target += ":";
    }
    std::string title = def.name + ": " + detail;
    if (!approval.request(title, ApprovalScope::Tool, target))
      return "用户拒绝执行";
  }
  return std::nullopt;
}

}  // namespace da
