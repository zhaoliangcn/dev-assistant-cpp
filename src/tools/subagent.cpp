#include "tools/subagent.hpp"

namespace da {

namespace {

ToolResult spawn_subagent(const nlohmann::json& args, ToolContext& ctx,
                          SubagentRunner* runner) {
  std::string prompt = args.value("prompt", "");
  std::string role = args.value("role", "general");
  int depth = args.value("depth", 1);
  if (prompt.empty()) return {false, "prompt 不能为空"};
  if (depth > kMaxSubagentDepth)
    return {false, "子代理深度超限（最大 " +
                       std::to_string(kMaxSubagentDepth) + "）"};
  if (!runner) return {false, "子代理执行器未注入"};
  return (*runner)(prompt, role, depth);
}

}  // namespace

void register_subagent_tool(ToolRegistry& reg, SubagentRunner runner) {
  auto* fn = new SubagentRunner(std::move(runner));
  ToolDefinition t;
  t.name = "spawn_subagent";
  t.description =
      "派生子代理执行独立子任务（深度≤3），返回其文本结果。"
      "role: general/architect/implementer/reviewer/tester/debugger";
  t.parameters = {
      {"type", "object"},
      {"properties",
       {{"prompt", {{"type", "string"}, {"description", "子任务描述"}}},
        {"role", {{"type", "string"}, {"description", "身份角色，默认 general"}}},
        {"depth", {{"type", "integer"}, {"description", "当前深度（父传子+1）"}}}}},
      {"required", {"prompt"}}};
  t.handler = [fn](const nlohmann::json& args, ToolContext& ctx) {
    return spawn_subagent(args, ctx, fn);
  };
  reg.register_tool(std::move(t));
}

}  // namespace da
