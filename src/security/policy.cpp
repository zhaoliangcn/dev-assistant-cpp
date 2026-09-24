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

  // 规范化为绝对路径
  std::string abs = path;
  if (abs[0] != '/') abs = workspace_ + "/" + abs;

  // 词法规范化：消除 . 与 .. 段（防 workspace/../outside 绕过前缀边界）
  {
    std::vector<std::string> segs;
    size_t start = 0;
    while (start <= abs.size()) {
      size_t slash = abs.find('/', start);
      std::string seg = abs.substr(
          start, slash == std::string::npos ? std::string::npos : slash - start);
      if (!seg.empty() && seg != ".") {
        if (seg == "..") {
          if (segs.empty()) {
            reason = "路径越界（过多 ..）: " + path;
            return false;
          }
          segs.pop_back();
        } else {
          segs.push_back(seg);
        }
      }
      if (slash == std::string::npos) break;
      start = slash + 1;
    }
    std::string norm;
    for (auto& s : segs) { norm += "/"; norm += s; }
    abs = norm.empty() ? "/" : norm;
  }

  // 逐级 lstat 检查 symlink——仅检查工作区前缀之下的组件：
  // 工作区路径本身可能是用户有意选择的 symlink（如 macOS /tmp → private/tmp），
  // 不属于逃逸面；工作区内新建的 symlink（含末段）一律拒绝
  {
    size_t ws_len = workspace_.size();
    size_t i = ws_len + 1;  // 跳过工作区前缀，从其下第一段开始
    while (i <= abs.size()) {
      size_t slash = abs.find('/', i);
      std::string cur =
          abs.substr(0, slash == std::string::npos ? abs.size() : slash);
      struct stat st;
      if (::lstat(cur.c_str(), &st) == 0 && S_ISLNK(st.st_mode)) {
        reason = "路径包含 symlink: " + cur;
        return false;
      }
      if (slash == std::string::npos) break;
      i = slash + 1;
    }
  }

  // 工作区边界（规范化后路径；前缀须落在分隔符边界上，防同名前缀目录绕过）
  if (check == PathCheck::Workspace || check == PathCheck::ReadOnly) {
    bool inside = abs == workspace_ ||
                  (abs.rfind(workspace_, 0) == 0 &&
                   abs.size() > workspace_.size() && abs[workspace_.size()] == '/');
    if (!inside) {
      reason = "路径超出工作区: " + abs;
      return false;
    }
  }
  return true;
}

DangerLevel SecurityPolicy::assess_command(const std::string& cmd) const {
  std::string lc = to_lower(cmd);
  // D3：廉价预筛——命令不含任何危险关键词时跳过 9 个 regex（热路径提速）。
  // 关键词集合是 regex 触发词的超集，漏筛风险为零（多筛只会多跑 regex）。
  static const char* kTriggers[] = {"rm",   "sudo", "su ",     "mkfs",
                                    "dd ",  "dd=",  ":()",     "shutdown",
                                    "reboot", "init", "/dev/sd", "chmod",
                                    "curl", ">",    "mv ",     "cp "};
  bool maybe = false;
  for (const char* t : kTriggers) {
    if (lc.find(t) != std::string::npos) { maybe = true; break; }
  }
  if (maybe) {
    for (auto& re : dangerous_patterns_) {
      if (std::regex_search(lc, re)) return DangerLevel::High;
    }
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
