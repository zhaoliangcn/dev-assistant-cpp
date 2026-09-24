// 安全策略单测：路径规范化/symlink/边界 + 危险命令评估 + 审批门（P0 回归）
#include "test_common.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>

#include <nlohmann/json.hpp>

#include "agent/agent.hpp"
#include "security/approval.hpp"
#include "security/policy.hpp"

using namespace da;

static const char* kBase = "/tmp/da-sec-test";

static void setup_env() {
  ::system("rm -rf /tmp/da-sec-test && mkdir -p /tmp/da-sec-test/ws/sub");
  // 末段 symlink：ws/link -> /etc/passwd；中间段 symlink：ws/ldir/file
  ::symlink("/etc/passwd", "/tmp/da-sec-test/ws/link");
  ::symlink("/etc", "/tmp/da-sec-test/ws/ldir");
}

static void cleanup_env() { ::system("rm -rf /tmp/da-sec-test"); }

int test_security() {
  setup_env();

  // ---- validate_path：.. 规范化 ----
  {
    SecurityPolicy p;
    p.set_workspace(std::string(kBase) + "/ws");
    std::string reason;
    EXPECT(!p.validate_path("../outside.txt", PathCheck::Workspace, reason));
    EXPECT(!p.validate_path("sub/../../x.txt", PathCheck::Workspace, reason));
    EXPECT(!p.validate_path("/tmp/da-sec-test/ws/../outside.txt",
                            PathCheck::Workspace, reason));
    // 规范化后仍在工作区内 → 通过
    EXPECT(p.validate_path("sub/../file.txt", PathCheck::Workspace, reason));
    EXPECT(p.validate_path("./sub/file.txt", PathCheck::Workspace, reason));
  }

  // ---- validate_path：symlink（含末段）----
  {
    SecurityPolicy p;
    p.set_workspace(std::string(kBase) + "/ws");
    std::string reason;
    // 末段 symlink（S5 原漏洞：此前放行）
    EXPECT(!p.validate_path("link", PathCheck::Workspace, reason));
    // 中间段 symlink
    EXPECT(!p.validate_path("ldir/passwd", PathCheck::Workspace, reason));
  }

  // ---- validate_path：工作区边界（含同名前缀目录）----
  {
    SecurityPolicy p;
    p.set_workspace(std::string(kBase) + "/ws");
    std::string reason;
    EXPECT(!p.validate_path("/etc/passwd", PathCheck::Workspace, reason));
    // 同名前缀目录 /tmp/da-sec-test/ws2 不属于工作区（S5 边界绕过）
    EXPECT(!p.validate_path("/tmp/da-sec-test/ws2/file",
                            PathCheck::Workspace, reason));
    // 工作区内绝对路径 → 通过
    EXPECT(p.validate_path(std::string(kBase) + "/ws/sub/a.txt",
                           PathCheck::Workspace, reason));
  }

  // ---- assess_command：危险命令分级 ----
  {
    SecurityPolicy p;
    EXPECT(p.assess_command("rm -rf /") == DangerLevel::High);
    EXPECT(p.assess_command("rm -fr /tmp/x") == DangerLevel::High);
    EXPECT(p.assess_command("sudo cat /etc/shadow") == DangerLevel::High);
    EXPECT(p.assess_command("mkfs.ext4 /dev/sda1") == DangerLevel::High);
    EXPECT(p.assess_command("shutdown now") == DangerLevel::High);
    EXPECT(p.assess_command("ls -la") == DangerLevel::Safe);
    EXPECT(p.assess_command("echo hello") == DangerLevel::Safe);
    EXPECT(p.assess_command("echo x > out.txt") == DangerLevel::Medium);
  }

  // ---- ApprovalManager：auto_all 门 + grant 语义 ----
  {
    ApprovalManager strict(false);
    EXPECT(!strict.auto_approve_all());
    EXPECT(!strict.has_grant(ApprovalScope::Tool, "exec_command"));

    ApprovalManager lax(true);
    EXPECT(lax.auto_approve_all());
    // auto_all 下 request 不读 stdin 直接放行
    EXPECT(lax.request("exec_command: ls", ApprovalScope::Tool,
                       "exec_command"));
  }

  // ---- tool_gate：S1 非交互拒绝 / --no-approval 放行 / S2 拆分绕过 ----
  {
    SecurityPolicy p;
    p.set_workspace(std::string(kBase) + "/ws");
    ApprovalManager strict(false);
    ApprovalManager lax(true);

    ToolDefinition exec_def;
    exec_def.name = "exec_command";
    exec_def.needs_approval = true;

    nlohmann::json safe_args{{"command", "echo"}, {"args", {"hi"}}};
    nlohmann::json split_args{{"command", "rm"}, {"args", {"-rf", "/tmp/x"}}};
    nlohmann::json plain_args{{"command", "rm -rf /tmp/x"}};

    // S1：非交互 + 严格审批 → 拒绝（Web 模式无审批通道）
    EXPECT(tool_gate(exec_def, safe_args, p, strict, false).has_value());
    // S1：非交互 + --no-approval → 放行
    EXPECT(!tool_gate(exec_def, safe_args, p, lax, false).has_value());
    // S2：完整命令行危险 → 即使 --no-approval 也硬拦
    EXPECT(tool_gate(exec_def, plain_args, p, lax, false).has_value());
    // S2：command/args 拆分绕过（"rm" + ["-rf", …]）→ 硬拦
    EXPECT(tool_gate(exec_def, split_args, p, lax, false).has_value());

    // 写类工具（write_file）：非交互严格 → 拒绝；--no-approval → 放行
    ToolDefinition write_def;
    write_def.name = "write_file";
    write_def.needs_approval = true;
    write_def.path_check = PathCheck::Workspace;
    nlohmann::json wargs{{"path", "ok.txt"}, {"content", "x"}};
    EXPECT(tool_gate(write_def, wargs, p, strict, false).has_value());
    EXPECT(!tool_gate(write_def, wargs, p, lax, false).has_value());
    // 路径越界：即使 --no-approval 也拒绝（路径校验独立于审批）
    nlohmann::json evil{{"path", "/etc/passwd"}, {"content", "x"}};
    EXPECT(tool_gate(write_def, evil, p, lax, false).has_value());
  }

  cleanup_env();
  return g_stats.failed;
}
