// 提示词与工具/流水线单测：结构分区、身份注入、后段目标去重、P1 工具
#include "test_common.hpp"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "agent/compressor.hpp"
#include "agent/pipeline.hpp"
#include "prompt.hpp"
#include "security/policy.hpp"
#include "tools/file_tools.hpp"
#include "tools/kb.hpp"
#include "tools/registry.hpp"
#include "utils/sha256.hpp"

namespace {

bool has(const std::string& s, const char* key) {
  return s.find(key) != std::string::npos;
}

}  // namespace

int test_prompt_pipeline() {
  // 1) 主提示词包含关键分区
  const std::string p = da::build_system_prompt(nullptr);
  EXPECT(has(p, "## 铁律（永远遵守）"));
  EXPECT(has(p, "## 安全策略"));
  EXPECT(has(p, "## 工作流程"));
  EXPECT(has(p, "## 何时结束本轮"));
  EXPECT(has(p, "## 何时提问"));
  EXPECT(has(p, "## 错误处理"));
  EXPECT(has(p, "## Token 与上下文管理"));
  EXPECT(has(p, "## 记忆沉淀"));
  EXPECT(has(p, "## 执行循环"));
  EXPECT(has(p, "## 工作方式"));
  EXPECT(has(p, "## 工具使用要点"));
  EXPECT(has(p, "## 输出规范"));
  EXPECT(has(p, "## 运行环境"));
  // 提示词引用的工具均已注册（finish/批读/预算工具为 P1 新增）
  EXPECT(has(p, "read_symbol"));
  EXPECT(has(p, "spawn_subagent"));
  EXPECT(has(p, "batch_read_files"));
  EXPECT(has(p, "finish"));
  EXPECT(has(p, "context_budget"));
  // General 身份不注入子代理段
  EXPECT(!has(p, "## 子代理执行规则"));

  // 2) 身份注入
  const std::string impl =
      da::build_system_prompt(nullptr, da::Identity::Implementer);
  EXPECT(has(impl, "【实现者】"));
  EXPECT(has(impl, "## 子代理执行规则"));
  EXPECT(has(impl, "完成条件"));
  const std::string arch =
      da::build_system_prompt(nullptr, da::Identity::Architect);
  EXPECT(has(arch, "【架构师】"));

  // 3) 流水线：目标只在需要验收标准的阶段出现
  std::vector<std::string> prompts;
  std::vector<da::Identity> roles;
  da::Pipeline pipeline(0);  // 关闭回退，固定 6 阶段
  auto runner = [&](const std::string& prompt, da::Identity role,
                    const std::vector<da::StageOutput>&) -> da::ToolResult {
    prompts.push_back(prompt);
    roles.push_back(role);
    return {true, "PASS"};
  };
  std::vector<da::StageOutput> out;
  EXPECT(pipeline.run("重构登录模块", runner, out));
  EXPECT_EQ(prompts.size(), static_cast<size_t>(6));
  // architect/implementer/tester 需要目标
  EXPECT(has(prompts[0], "目标: 重构登录模块"));
  EXPECT(has(prompts[1], "目标: 重构登录模块"));
  EXPECT(has(prompts[3], "目标: 重构登录模块"));
  // reviewer/repair/document 不重复目标
  EXPECT(!has(prompts[2], "目标: 重构登录模块"));
  EXPECT(!has(prompts[4], "目标: 重构登录模块"));
  EXPECT(!has(prompts[5], "目标: 重构登录模块"));
  // 阶段身份映射
  EXPECT(roles[0] == da::Identity::Architect);
  EXPECT(roles[1] == da::Identity::Implementer);
  EXPECT(roles[2] == da::Identity::Reviewer);
  EXPECT(roles[3] == da::Identity::Tester);
  EXPECT(roles[4] == da::Identity::Debugger);
  EXPECT(roles[5] == da::Identity::Reviewer);
  return 0;
}

// P1：新增工具全部注册，finish 的 schema 必须要求 summary
int test_agent_tools_registered() {
  da::ToolRegistry reg;
  da::register_builtin_tools(reg);
  EXPECT(reg.has("finish"));
  EXPECT(reg.has("batch_read_files"));
  EXPECT(reg.has("context_budget"));
  EXPECT(reg.has("compress_context"));
  EXPECT(reg.has("save_summary"));
  EXPECT(reg.has("kb_store"));
  EXPECT(reg.has("kb_query"));

  nlohmann::json schema = reg.to_openai_schema();
  bool finish_ok = false;
  for (const auto& f : schema) {
    if (f["function"]["name"] != "finish") continue;
    const auto& req = f["function"]["parameters"]["required"];
    finish_ok = req.is_array() && !req.empty();
  }
  EXPECT(finish_ok);
  return 0;
}

// P1：batch_read_files 多文件读取，单文件失败不影响其他文件
int test_batch_read_files() {
  std::system(
      "mkdir -p /tmp/da-batch-test && printf 'alpha\\nbeta\\n' > "
      "/tmp/da-batch-test/a.txt && printf 'gamma\\ndelta\\n' > "
      "/tmp/da-batch-test/b.txt");
  da::ToolRegistry reg;
  da::register_file_tools(reg);
  da::SecurityPolicy policy;
  policy.set_workspace("/tmp/da-batch-test");
  da::ToolContext ctx{&policy, "/tmp/da-batch-test"};

  nlohmann::json args;
  args["files"] = nlohmann::json::array(
      {"a.txt",
       nlohmann::json{{"path", "b.txt"}, {"offset", 0}, {"limit", 1}},
       nlohmann::json{{"path", "missing.txt"}}});
  da::ToolResult r = reg.dispatch("batch_read_files", args, ctx);
  EXPECT(r.ok);  // 至少一个文件成功即整体成功
  EXPECT(has(r.output, "alpha"));
  EXPECT(has(r.output, "gamma"));
  EXPECT(!has(r.output, "delta"));  // b.txt 仅取 1 行
  EXPECT(has(r.output, "missing.txt"));
  EXPECT(has(r.output, "无法读取文件"));

  // 参数缺失：files 必须为数组
  da::ToolResult bad =
      reg.dispatch("batch_read_files", nlohmann::json::object(), ctx);
  EXPECT(!bad.ok);
  return 0;
}

// P1：上下文预算——force_compress 可无条件压缩，阈值可读
int test_compressor_force() {
  da::Compressor c(10, 2);
  std::vector<da::ChatMessage> h;
  h.push_back({"system", "sys", "", {}});
  for (int i = 0; i < 8; i++)
    h.push_back({"user", std::string(40, 'x'), "", {}});
  EXPECT(da::Compressor::history_tokens(h) > 10);
  EXPECT(c.force_compress(h));
  EXPECT_EQ(h.size(), static_cast<size_t>(4));  // system + 摘要 + 最近 2 条
  EXPECT_EQ(c.threshold(), static_cast<size_t>(10));
  return 0;
}

// P2：kb_store 写入条目 + index.json，kb_query 按关键词/类型/标签检索
int test_kb_tools() {
  std::system("rm -rf /tmp/da-kb-test && mkdir -p /tmp/da-kb-test");
  da::ToolRegistry reg;
  da::register_kb_tools(reg);
  da::SecurityPolicy policy;
  policy.set_workspace("/tmp/da-kb-test");
  da::ToolContext ctx{&policy, "/tmp/da-kb-test"};

  // 写入：path 带 .kb/ 前缀也应被规范化
  nlohmann::json store;
  store["path"] = ".kb/decisions/ADR-001.md";
  store["content"] =
      "---\nid: ADR-001\ntype: decision\ntitle: 使用 ECS 架构\n"
      "tags: [architecture, ecs]\nstatus: accepted\n---\n# ECS\n\nBody.";
  da::ToolResult r = reg.dispatch("kb_store", store, ctx);
  EXPECT(r.ok);
  EXPECT(has(r.output, "ADR-001"));

  // 索引已生成且包含条目
  std::string index;
  {
    std::ifstream f("/tmp/da-kb-test/.kb/index.json");
    std::ostringstream ss;
    ss << f.rdbuf();
    index = ss.str();
  }
  EXPECT(has(index, "ADR-001"));
  EXPECT(has(index, "decisions/ADR-001.md"));

  // 关键词检索（中文标题 + 英文标签均可命中）
  nlohmann::json q;
  q["query"] = "ECS";
  da::ToolResult qr = reg.dispatch("kb_query", q, ctx);
  EXPECT(qr.ok);
  EXPECT(has(qr.output, "ADR-001"));
  EXPECT(has(qr.output, "使用 ECS 架构"));

  // include_content 返回正文
  q["include_content"] = true;
  EXPECT(has(reg.dispatch("kb_query", q, ctx).output, "Body."));

  // 类型过滤：interface 不应命中 decision
  nlohmann::json q_type;
  q_type["type"] = "interface";
  EXPECT(!has(reg.dispatch("kb_query", q_type, ctx).output, "ADR-001"));

  // 标签过滤
  nlohmann::json q_tag;
  q_tag["tags"] = nlohmann::json::array({"ecs"});
  EXPECT(has(reg.dispatch("kb_query", q_tag, ctx).output, "ADR-001"));

  // 路径遍历必须被拒绝
  nlohmann::json bad;
  bad["path"] = "../evil.md";
  bad["content"] = "x";
  EXPECT(!reg.dispatch("kb_store", bad, ctx).ok);

  // 非 .md 路径必须被拒绝
  nlohmann::json bad2;
  bad2["path"] = "decisions/a.txt";
  bad2["content"] = "x";
  EXPECT(!reg.dispatch("kb_store", bad2, ctx).ok);
  return 0;
}

// P3：sha256 已知向量 + source_refs 过期检测 + query-stats
int test_kb_source_refs() {
  EXPECT(da::sha256_hex("") ==
         "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  EXPECT(da::sha256_hex("abc") ==
         "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

  std::system(
      "rm -rf /tmp/da-kb-ref && mkdir -p /tmp/da-kb-ref/src && "
      "printf 'int main() {}\\n' > /tmp/da-kb-ref/src/app.cpp");
  da::ToolRegistry reg;
  da::register_kb_tools(reg);
  da::SecurityPolicy policy;
  policy.set_workspace("/tmp/da-kb-ref");
  da::ToolContext ctx{&policy, "/tmp/da-kb-ref"};

  nlohmann::json store;
  store["path"] = "decisions/D-1.md";
  store["content"] =
      "---\nid: D-1\ntype: decision\ntitle: 入口约定\n"
      "tags: [entry]\nstatus: accepted\nsource_paths: src/app.cpp\n---\n# 入口\n";
  EXPECT(reg.dispatch("kb_store", store, ctx).ok);

  // 源文件未变更：无过期引用
  nlohmann::json q;
  q["query"] = "入口";
  da::ToolResult fresh = reg.dispatch("kb_query", q, ctx);
  EXPECT(fresh.ok);
  EXPECT(!has(fresh.output, "过期引用"));
  // query-stats 已记录
  std::string stats;
  {
    std::ifstream f("/tmp/da-kb-ref/.kb/query-stats.json");
    std::ostringstream ss;
    ss << f.rdbuf();
    stats = ss.str();
  }
  EXPECT(has(stats, "D-1"));
  EXPECT(has(stats, "query_count"));

  // 修改源文件后：报告过期引用
  std::ofstream("/tmp/da-kb-ref/src/app.cpp") << "int main() { return 0; }\n";
  da::ToolResult stale = reg.dispatch("kb_query", q, ctx);
  EXPECT(has(stale.output, "过期引用: 1"));
  return 0;
}
