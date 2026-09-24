#pragma once
// 审批管理（对应 Rust security/approval.cpp）
// Auto / OneTime / Session 三种模式，带过期与范围
#include <chrono>
#include <string>
#include <vector>

namespace da {

enum class ApprovalMode { Auto, OneTime, Session };
enum class ApprovalScope { Command, Path, Tool };

struct Grant {
  ApprovalScope scope;
  std::string pattern;  // 命令前缀 / 路径 / 工具名
  std::chrono::steady_clock::time_point expires_at;  // Session 模式有效期
};

class ApprovalManager {
public:
  explicit ApprovalManager(bool auto_approve_all = false)
      : auto_all_(auto_approve_all) {}

  // --no-approval 模式（全部放行）；供非交互路径判断是否有审批通道
  bool auto_approve_all() const { return auto_all_; }

  // 记录 Session 级授权（S3：pattern 为细粒度 target，如 "exec:ls" /
  // "write_file:sub/"；供 request('s') 与测试使用）
  void grant(ApprovalScope scope, const std::string& pattern,
             std::chrono::seconds ttl = std::chrono::hours(8));

  // 询问用户审批；OneTime 通过即返回 true，Session 通过则记录授权
  // 返回：true 放行，false 拒绝
  bool request(const std::string& title, ApprovalScope scope,
               const std::string& pattern);

  // 已有授权检查（Session 模式）
  bool has_grant(ApprovalScope scope, const std::string& target) const;

  void clear_session_grants() { grants_.clear(); }

private:
  bool auto_all_;
  std::vector<Grant> grants_;  // Session 级授权
};

}  // namespace da
