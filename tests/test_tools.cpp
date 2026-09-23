// 工具 schema 规范化回归测试：required 恒为数组（agnes 严格校验曾报 400）
#include "test_common.hpp"

#include "tools/file_tools.hpp"
#include "tools/registry.hpp"
#include "tools/subagent.hpp"
#include "tools/system_tools.hpp"

using da::ToolDefinition;
using da::ToolRegistry;
using json = nlohmann::json;

static void test_schema_required_empty_list() {
  // 复现 list_dir 的写法：初始化列表空 {}（曾被序列化为 null 而非 []）
  ToolRegistry reg;
  ToolDefinition t;
  t.name = "list_dir";
  t.description = "列出目录内容";
  t.parameters = {{"type", "object"},
                  {"properties", {{"path", {{"type", "string"}}}}},
                  {"required", {}}};
  t.handler = [](const json&, da::ToolContext&) {
    return da::ToolResult{true, ""};
  };
  reg.register_tool(std::move(t));

  json schema = reg.to_openai_schema();
  EXPECT(schema.is_array());
  EXPECT_EQ(schema.size(), 1u);
  auto& params = schema[0]["function"]["parameters"];
  EXPECT(params["required"].is_array());
  EXPECT_EQ(params["required"].size(), 0u);
}

static void test_schema_required_missing() {
  // 工具未声明 required 键：规范化后补为空数组
  ToolRegistry reg;
  ToolDefinition t;
  t.name = "no_required";
  t.description = "无必填参数";
  t.parameters = {{"type", "object"},
                  {"properties", {{"opt", {{"type", "string"}}}}}};
  t.handler = [](const json&, da::ToolContext&) {
    return da::ToolResult{true, ""};
  };
  reg.register_tool(std::move(t));
  json schema = reg.to_openai_schema();
  auto& params = schema[0]["function"]["parameters"];
  EXPECT(params["required"].is_array());
  EXPECT_EQ(params["required"].size(), 0u);
}

static void test_schema_full_registry() {
  // 完整工具集：全部 required 恒为数组（回归 agnes 400 场景）
  ToolRegistry reg;
  register_system_tools(reg);
  register_file_tools(reg);
  register_subagent_tool(reg, [](const std::string&, const std::string&, int) {
    return da::ToolResult{true, ""};
  });
  json schema = reg.to_openai_schema();
  int n = 0;
  for (auto& tool : schema) {
    n++;
    auto& params = tool["function"]["parameters"];
    EXPECT(params.is_object());
    EXPECT(params["required"].is_array());
  }
  // exec_command/list_dir/read_file/read_symbol/write_file/edit_file/glob/grep/spawn_subagent
  EXPECT_EQ(n, 9);
}

int test_tools() {
  test_schema_required_empty_list();
  test_schema_required_missing();
  test_schema_full_registry();
  return g_stats.failed;
}
