#include "prompt.hpp"

#include <unistd.h>

#include <cstdio>
#include <string>

namespace da {

// 环境信息（对应 Rust env_info.rs 的最小子集）
static std::string env_info() {
  char buf[4096];
  std::string cwd = ::getcwd(buf, sizeof buf) ? buf : "?";
  std::string out = "cwd: " + cwd;
  if (FILE* f = ::popen("uname -srm 2>/dev/null", "r")) {
    char b[256];
    std::string sys;
    while (fgets(b, sizeof b, f)) sys += b;
    ::pclose(f);
    if (!sys.empty() && sys.back() == '\n') sys.pop_back();
    out += " | " + sys;
  }
  return out;
}

std::string build_system_prompt(const SkillRegistry* skills) {
  std::string p =
      "你是 dev-assistant，一个代码库级 AI 编程助手。\n"
      "运行环境: " + env_info() + "\n"
      "请用用户使用的语言回答；遵循所在项目的代码风格；"
      "需要执行操作时调用提供的工具，而不是只给建议。\n";
  if (skills) p += skills->render_prompt_section();
  return p;
}

}  // namespace da
