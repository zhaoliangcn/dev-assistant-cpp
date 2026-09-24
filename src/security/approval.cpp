#include "security/approval.hpp"

#include <cstdio>
#include <cstring>

namespace da {

namespace {
// 边界字符：前缀匹配必须落在边界上（防 "exec:ls" 命中 "exec:lsof"）
bool boundary(char c) { return c == ' ' || c == '\t' || c == '/' || c == ':'; }
}  // namespace

void ApprovalManager::grant(ApprovalScope scope, const std::string& pattern,
                            std::chrono::seconds ttl) {
  grants_.push_back(
      {scope, pattern, std::chrono::steady_clock::now() + ttl});
}

bool ApprovalManager::has_grant(ApprovalScope scope,
                                const std::string& target) const {
  auto now = std::chrono::steady_clock::now();
  for (const auto& g : grants_) {
    if (g.scope != scope) continue;
    if (g.expires_at < now) continue;  // 已过期
    if (g.pattern == "*") return true;
    if (target.rfind(g.pattern, 0) != 0) continue;
    // 边界感知前缀（S3）：完全相等，或 pattern 以边界符结尾（如目录前缀
    // "write_file:sub/"），或 target 在前缀后的字符是边界符
    if (target.size() == g.pattern.size()) return true;
    if (boundary(g.pattern.back()) || boundary(target[g.pattern.size()]))
      return true;
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
      grant(scope, pattern);  // Session 8 小时（S3：细粒度 target）
      return true;
    default:
      return false;
  }
}

}  // namespace da
