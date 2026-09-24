#include "repl.hpp"

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <regex>
#include <string>
#include <vector>

#include "scheduler/scheduler.hpp"

namespace da {

namespace {
// fork+execvp 执行（不经 shell，防注入）；捕获 stdout，stderr 可选并入/静默。
// 返回 {exit_code, output}
std::pair<int, std::string> run_child_capture(
    const std::vector<std::string>& argv, bool quiet_stderr) {
  std::string out;
  int pipefd[2];
  if (::pipe(pipefd) != 0) return {-1, "(pipe 创建失败)"};
  std::fflush(stdout);
  pid_t pid = ::fork();
  if (pid < 0) {
    ::close(pipefd[0]);
    ::close(pipefd[1]);
    return {-1, "(fork 失败)"};
  }
  if (pid == 0) {
    ::close(pipefd[0]);
    ::dup2(pipefd[1], STDOUT_FILENO);
    if (quiet_stderr) {
      int devnull = ::open("/dev/null", O_WRONLY);
      if (devnull >= 0) {
        ::dup2(devnull, STDERR_FILENO);
        ::close(devnull);
      }
    } else {
      ::dup2(pipefd[1], STDERR_FILENO);
    }
    ::close(pipefd[1]);
    std::vector<char*> cargv;
    cargv.reserve(argv.size() + 1);
    for (auto& a : argv) cargv.push_back(const_cast<char*>(a.c_str()));
    cargv.push_back(nullptr);
    ::execvp(cargv[0], cargv.data());
    ::_exit(127);  // exec 失败
  }
  ::close(pipefd[1]);
  char buf[4096];
  ssize_t n;
  while ((n = ::read(pipefd[0], buf, sizeof buf)) > 0) {
    if (out.size() < 256 * 1024) out.append(buf, n);
  }
  ::close(pipefd[0]);
  int status = 0;
  ::waitpid(pid, &status, 0);
  return {WIFEXITED(status) ? WEXITSTATUS(status) : -1, out};
}
}  // namespace

// /grep：正则搜文件（对齐 Rust run_grep：递归文本文件、跳过大目录、上限每文件 5 条）
static void run_grep_cmd(const std::string& pattern) {
  if (pattern.empty()) {
    std::printf("🔍 用法: /grep <正则>\n");
    return;
  }
  std::regex re;
  try {
    re = std::regex(pattern);
  } catch (const std::regex_error&) {
    std::printf("❌ 无效的正则表达式: %s\n", pattern.c_str());
    return;
  }
  // fork+execvp（不经 shell）：pattern 作为单个 argv 传递，无注入面
  auto [rc, out] = run_child_capture(
      {"grep", "-rnE", "--exclude-dir=.git", "--exclude-dir=build",
       "--exclude-dir=build-asan", "--exclude-dir=build-musl",
       "--exclude-dir=node_modules", "--exclude-dir=.dev-assistant", "-I",
       "-m", "5", "--", pattern, "."},
      /*quiet_stderr=*/true);
  if (rc != 0 || out.empty())
    std::printf("🔍 未找到匹配 \"%s\" 的内容\n", pattern.c_str());
  else
    std::fputs(out.c_str(), stdout);
}

// /diff：git diff 查看工作区改动（可选路径参数，按空白分词作 argv 传递）
static void run_diff_cmd(const std::string& args) {
  std::vector<std::string> argv{"git", "diff", "--"};
  size_t i = 0;
  while (i < args.size()) {
    while (i < args.size() && (args[i] == ' ' || args[i] == '\t')) i++;
    size_t b = i;
    while (i < args.size() && args[i] != ' ' && args[i] != '\t') i++;
    if (i > b) argv.push_back(args.substr(b, i - b));
  }
  auto [rc, out] = run_child_capture(argv, /*quiet_stderr=*/true);
  if (rc != 0) {
    std::printf("❌ git diff 执行失败（非 git 仓库或参数错误）\n");
    return;
  }
  // 去尾空白后判断是否有改动
  std::string trimmed = out;
  while (!trimmed.empty() && (trimmed.back() == '\n' || trimmed.back() == ' '))
    trimmed.pop_back();
  if (trimmed.empty()) {
    std::printf("ℹ️ 工作区没有未提交的改动\n");
    return;
  }
  std::fputs(out.c_str(), stdout);
}

static bool handle_slash(const std::string& line, App& app) {
  if (line == "/exit" || line == "/quit") return true;
  // /history：查看最近一轮对话历史（对齐 Rust：角色图标 + 120 字预览）
  if (line == "/history") {
    const auto& msgs = app.last_history();
    if (msgs.empty()) {
      std::printf("📋 暂无对话历史\n");
      return false;
    }
    std::printf("📋 对话历史（共 %zu 条）:\n", msgs.size());
    for (size_t i = 0; i < msgs.size(); i++) {
      const char* icon = msgs[i].role == "system"  ? "⚙ 系统"
                         : msgs[i].role == "user"  ? "👤 用户"
                         : msgs[i].role == "assistant" ? "🤖 助手"
                         : msgs[i].role == "tool"  ? "🔧 工具"
                                                   : "📝 未知";
      std::string c = msgs[i].content;
      if (c.size() > 120) {
        c.resize(120);
        std::printf("  #%zu %s: %s...\n", i + 1, icon, c.c_str());
      } else {
        std::printf("  #%zu %s: %s\n", i + 1, icon, c.c_str());
      }
    }
    return false;
  }
  // /grep <正则>：正则搜文件
  if (line.rfind("/grep ", 0) == 0 || line == "/grep" ||
      line.rfind("/search", 0) == 0) {
    run_grep_cmd(line.find(' ') == std::string::npos ? "" : line.substr(line.find(' ') + 1));
    return false;
  }
  // /diff [路径...]：git diff 查看工作区改动
  if (line == "/diff" || line.rfind("/diff ", 0) == 0) {
    run_diff_cmd(line == "/diff" ? "" : line.substr(6));
    return false;
  }
  // /budget：上下文预算面板（对齐 Rust /budget：用量、进度条、压力等级）
  if (line == "/budget") {
    auto r = app.budget_report();
    const char* pressure = r.pressure == 3 ? "🔴 爆满"
                           : r.pressure == 2 ? "🟠 临界"
                           : r.pressure == 1 ? "🟡 提示"
                                             : "🟢 正常";
    int bar_n = (int)(r.utilization * 20);
    if (bar_n < 0) bar_n = 0;
    if (bar_n > 20) bar_n = 20;
    std::string bar;
    for (int i = 0; i < 20; i++) bar += i < bar_n ? "█" : "░";
    std::printf("📊 上下文预算:\n");
    std::printf("  系统: %ld tok\n", r.system_tokens);
    std::printf("  历史: %ld tok\n", r.history_tokens);
    std::printf("  工具: %ld tok\n", r.tool_schema_tokens);
    std::printf("  合计: %ld / %ld tok（%.1f%%）\n", r.total_tokens,
                r.max_tokens, r.utilization * 100);
    std::printf("  [%s] %s\n", bar.c_str(), pressure);
    std::printf("  剩余可用: %ld tok\n", r.estimated_room);
    return false;
  }
  // /dream [--dry-run]：记忆整理（对齐 Rust /dream：dry_run 预览不落盘）
  if (line == "/dream" || line.rfind("/dream ", 0) == 0) {
    bool dry_run = line.find("--dry-run") != std::string::npos;
    auto& dream = app.dream();
    auto rep = dream.report();
    std::printf("🧠 Dream 记忆整理%s开始...（当前 %d 条）\n",
                dry_run ? "（预演模式）" : "", rep.total);
    if (rep.total == 0) {
      std::printf("ℹ️ 记忆库为空，无需整理\n");
      return false;
    }
    int dedup_n = dream.dedup(dry_run ? 0.6 : 0.6);
    int forget_n = dream.forget(90, 0);
    if (dry_run) {
      // 预演：把变更当作“预览”展示（dedup/forget 已改内存，不 save_all 落盘）
      std::printf("  预览: 将去重合并 %d 条、遗忘 %d 条（--dry-run 不落盘）\n",
                  dedup_n, forget_n);
      std::printf("  实际执行请去掉 --dry-run\n");
      // 重新加载以撤销内存变更
      dream.load_all();
    } else {
      dream.save_all();
      std::printf("✅ 整理完成: 去重合并 %d 条、遗忘 %d 条\n", dedup_n, forget_n);
    }
    auto after = dream.report();
    std::printf("  记忆库: %d → %d 条（累计使用 %d 次）\n", rep.total,
                after.total, after.total_uses);
    return false;
  }
  // /schedule cron <表达式> agent <指令>：创建定时任务（对齐 Rust）
  if (line.rfind("/schedule ", 0) == 0) {
    std::string rest = line.substr(10);
    if (rest.rfind("cron ", 0) != 0) {
      std::printf("用法: /schedule cron <cron表达式> agent <指令>\n");
      return false;
    }
    std::string rest2 = rest.substr(5);
    size_t ag = rest2.find("agent ");
    if (ag == std::string::npos) {
      std::printf("用法: /schedule cron <cron表达式> agent <指令>\n");
      return false;
    }
    std::string cron_expr = rest2.substr(0, ag);
    while (!cron_expr.empty() && cron_expr.back() == ' ') cron_expr.pop_back();
    std::string prompt = rest2.substr(ag + 6);
    CronSpec spec;
    if (cron_expr.empty() || prompt.empty() || !CronSpec::parse(cron_expr, spec)) {
      std::printf("❌ 无效任务（cron 表达式或指令为空/不合法）\n");
      return false;
    }
    ScheduledTask t;
    t.id = "task-" + std::to_string(std::time(nullptr));
    t.name = prompt.substr(0, prompt.find(' ') == std::string::npos
                                   ? prompt.size()
                                   : prompt.find(' '));
    t.cron = cron_expr;
    t.prompt = prompt;
    auto& sch = app.scheduler();
    sch.add_task(t);
    sch.save(".dev-assistant/scheduled_tasks.jsonl");
    std::printf("✅ 已创建定时任务 %s（cron: %s）\n", t.id.c_str(),
                t.cron.c_str());
    return false;
  }
  // /unschedule <任务ID>：取消定时任务
  if (line.rfind("/unschedule ", 0) == 0) {
    std::string id = line.substr(12);
    auto& sch = app.scheduler();
    if (sch.remove_task(id)) {
      sch.save(".dev-assistant/scheduled_tasks.jsonl");
      std::printf("✅ 已取消任务: %s\n", id.c_str());
    } else {
      std::printf("❌ 未找到任务: %s\n", id.c_str());
    }
    return false;
  }
  // /scheduled | /tasks：列出全部定时任务
  if (line == "/scheduled" || line == "/tasks") {
    auto ts = app.scheduler().tasks();
    if (ts.empty()) {
      std::printf("ℹ️ 暂无定时任务\n");
      return false;
    }
    std::printf("⏰ 定时任务（共 %zu 个）:\n", ts.size());
    for (const auto& t : ts) {
      std::string when = t.cron.empty()
                             ? "一次性 @" + std::to_string(t.run_at)
                             : "cron " + t.cron;
      std::printf("  %s [%s] %s — %s\n", t.id.c_str(),
                  t.enabled ? "启用" : "停用", when.c_str(),
                  t.prompt.c_str());
    }
    return false;
  }
  // /pipeline <任务>：六阶段流水线（对齐 Rust /pipeline）
  if (line.rfind("/pipeline ", 0) == 0) {
    std::string objective = line.substr(10);
    if (objective.empty()) {
      std::printf("用法: /pipeline <任务描述>\n");
      return false;
    }
    std::printf("🚀 流水线开始（设计→实现→审查→测试→修复→记录）...\n");
    bool ok = app.run_pipeline(objective);
    std::printf("%s\n", ok ? "✅ 流水线全部通过" : "❌ 流水线存在未通过阶段");
    return false;
  }
  if (line == "/status") {
    auto& m = app.llm().config().current_model();
    std::printf("模型: %s\nAPI: %s\n", m.model.c_str(), m.api_url.c_str());
    return false;
  }
  // /model：对齐 Rust 版 — 无参数列出全部模型并按编号切换；带参数按名称切换
  if (line == "/model" || line.rfind("/model ", 0) == 0) {
    const auto& cfg = app.llm().config();
    if (cfg.models.empty()) {
      std::printf("ℹ️ 当前没有可用的模型配置\n");
      return false;
    }
    if (line == "/model") {
      std::printf("📦 可用模型（当前: %s）:\n", cfg.current_model().name.c_str());
      for (size_t i = 0; i < cfg.models.size(); i++)
        std::printf("  %zu. %s %s\n", i + 1,
                    i == (size_t)cfg.current ? "👉" : "  ",
                    cfg.models[i].name.c_str());
      std::fputs("  输入编号切换（直接回车取消）: ", stdout);
      std::fflush(stdout);
      std::string choice;
      if (!std::getline(std::cin, choice) || choice.empty()) {  // EOF 或空回车
        std::printf("ℹ️ 已取消切换\n");
        return false;
      }
      int idx = -1;
      try {
        size_t pos = 0;
        idx = std::stoi(choice, &pos);
        if (pos != choice.size()) idx = -1;  // 非纯数字
      } catch (...) {
        idx = -1;
      }
      if (idx < 1 || (size_t)idx > cfg.models.size()) {
        std::printf("❌ 无效编号: %s\n", choice.c_str());
        return false;
      }
      const std::string& target = cfg.models[idx - 1].name;
      std::printf(app.llm().switch_model(target) ? "✅ 已切换到模型: %s\n"
                                                 : "❌ 切换失败: %s\n",
                  target.c_str());
      return false;
    }
    std::string target = line.substr(7);
    std::printf(app.llm().switch_model(target) ? "✅ 已切换到模型: %s\n"
                                               : "❌ 未找到模型: %s（/model 查看模型列表）\n",
                target.c_str());
    return false;
  }
  if (line == "/skills") {
    int n = 0;
    for (const auto& s : app.skills().skills()) {
      std::printf("- %s: %s\n", s.title.c_str(), s.description.c_str());
      n++;
    }
    if (n == 0) std::printf("(无已加载技能)\n");
    return false;
  }
  if (line == "/memory") {
    int n = 0;
    for (const auto& m : app.dream().memories()) {
      std::printf("- [%s] %s (uses=%d)\n", m.id.c_str(), m.title.c_str(),
                  m.use_count);
      n++;
    }
    if (n == 0) std::printf("(记忆库为空)\n");
    return false;
  }
  if (line == "/help") {
    std::printf("命令: /exit /quit /status /model [name] /skills /memory"
                " /history /diff [路径] /grep|/search <正则> /budget"
                " /dream [--dry-run] /schedule cron <expr> agent <cmd>"
                " /unschedule <id> /scheduled\n");
    return false;
  }
  std::printf("未知命令: %s（/help 查看命令）\n", line.c_str());
  return false;
}

int run_repl(App& app) {
  std::printf("dev-assistant-cpp 交互模式（/help 查看命令，/exit 退出）\n");
  std::string line;
  while (true) {
    std::fputs("> ", stdout);
    std::fflush(stdout);
    if (!std::getline(std::cin, line)) break;
    if (line.empty()) continue;
    if (line[0] == '/') {
      if (handle_slash(line, app)) break;
      continue;
    }
    app.process_message(line);
  }
  app.finish_session();
  app.stop_scheduler();
  return 0;
}

}  // namespace da
