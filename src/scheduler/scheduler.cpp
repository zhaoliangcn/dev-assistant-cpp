#include "scheduler/scheduler.hpp"

#include <nlohmann/json.hpp>

#include <sys/wait.h>
#include <unistd.h>

#include <ctime>
#include <thread>

#include "utils/atomic_write.hpp"

namespace da {

using json = nlohmann::json;

// ---- CronSpec ----

// 解析单段：* / 数字 / 逗号列表（取列表中的每个值分别匹配由调用方展开；
// 此子集实现为：列表匹配任一值）
namespace {

struct FieldSet {
  bool any = true;
  std::vector<int> values;
};

bool parse_field(const std::string& s, int lo, int hi, FieldSet& out) {
  if (s == "*") { out.any = true; out.values.clear(); return true; }
  out.any = false;
  // 步进语法：*/n 或 a-b/n（子集支持 */n）
  std::string body = s;
  int step = 1;
  size_t slash = s.find('/');
  if (slash != std::string::npos) {
    body = s.substr(0, slash);
    try {
      size_t pos = 0;
      step = std::stoi(s.substr(slash + 1), &pos);
      if (pos != s.size() - slash - 1) return false;  // 整串消费，拒绝 "1-2/3x"
    } catch (...) {
      return false;
    }
    if (step <= 0) return false;
  }
  if (body == "*") {
    for (int v = lo; v <= hi; v += step) out.values.push_back(v);
    return !out.values.empty();
  }
  size_t start = 0;
  while (start <= body.size()) {
    size_t comma = body.find(',', start);
    std::string part = body.substr(
        start, comma == std::string::npos ? std::string::npos : comma - start);
    if (!part.empty()) {
      try {
        // 整串消费校验：拒绝 "1-5" 区间被 stoi 静默截断为 1（危险误配）
        size_t pos = 0;
        int v = std::stoi(part, &pos);
        if (pos != part.size()) return false;
        if (v < lo || v > hi) return false;
        out.values.push_back(v);
      } catch (...) {
        return false;
      }
    }
    if (comma == std::string::npos) break;
    start = comma + 1;
  }
  return !out.values.empty();
}

bool field_matches(const FieldSet& f, int v) {
  return f.any ||
         std::find(f.values.begin(), f.values.end(), v) != f.values.end();
}

}  // namespace

bool CronSpec::parse(const std::string& spec, CronSpec& out) {
  out = CronSpec{};
  std::vector<std::string> parts;
  size_t start = 0;
  while (start <= spec.size()) {
    size_t sp = spec.find(' ', start);
    std::string part = spec.substr(
        start, sp == std::string::npos ? std::string::npos : sp - start);
    // 去重复空格产生的空段
    if (!part.empty()) parts.push_back(part);
    if (sp == std::string::npos) break;
    start = sp;
    while (start < spec.size() && spec[start] == ' ') start++;
  }
  if (parts.size() != 5) return false;

  FieldSet f[5];  // C2：不再用 static（多线程隐患），每次 parse 独立
  if (!parse_field(parts[0], 0, 59, f[0])) return false;
  if (!parse_field(parts[1], 0, 23, f[1])) return false;
  if (!parse_field(parts[2], 1, 31, f[2])) return false;
  if (!parse_field(parts[3], 1, 12, f[3])) return false;
  if (!parse_field(parts[4], 0, 6, f[4])) return false;

  out.every_minute = f[0].any && f[1].any;
  // C3：全量存储允许值（列表/步进完整匹配，不再只取第一个值）；any = 空 = 不限
  if (!f[0].any) out.minute = f[0].values;
  if (!f[1].any) out.hour = f[1].values;
  if (!f[2].any) out.day = f[2].values;
  if (!f[3].any) out.month = f[3].values;
  if (!f[4].any) out.weekday = f[4].values;
  return true;
}

namespace {
bool field_allows(const std::vector<int>& vals, int v) {
  return vals.empty() ||
         std::find(vals.begin(), vals.end(), v) != vals.end();
}
}  // namespace

bool CronSpec::matches(const std::tm& t) const {
  if (!field_allows(minute, t.tm_min)) return false;
  if (!field_allows(hour, t.tm_hour)) return false;
  if (!field_allows(day, t.tm_mday)) return false;
  if (!field_allows(month, t.tm_mon + 1)) return false;
  if (!field_allows(weekday, t.tm_wday)) return false;
  return true;
}

// ---- Scheduler ----

void Scheduler::add_task(ScheduledTask t) {
  tasks_.push_back(std::move(t));
}

bool Scheduler::remove_task(const std::string& id) {
  for (auto it = tasks_.begin(); it != tasks_.end(); ++it) {
    if (it->id == id) {
      tasks_.erase(it);
      return true;
    }
  }
  return false;
}

std::vector<ScheduledTask> Scheduler::tasks() const { return tasks_; }

bool Scheduler::save(const std::string& path) const {
  json arr = json::array();
  for (const auto& t : tasks_) {
    arr.push_back({{"id", t.id},
                   {"name", t.name},
                   {"cron", t.cron},
                   {"run_at", t.run_at},
                   {"prompt", t.prompt},
                   {"enabled", t.enabled}});
  }
  auto st = atomic_write(path, arr.dump(-1, ' ', false));
  return st.ok();
}

bool Scheduler::load(const std::string& path) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  std::string text;
  char buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
  std::fclose(f);

  json arr = json::parse(text, nullptr, false);
  if (!arr.is_array()) return false;
  tasks_.clear();
  for (const auto& j : arr) {
    ScheduledTask t;
    t.id = j.value("id", "");
    t.name = j.value("name", "");
    t.cron = j.value("cron", "");
    t.run_at = j.value("run_at", 0L);
    t.prompt = j.value("prompt", "");
    t.enabled = j.value("enabled", true);
    tasks_.push_back(std::move(t));
  }
  return true;
}

int Scheduler::tick() {
  long now = (long)std::time(nullptr);
  if (last_tick_sec_ == 0) last_tick_sec_ = now;
  // 补齐跳过的秒（进程休眠后恢复场景）
  for (long sec = last_tick_sec_ + 1; sec <= now; sec++) {
    std::time_t tt = sec;
    std::tm lt;
    localtime_r(&tt, &lt);
    for (auto& t : tasks_) {
      if (!t.enabled) continue;
      if (!t.cron.empty()) {
        CronSpec spec;
        if (CronSpec::parse(t.cron, spec) && spec.matches(lt)) fire(t);
      } else if (t.run_at > 0 && sec >= t.run_at) {
        fire(t);
        t.enabled = false;  // 一次性任务触发后停用
      }
    }
  }
  last_tick_sec_ = now;
  return 0;
}

void Scheduler::fire(const ScheduledTask& t) {
  if (on_fire_) on_fire_(t);
}

void Scheduler::execute_shell(const ScheduledTask& t, const std::string& log_path) {
  // 不经 shell：prompt 按空白分词后 fork+execvp（防注入，与 exec_command
  // 工具同防线）；分词不支持引号/管道等 shell 语法——这是安全取舍
  std::vector<std::string> toks;
  {
    const std::string& s = t.prompt;
    size_t i = 0;
    while (i < s.size()) {
      while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
      size_t b = i;
      while (i < s.size() && s[i] != ' ' && s[i] != '\t') i++;
      if (i > b) toks.push_back(s.substr(b, i - b));
    }
  }

  std::string out;
  int rc = -1;
  if (toks.empty()) {
    out = "(空命令)";
  } else {
    int out_pipe[2];
    if (::pipe(out_pipe) != 0) {
      out = "(pipe 创建失败)";
    } else {
      pid_t pid = ::fork();
      if (pid < 0) {
        ::close(out_pipe[0]);
        ::close(out_pipe[1]);
        out = "(fork 失败)";
      } else if (pid == 0) {
        // 子进程：进程组 + stdout/stderr 合并到管道 + execvp
        ::setpgid(0, 0);
        ::close(out_pipe[0]);
        ::dup2(out_pipe[1], STDOUT_FILENO);
        ::dup2(out_pipe[1], STDERR_FILENO);
        ::close(out_pipe[1]);
        std::vector<char*> cargv;
        cargv.reserve(toks.size() + 1);
        for (auto& a : toks) cargv.push_back(const_cast<char*>(a.c_str()));
        cargv.push_back(nullptr);
        ::execvp(toks[0].c_str(), cargv.data());
        ::_exit(127);  // exec 失败
      }
      // 父进程：读输出（4KB 上限，超出丢弃但记截断标记）
      ::close(out_pipe[1]);
      char buf[4096];
      ssize_t n;
      bool truncated = false;
      while ((n = ::read(out_pipe[0], buf, sizeof buf)) > 0) {
        if (out.size() < 4096)
          out.append(buf, n);
        else
          truncated = true;
      }
      ::close(out_pipe[0]);
      if (truncated) out += "...(截断)";
      int status = 0;
      ::waitpid(pid, &status, 0);
      rc = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
  }
  // 执行日志（JSONL 追加）
  json entry{{"ts", (long)std::time(nullptr)},
             {"task_id", t.id},
             {"name", t.name},
             {"command", t.prompt},
             {"exit", rc},
             {"output", out}};
  std::FILE* lf = std::fopen(log_path.c_str(), "a");
  if (lf) {
    std::fputs(entry.dump(-1, ' ', false).c_str(), lf);
    std::fputc('\n', lf);
    std::fclose(lf);
  }
}

void Scheduler::run_loop() {
  running_ = true;
  while (running_) {
    tick();
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
}

}  // namespace da
