// 工具 schema 规范化回归测试：required 恒为数组（agnes 严格校验曾报 400）
#include "test_common.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "security/policy.hpp"
#include "tools/file_tools.hpp"
#include "tools/registry.hpp"
#include "utils/utf8.hpp"
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

static void test_read_symbol_braces_in_strings() {
  // C1 回归：字符串/注释中的 { } 不再干扰函数边界判定
  ToolRegistry reg;
  register_file_tools(reg);
  ::system("rm -rf /tmp/da-c1-test && mkdir -p /tmp/da-c1-test");
  da::SecurityPolicy p;
  p.set_workspace("/tmp/da-c1-test");
  da::ToolContext ctx{&p, "/tmp/da-c1-test"};
  std::string path = "/tmp/da-c1-test/demo.cpp";
  {
    std::ofstream f(path);
    f << "std::string demo() {\n"
      << "  std::string s = \"brace } in string {\";\n"
      << "  /* comment with } brace { */\n"
      << "  // line comment }\n"
      << "  return s + \"}\";\n"
      << "}\n"
      << "std::string next_fn() { return \"next\"; }\n";
  }
  json args{{"path", path}, {"symbol", "demo"}};
  auto r = reg.dispatch("read_symbol", args, ctx);
  EXPECT(r.ok);
  // 正确边界：demo() 定义应止于第 6 行 "}"，不吞掉 next_fn
  EXPECT(r.output.find("next_fn") == std::string::npos);
  EXPECT(r.output.find("return s") != std::string::npos);
  ::system("rm -rf /tmp/da-c1-test");
}

static void test_read_file_size_cap() {
  // C8 regression: the oversized-file guard must reject, and it must measure the
  // same fd it later reads (fstat), never a separately stat()-ed path.
  ToolRegistry reg;
  register_file_tools(reg);
  ::system("rm -rf /tmp/da-cap-test && mkdir -p /tmp/da-cap-test");
  da::SecurityPolicy p;
  p.set_workspace("/tmp/da-cap-test");
  da::ToolContext ctx{&p, "/tmp/da-cap-test"};

  // small file: must still be readable
  const std::string small = "/tmp/da-cap-test/small.txt";
  {
    std::ofstream f(small);
    f << "hello";
  }
  json a1{{"path", small}};
  auto r1 = reg.dispatch("read_file", a1, ctx);
  EXPECT(r1.ok);
  EXPECT(r1.output.find("hello") != std::string::npos);

  // 11MB > 10MB cap: both readers must refuse
  const std::string big = "/tmp/da-cap-test/big.txt";
  {
    std::ofstream f(big, std::ios::binary);
    const std::string chunk(1024 * 1024, 'x');
    for (int i = 0; i < 11; ++i) f << chunk;
  }
  json a2{{"path", big}};
  EXPECT(!reg.dispatch("read_file", a2, ctx).ok);
  json a3{{"path", big}, {"symbol", "x"}};
  EXPECT(!reg.dispatch("read_symbol", a3, ctx).ok);

  // missing file: clean failure, no crash
  json a4{{"path", "/tmp/da-cap-test/nope.txt"}};
  EXPECT(!reg.dispatch("read_file", a4, ctx).ok);

  ::system("rm -rf /tmp/da-cap-test");
}

static long long ms_since(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now() - t0).count();
}

static void test_exec_command_timeout_and_truncation() {
  ToolRegistry reg;
  register_system_tools(reg);
  ::system("rm -rf /tmp/da-exec-test && mkdir -p /tmp/da-exec-test");
  da::SecurityPolicy p;
  p.set_workspace("/tmp/da-exec-test");
  da::ToolContext ctx{&p, "/tmp/da-exec-test"};

  // baseline: a normal command succeeds and its output is captured
  {
    json a{{"command", "sh"}, {"args", json::array({"-c", "echo hello"})}};
    auto r = reg.dispatch("exec_command", a, ctx);
    EXPECT(r.ok);
    EXPECT(r.output.find("hello") != std::string::npos);
  }

  // a silent command must hit the timeout promptly
  {
    auto t0 = std::chrono::steady_clock::now();
    json a{{"command", "sh"}, {"args", json::array({"-c", "sleep 30"})},
           {"timeout", 1}};
    auto r = reg.dispatch("exec_command", a, ctx);
    EXPECT(!r.ok);
    EXPECT(ms_since(t0) < 10000);
  }

  // REGRESSION: a command that keeps emitting output used to run forever,
  // because the deadline was only tested on the EAGAIN path.
  {
    auto t0 = std::chrono::steady_clock::now();
    json a{{"command", "sh"},
           {"args", json::array({"-c",
                                 "i=0; while [ $i -lt 400 ]; do echo tick; "
                                 "sleep 0.05; i=$((i+1)); done"})},
           {"timeout", 1}};
    auto r = reg.dispatch("exec_command", a, ctx);
    EXPECT(!r.ok);
    EXPECT(ms_since(t0) < 10000);
  }

  // REGRESSION: blowing past the 256KB output cap used to break out of the
  // read loop and then block in waitpid(), bypassing the timeout entirely.
  {
    auto t0 = std::chrono::steady_clock::now();
    json a{{"command", "sh"},
           {"args", json::array({"-c",
                                 "dd if=/dev/zero bs=1024 count=1024 2>/dev/null; sleep 30"})},
           {"timeout", 2}};
    auto r = reg.dispatch("exec_command", a, ctx);
    EXPECT(!r.ok);
    EXPECT(r.output.find("truncated") != std::string::npos);
    EXPECT(ms_since(t0) < 15000);
  }

  ::system("rm -rf /tmp/da-exec-test");
}

static void test_non_utf8_file_does_not_crash() {
  // Regression: nlohmann dump() throws type_error.316 on strings that are not
  // valid UTF-8. Tools return raw file bytes, so a latin-1 file made the journal
  // dump throw; nothing caught it and the process died with std::terminate.
  ToolRegistry reg;
  register_file_tools(reg);
  ::system("rm -rf /tmp/da-u8 && mkdir -p /tmp/da-u8");
  da::SecurityPolicy p;
  p.set_workspace("/tmp/da-u8");
  da::ToolContext ctx{&p, "/tmp/da-u8"};

  const std::string f = "/tmp/da-u8/latin1.txt";
  {
    FILE* fp = std::fopen(f.c_str(), "wb");
    if (!fp) { EXPECT(false); return; }
    // 0xE8 is latin-1 "e-acute": invalid as a standalone UTF-8 lead byte.
    const char raw[] = "hello NEEDLE caf\xe8 latin1 end\n";
    std::fwrite(raw, 1, sizeof(raw) - 1, fp);
    std::fclose(fp);
  }
  {
    FILE* fp = std::fopen(f.c_str(), "rb");
    EXPECT(fp != nullptr);
    if (fp) std::fclose(fp);
  }

  // read_file must return something the JSON layer can serialize.
  json a1{{"path", f}};
  auto r1 = reg.dispatch("read_file", a1, ctx);
  EXPECT(r1.ok);
  {
    nlohmann::json probe = nlohmann::json{{"t", r1.output}};
    EXPECT(!probe.dump().empty());   // must not throw
  }

  // grep over the directory must still work, not abort the whole scan.
  json a2{{"pattern", "NEEDLE"}};
  auto r2 = reg.dispatch("grep", a2, ctx);
  EXPECT(r2.ok);
  {
    nlohmann::json probe = nlohmann::json{{"t", r2.output}};
    EXPECT(!probe.dump().empty());
  }

  // sanitize_utf8 itself: replacement char appears, valid UTF-8 passes through.
  const std::string bad = "a\xe8z";
  const std::string fixed = da::sanitize_utf8(bad);
  EXPECT(fixed.find("\xEF\xBF\xBD") != std::string::npos);
  EXPECT(fixed.size() == bad.size() + 2);
  EXPECT(da::sanitize_utf8("plain ascii") == "plain ascii");
  EXPECT(da::sanitize_utf8("\xe4\xb8\xad\xe6\x96\x87") == "\xe4\xb8\xad\xe6\x96\x87");

  ::system("rm -rf /tmp/da-u8");
}

int test_tools() {
  test_schema_required_empty_list();
  test_schema_required_missing();
  test_schema_full_registry();
  test_edit_file_replace_all();
  test_read_symbol_braces_in_strings();
  test_read_file_size_cap();
  test_exec_command_timeout_and_truncation();
  test_non_utf8_file_does_not_crash();
  return g_stats.failed;
}
