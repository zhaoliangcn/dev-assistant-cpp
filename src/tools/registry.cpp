#include "tools/registry.hpp"

#include "tools/file_tools.hpp"
#include "tools/system_tools.hpp"

namespace da {

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
}

}  // namespace da
