#pragma once
// 工具系统核心（对应 Rust tools/registry.rs）
#include <functional>
#include <map>
#include <string>

#include <nlohmann/json.hpp>

#include "security/policy.hpp"

namespace da {

struct ToolResult {
  bool ok = true;
  std::string output;
};

// 每次工具调用的上下文（安全策略 + 审批由 Agent 层注入）
struct ToolContext {
  SecurityPolicy* security = nullptr;
  std::string workspace;
};

using ToolHandler =
    std::function<ToolResult(const nlohmann::json& args, ToolContext& ctx)>;

struct ToolDefinition {
  std::string name;
  std::string description;
  nlohmann::json parameters;  // JSON Schema
  PathCheck path_check = PathCheck::None;
  bool needs_approval = false;  // 写类/执行类工具需要审批
  ToolHandler handler;
};

class ToolRegistry {
public:
  void register_tool(ToolDefinition def);
  bool has(const std::string& name) const;
  const ToolDefinition* find(const std::string& name) const;

  // 生成 OpenAI tools 数组 JSON
  nlohmann::json to_openai_schema() const;

  // 分发调用（不做安全检查——由 Agent 层先校验）
  ToolResult dispatch(const std::string& name, const nlohmann::json& args,
                      ToolContext& ctx) const;

  std::vector<std::string> names() const;

private:
  std::map<std::string, ToolDefinition> tools_;
};

// 注册全部内置工具
void register_builtin_tools(ToolRegistry& reg);

}  // namespace da
