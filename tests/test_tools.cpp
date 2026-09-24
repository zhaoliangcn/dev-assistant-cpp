// 工具 schema 规范化回归测试：required 恒为数组（agnes 严格校验曾报 400）
#include "test_common.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <fstream>
#include <sstream>

#include "security/policy.hpp"
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

static void test_edit_file_replace_all() {
  // B8 回归：replace_all 且 new_s 长度 ≠ old_s 长度时，后续匹配不错位/不漏检。
  // （核实结论：原实现 pos += new_s.size() 语义正确——replace 后插入文本占据
  //   [pos, pos+new_s.size())，从其后续找恰好跳过刚替换的内容）
  ToolRegistry reg;
  register_file_tools(reg);
  ::system("rm -rf /tmp/da-edit-test && mkdir -p /tmp/da-edit-test");
  da::SecurityPolicy p;
  p.set_workspace("/tmp/da-edit-test");
  da::ToolContext ctx{&p, "/tmp/da-edit-test"};
  std::string path = "/tmp/da-edit-test/t.txt";
  {
    std::ofstream f(path);
    f << "aXaXa";
  }
  json args{{"path", path},
            {"old_string", "a"},
            {"new_string", "bb"},
            {"replace_all", true}};
  auto r = reg.dispatch("edit_file", args, ctx);
  EXPECT(r.ok);
  std::ifstream f(path);
  std::stringstream ss;
  ss << f.rdbuf();
  // 3 处全部替换：若 pos 计算错误会出现 "bbbbXbb" 之类的错位/漏检
  EXPECT_EQ(ss.str(), std::string("bbXbbXbb"));

  // 删除场景（new_s 为空串）也不死循环、不漏删
  json del{{"path", path}, {"old_string", "bb"}, {"new_string", ""}, {"replace_all", true}};
  r = reg.dispatch("edit_file", del, ctx);
  EXPECT(r.ok);
  std::ifstream f2(path);
  std::stringstream ss2;
  ss2 << f2.rdbuf();
  EXPECT_EQ(ss2.str(), std::string("XX"));  // bbXbbXbb → 删除全部 "bb" → XX
  ::system("rm -rf /tmp/da-edit-test");
}

int test_tools() {
  test_schema_required_empty_list();
  test_schema_required_missing();
  test_schema_full_registry();
  test_edit_file_replace_all();
  return g_stats.failed;
}
