#include "security/policy.hpp"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

namespace da {

static std::string to_lower(const std::string& s) {
  std::string out = s;
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return out;
}

// 取文件名部分
static std::string basename_of(const std::string& p) {
  size_t pos = p.find_last_of('/');
  return pos == std::string::npos ? p : p.substr(pos + 1);
}

SecurityPolicy::SecurityPolicy() {
  char buf[PATH_MAX];
  if (::getcwd(buf, sizeof buf)) workspace_ = buf;

  dangerous_patterns_ = {
      std::regex(R"(rm\s+(-[a-zA-Z]*r[a-zA-Z]*f|-[a-zA-Z]*f[a-zA-Z]*r))"),
      std::regex(R"(\bsudo\b)"),   std::regex(R"(\bsu\s+)"),
      std::regex(R"(\bmkfs\b)"),   std::regex(R"(\bdd\s+if=of=/dev/)"),
      std::regex(R"(:\(\)\s*\{)"), // fork bomb 形态
      std::regex(R"(\bshutdown\b|\breboot\b|\binit\s+0\b)"),
      std::regex(R"(>\s*/dev/sd)"),
      std::regex(R"(\bchmod\s+(-R\s+)?777\s+/)"),
      std::regex(R"(\bcurl\b[^|]*\|\s*(ba)?sh)"),
  };
  sensitive_names_ = {".env", "id_rsa", "id_ed25519", ".netrc", ".npmrc",
                      ".aws", ".ssh"};
}

bool SecurityPolicy::validate_path(const std::string& path, PathCheck check,
                                   std::string& reason) const {
  if (check == PathCheck::None) return true;
  if (path.empty()) { reason = "空路径"; return false; }

  // 敏感文件拦截（写类操作时由调用方决定；这里统一提示）
  // 规范化为绝对路径
  std::string abs = path;
  if (abs[0] != '/') abs = workspace_ + "/" + abs;

  // 逐级 lstat 检查 symlink（O_NOFOLLOW 等效）
  std::string cur;
  size_t i = 1;
  while (i <= abs.size()) {
    size_t slash = abs.find('/', i);
    std::string seg = abs.substr(1, (slash == std::string::npos ? abs.size() : slash) - 1);
    cur = "/" + seg;
    struct stat st;
    if (::lstat(cur.c_str(), &st) == 0 && S_ISLNK(st.st_mode)) {
      // 最终组件允许是 symlink 的场景极少，统一拒绝工作区内跳转
      if (slash != std::string::npos) {
        reason = "路径包含 symlink: " + cur;
        return false;
      }
    }
    if (slash == std::string::npos) break;
    i = slash + 1;
  }

  // 工作区边界
  if (check == PathCheck::Workspace || check == PathCheck::ReadOnly) {
    if (abs.rfind(workspace_, 0) != 0) {
      reason = "路径超出工作区: " + abs;
      return false;
    }
  }
  return true;
}

DangerLevel SecurityPolicy::assess_command(const std::string& cmd) const {
  std::string lc = to_lower(cmd);
  for (auto& re : dangerous_patterns_) {
    if (std::regex_search(lc, re)) return DangerLevel::High;
  }
  // 写文件命令视为 Medium
  if (lc.find(">") != std::string::npos || lc.find("mv ") != std::string::npos ||
      lc.find("cp ") != std::string::npos)
    return DangerLevel::Medium;
  return DangerLevel::Safe;
}

bool SecurityPolicy::is_sensitive_file(const std::string& path) const {
  std::string name = basename_of(path);
  std::string lname = to_lower(name);
  for (auto& s : sensitive_names_)
    if (lname == s || lname.rfind(s + "/", 0) == 0) return true;
  const char* suffixes[] = {".pem", ".key", ".p12", ".pfx", ".keystore"};
  for (auto* suf : suffixes) {
    size_t n = strlen(suf);
    if (lname.size() >= n && lname.compare(lname.size() - n, n, suf) == 0)
      return true;
  }
  return false;
}

}  // namespace da
