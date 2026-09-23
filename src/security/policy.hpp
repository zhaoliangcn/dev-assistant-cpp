#pragma once
// 安全策略（对应 Rust security/policy.rs）
#include <regex>
#include <string>
#include <vector>

namespace da {

enum class DangerLevel { Safe, Low, Medium, High, Blocked };

enum class PathCheck {
  None,        // 不校验
  Workspace,   // 限制在工作区内
  ReadOnly,    // 仅读路径
};

class SecurityPolicy {
public:
  SecurityPolicy();

  void set_workspace(const std::string& ws) { workspace_ = ws; }
  const std::string& workspace() const { return workspace_; }

  // 路径校验：规范化 + symlink 逐级检查 + 工作区边界
  bool validate_path(const std::string& path, PathCheck check,
                     std::string& reason) const;

  // 命令危险级别评估（rm -rf / sudo / mkfs 等）
  DangerLevel assess_command(const std::string& cmd) const;

  // 敏感文件：.env / *.key / *.pem / id_rsa 等
  bool is_sensitive_file(const std::string& path) const;

private:
  std::string workspace_;
  std::vector<std::regex> dangerous_patterns_;
  std::vector<std::string> sensitive_names_;
};

}  // namespace da
