#include "app.hpp"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <ctime>

#include "agent/agent.hpp"
#include "agent/compressor.hpp"
#include "agent/pipeline.hpp"
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
  if (verbose_)
    std::fprintf(stderr, "[verbose] 模型=%s 输入=%zu 字符\n",
                 llm_.config().current_model().model.c_str(), input.size());
  // 持久 Agent（B2）：首轮创建，后续轮次复用 → 模型可见完整对话上下文
  if (!agent_) {
    agent_ = std::make_unique<Agent>(llm_, tools_, security_, approval_);
    agent_->set_context_budget((size_t)max_tokens_);  // B1：压缩阈值=上下文预算
  }
  // --resume 重建的历史（B3）：仅在首次注入
  if (!resumed_history_.empty()) {
    agent_->set_history(std::move(resumed_history_));
    resumed_history_.clear();
  }
  // B3：挂载会话日志，Agent 内真写 user/assistant/tool 事件（脱敏）
  Journal j;
  if (j.open(last_session_dir_ + "/events.jsonl")) agent_->set_journal(&j);
  int rc = agent_->run(input, true);
  agent_->set_journal(nullptr);
  j.close();
  last_history_ = agent_->history();  // 供 REPL /history 查看
  return rc;
}

App::BudgetReport App::budget_report() const {
  BudgetReport r;
  r.max_tokens = max_tokens_;
  // 历史估算（与 Compressor 同口径）
  r.history_tokens = (long)Compressor::history_tokens(last_history_);
  // 工具 schema 估算：序列化后按 token 估算
  size_t schema_bytes = 0;
  for (const auto& s : tools_.to_openai_schema())
    schema_bytes += s.dump().size();
  r.tool_schema_tokens = (long)(schema_bytes * 3 / 4);
  r.total_tokens = r.system_tokens + r.history_tokens + r.tool_schema_tokens;
  r.utilization = r.max_tokens > 0
                      ? (double)r.total_tokens / (double)r.max_tokens
                      : 0.0;
  r.estimated_room = r.max_tokens - r.total_tokens;
  if (r.estimated_room < 0) r.estimated_room = 0;
  // 压力分级（对齐 Rust ContextPressure：正常/提示/临界/爆满）
  r.pressure = r.utilization >= 1.0 ? 3
               : r.utilization >= 0.85 ? 2
               : r.utilization >= 0.70 ? 1
                                       : 0;
  return r;
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

Scheduler& App::scheduler() {
  if (!scheduler_) {
    // 到期回调：默认执行器（shell + JSONL 日志，对应 Rust executor.rs）
    std::string log_path = ".dev-assistant/scheduler.log";
    scheduler_ = std::make_unique<Scheduler>(
        [log_path](const ScheduledTask& t) {
          Scheduler::execute_shell(t, log_path);
        });
    scheduler_->load(".dev-assistant/scheduled_tasks.jsonl");
    if (!scheduler_started_) {
      scheduler_started_ = true;
      scheduler_thread_ = std::thread([this]() { scheduler_->run_loop(); });
    }
  }
  return *scheduler_;
}

void App::stop_scheduler() {
  if (scheduler_ && scheduler_->running()) {
    scheduler_->save(".dev-assistant/scheduled_tasks.jsonl");
    scheduler_->stop();
  }
  if (scheduler_thread_.joinable()) scheduler_thread_.join();
}

bool App::run_pipeline(const std::string& objective) {
  Pipeline pipeline(1);  // Reviewer 回退重跑上限 1 次
  auto runner = [this](const std::string& prompt, Identity role,
                       const std::vector<StageOutput>&) -> ToolResult {
    Agent agent(llm_, tools_, security_, approval_);
    agent.set_depth(1);  // 各阶段以子代理深度运行（受限递归）
    agent.run(prompt, true);
    // B4：返回子代理真实 assistant 文本（供 Reviewer/Tester/Repair 消费），
    // 而非占位字符串——否则流水线各阶段拿到的"上阶段产出"全是空话
    for (auto it = agent.history().rbegin(); it != agent.history().rend(); ++it) {
      if (it->role == "assistant" && !it->tool_calls.empty()) continue;
      if (it->role == "assistant" && !it->content.empty())
        return {true, it->content};
    }
    return {false, "阶段未产出文本结果"};
  };
  std::vector<StageOutput> out;
  bool ok = pipeline.run(objective, runner, out);
  for (const auto& o : out)
    std::printf("  [%s] %s\n", pipeline_stage_name(o.stage), o.ok ? "✅" : "❌");
  return ok;
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
  // B3：解析 JSONL 事件，重建对话历史（user/assistant/tool 真实内容）
  std::vector<ChatMessage> rebuilt;
  char buf[65536];
  while (std::fgets(buf, sizeof buf, f)) {
    nlohmann::json ev = nlohmann::json::parse(buf, nullptr, false);
    if (ev.is_discarded()) continue;
    std::string type = ev.value("type", "");
    if (type == "user_message") {
      rebuilt.push_back({"user", ev.value("content", ""), "", {}});
    } else if (type == "assistant_message") {
      ChatMessage m{"assistant", ev.value("content", ""), "", {}};
      if (ev.contains("tool_calls") && ev["tool_calls"].is_array()) {
        for (auto& tc : ev["tool_calls"])
          m.tool_calls.push_back({tc.value("id", ""), tc.value("name", ""),
                                  tc.value("arguments", "")});
      }
      rebuilt.push_back(std::move(m));
    } else if (type == "tool_result") {
      rebuilt.push_back({"tool", ev.value("content", ""),
                         ev.value("tool_call_id", ""), {}});
    }
  }
  std::fclose(f);
  if (rebuilt.empty()) return 0;
  // 历史须以 user/assistant 结尾且 tool 结果有主：丢弃尾部悬空 assistant
  // tool_calls（其 tool 结果可能在下一会话）——保守起见原样注入，由
  // Compressor/模型容错；仅记录条数
  resumed_history_ = std::move(rebuilt);
  last_session_dir_ = base + "/" + latest;
  return (int)resumed_history_.size();
}

}  // namespace da
