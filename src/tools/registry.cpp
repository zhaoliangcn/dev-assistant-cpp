#include "tools/registry.hpp"

#include "tools/file_tools.hpp"
#include "tools/kb.hpp"
#include "tools/system_tools.hpp"

namespace da {

namespace {

// Agent 主循环直接处理的工具：注册仅为让模型看到 schema，
// 分发入口会被 Agent::execute_tool 提前拦截
ToolResult agent_only(const nlohmann::json&, ToolContext&) {
  return {false, "该工具由 Agent 主循环直接处理"};
}

void register_agent_tools(ToolRegistry& reg) {
  {
    ToolDefinition t;
    t.name = "finish";
    t.description =
        "结束当前任务并提交最终结论。summary 说明已完成内容、验证结果与"
        "遗留风险；调用后本轮立即结束，不再请求模型。";
    t.parameters = {
        {"type", "object"},
        {"properties",
         {{"summary",
           {{"type", "string"}, {"description", "最终结论/交付摘要"}}}}},
        {"required", {"summary"}}};
    t.handler = agent_only;
    reg.register_tool(std::move(t));
  }
  {
    ToolDefinition t;
    t.name = "context_budget";
    t.description = "查看当前上下文占用：token 估算、压缩阈值与消息条数。";
    t.parameters = {{"type", "object"}, {"properties", nlohmann::json::object()}};
    t.handler = agent_only;
    reg.register_tool(std::move(t));
  }
  {
    ToolDefinition t;
    t.name = "compress_context";
    t.description =
        "立即压缩上下文：把早期消息折叠为摘要，保留 system 与最近若干条。";
    t.parameters = {{"type", "object"}, {"properties", nlohmann::json::object()}};
    t.handler = agent_only;
    reg.register_tool(std::move(t));
  }
  {
    ToolDefinition t;
    t.name = "save_summary";
    t.description =
        "把关键结论保存到会话摘要（落盘到会话日志），供后续恢复时参考。";
    t.parameters = {{"type", "object"},
                    {"properties",
                     {{"summary",
                       {{"type", "string"}, {"description", "要保留的结论"}}}}},
                    {"required", {"summary"}}};
    t.handler = agent_only;
    reg.register_tool(std::move(t));
  }
}

}  // namespace

void ToolRegistry::register_tool(ToolDefinition def) {
  tools_[def.name] = std::move(def);
}

bool ToolRegistry::has(const std::string& name) const {
  return tools_.count(name) > 0;
}

const ToolDefinition* ToolRegistry::find(const std::string& name) const {
  auto it = tools_.find(name);
  return it == tools_.end() ? nullptr : &it->second;
}

nlohmann::json ToolRegistry::to_openai_schema() const {
  nlohmann::json arr = nlohmann::json::array();
  for (const auto& kv : tools_) {
    const auto& t = kv.second;
    // 参数规范化：required 恒为数组。
    // （注意：初始化列表里的空 {} 会被序列化为 null 而非 []，严格端点会拒绝）
    nlohmann::json params = t.parameters;
    if (!params.is_object()) params = nlohmann::json::object();
    if (!params.contains("required") || !params["required"].is_array())
      params["required"] = nlohmann::json::array();
    arr.push_back({
        {"type", "function"},
        {"function",
         {{"name", t.name},
          {"description", t.description},
          {"parameters", params}}},
    });
  }
  return arr;
}

ToolResult ToolRegistry::dispatch(const std::string& name,
                                  const nlohmann::json& args,
                                  ToolContext& ctx) const {
  const ToolDefinition* def = find(name);
  if (!def) return {false, "未知工具: " + name};
  try {
    return def->handler(args, ctx);
  } catch (const std::exception& e) {
    return {false, std::string("工具异常: ") + e.what()};
  }
}

std::vector<std::string> ToolRegistry::names() const {
  std::vector<std::string> out;
  for (const auto& kv : tools_) out.push_back(kv.first);
  return out;
}

void register_builtin_tools(ToolRegistry& reg) {
  register_file_tools(reg);
  register_system_tools(reg);
  register_kb_tools(reg);
  register_agent_tools(reg);
}

}  // namespace da
