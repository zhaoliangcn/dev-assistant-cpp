#include "app.hpp"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <ctime>

#include "agent/agent.hpp"
#include "config/config.hpp"
#include "persist/journal.hpp"
#include "tools/file_tools.hpp"
#include "tools/registry.hpp"
#include "tools/subagent.hpp"
#include "tools/system_tools.hpp"

namespace da {

static std::string session_dir_today() {
  std::time_t t = std::time(nullptr);
  char buf[64];
  std::strftime(buf, sizeof buf, "%Y%m%d-%H%M%S", std::localtime(&t));
  return std::string(".dev-assistant/sessions/") + buf;
}

bool App::load_config(const std::string& path) {
  load_dotenv();
  AppConfig cfg;
  if (!cfg.load(path.empty() ? ".dev-assistant-models.toml" : path)) return false;
  llm_.set_config(cfg);
  return true;
}

bool App::init(const std::string& config_path) {
  if (!load_config(config_path)) return false;
  register_builtin_tools(tools_);
  register_subagent_tool(tools_, nullptr);  // runner 由 Agent 层特殊处理
  approval_ = ApprovalManager(no_approval_);

  char buf[4096];
  security_.set_workspace(
      ::getcwd(buf, sizeof buf) ? std::string(buf) : std::string("."));

  // skills 扫描（工作目录与可执行目录都试）
  if (skills_.scan("skills") == 0) skills_.scan(".");

  // dream 记忆库
  ::mkdir(".dev-assistant", 0700);
  dream_.load_all();

  // hooks 配置（可选文件）
  hooks_.load_config(".dev-assistant/hooks.toml");

  // session 日志目录
  last_session_dir_ = session_dir_today();
  ::mkdir(".dev-assistant", 0700);
  std::string sd = last_session_dir_;
  ::mkdir(sd.substr(0, sd.find_last_of('/')).c_str(), 0700);
  ::mkdir(sd.c_str(), 0700);

  // session_start 钩子
  std::string out;
  hooks_.fire(HookEvent::SessionStart, security_.workspace(), out);
  return true;
}

int App::process_message(const std::string& input) {
  // before_tool_call 之类的事件在 Agent 工具执行处触发（简化：
  // 此处在会话级触发 before/after 由 Agent 内部完成，这里只管主循环）
  Agent agent(llm_, tools_, security_, approval_);
  int rc = agent.run(input, true);

  // 对话事件落盘（JSONL 0600）
  Journal j;
  if (j.open(last_session_dir_ + "/events.jsonl")) {
    j.append("user_message", R"("role":"user")");
    j.close();
  }
  return rc;
}

void App::finish_session() {
  std::string out;
  hooks_.fire(HookEvent::SessionEnd, last_session_dir_, out);

  // dream.ingest：把本次会话经验写入记忆库（简化：无 LLM 提炼时
  // 记录会话摘要条目，带 session 标签）
  if (!last_session_dir_.empty()) {
    dream_.ingest("会话 " + last_session_dir_,
                  "会话日志见 " + last_session_dir_ + "/events.jsonl",
                  {"session"});
    dream_.save_all();
  }
}

int App::resume_last_session() {
  // 找 .dev-assistant/sessions/ 下最新目录
  std::string base = ".dev-assistant/sessions";
  DIR* d = ::opendir(base.c_str());
  if (!d) return 0;
  std::string latest;
  struct dirent* e;
  while ((e = ::readdir(d)) != nullptr) {
    std::string name = e->d_name;
    if (name == "." || name == "..") continue;
    if (latest.empty() || name > latest) latest = name;  // 时间戳目录名字典序=时间序
  }
  ::closedir(d);
  if (latest.empty()) return 0;

  std::string path = base + "/" + latest + "/events.jsonl";
  FILE* f = std::fopen(path.c_str(), "r");
  if (!f) return 0;
  int count = 0;
  char buf[8192];
  // 逐行读 JSONL；本实现的日志格式 {"ts":...,"type":...}，
  // resume 恢复的是"有会话发生过"这一事实 + 提示用户；
  // 完整消息级恢复需要 persist 层记录完整 payload，此处恢复为可继续对话状态
  while (std::fgets(buf, sizeof buf, f)) count++;
  std::fclose(f);
  last_session_dir_ = base + "/" + latest;
  return count;
}

}  // namespace da
