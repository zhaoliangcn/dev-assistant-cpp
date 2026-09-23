#include "security/approval.hpp"

#include <cstdio>
#include <cstring>

namespace da {

bool ApprovalManager::has_grant(ApprovalScope scope,
                                const std::string& target) const {
  auto now = std::chrono::steady_clock::now();
  for (const auto& g : grants_) {
    if (g.scope != scope) continue;
    if (g.expires_at < now) continue;  // 已过期
    if (g.pattern == "*" || target.rfind(g.pattern, 0) == 0) return true;
  }
  return false;
}

bool ApprovalManager::request(const std::string& title, ApprovalScope scope,
                              const std::string& pattern) {
  if (auto_all_) return true;  // --no-approval：放行（审计由调用方落盘）
  if (has_grant(scope, pattern)) return true;

  std::printf("\n⚠️  需要审批: %s\n[y] 本次允许  [s] 本会话允许  [n] 拒绝 > ",
              title.c_str());
  std::fflush(stdout);
  char buf[16] = {0};
  if (!std::fgets(buf, sizeof buf, stdin)) return false;
  switch (buf[0]) {
    case 'y':
    case 'Y':
      return true;  // OneTime
    case 's':
    case 'S':
      grants_.push_back({scope, pattern,
                         std::chrono::steady_clock::now() +
                             std::chrono::hours(8)});  // Session 8 小时
      return true;
    default:
      return false;
  }
}

}  // namespace da
