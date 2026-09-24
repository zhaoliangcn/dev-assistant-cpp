#include "hooks/hooks.hpp"

#include <sys/wait.h>
#include <unistd.h>

#include <signal.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <thread>

#include "config/toml.hpp"

namespace da {

namespace {

// fork-exec 经 /bin/sh -c 执行；捕获 stdout+stderr；超时 kill
struct ExecOutcome {
  int exit_code = -1;
  std::string output;
  bool timed_out = false;
};

ExecOutcome run_shell(const std::string& cmd, int timeout_sec,
                      const std::string& payload) {
  ExecOutcome out;
  int pipefd[2];
  if (::pipe(pipefd) != 0) return out;

  pid_t pid = ::fork();
  if (pid < 0) {
    ::close(pipefd[0]);
    ::close(pipefd[1]);
    return out;
  }
  if (pid == 0) {
    ::setpgid(0, 0);
    ::close(pipefd[0]);
    ::dup2(pipefd[1], STDOUT_FILENO);
    ::dup2(pipefd[1], STDERR_FILENO);
    ::close(pipefd[1]);
    ::setenv("DA_HOOK_PAYLOAD", payload.c_str(), 1);
    ::execl("/bin/sh", "sh", "-c", cmd.c_str(), (char*)nullptr);
    ::_exit(127);
  }
  ::setpgid(pid, pid);
  ::close(pipefd[1]);

  fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
  auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(timeout_sec);
  char buf[4096];
  while (true) {
    ssize_t n = ::read(pipefd[0], buf, sizeof buf);
    if (n > 0) {
      out.output.append(buf, n);
      if (out.output.size() > 128 * 1024) out.output.resize(128 * 1024);
    } else if (n == 0) {
      break;
    } else {
      if (errno != EAGAIN && errno != EINTR) break;
      if (std::chrono::steady_clock::now() > deadline) {
        out.timed_out = true;
        ::kill(-pid, SIGKILL);
        // 排空后退出
        while ((n = ::read(pipefd[0], buf, sizeof buf)) > 0)
          out.output.append(buf, n);
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      int status;
      if (::waitpid(pid, &status, WNOHANG) == pid) {
        while ((n = ::read(pipefd[0], buf, sizeof buf)) > 0)
          out.output.append(buf, n);
        out.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        ::close(pipefd[0]);
        return out;
      }
    }
  }
  ::close(pipefd[0]);
  int status = 0;
  ::waitpid(pid, &status, 0);
  out.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  return out;
}

}  // namespace

void HookManager::add_hook(HookConfig h) { hooks_.push_back(std::move(h)); }

bool HookManager::fire(HookEvent event, const std::string& payload,
                       std::string& output) const {
  if (!enabled_) return true;  // --no-hooks：全部钩子不执行
  bool allowed = true;
  for (const auto& h : hooks_) {
    if (h.event != event) continue;
    if (dry_run_) {  // --hooks-dry-run：只打印，不执行
      output += std::string("[dry-run] ") + hook_event_name(event) + " <- " +
                h.name + ": " + h.command + "\n";
      continue;
    }
    ExecOutcome r = run_shell(h.command, h.timeout_sec, payload);
    if (!r.output.empty()) {
      output += r.output;
      if (output.back() != '\n') output += '\n';
    }
    if (h.blocking && !r.timed_out && r.exit_code != 0) allowed = false;
  }
  return allowed;
}

bool HookManager::load_config(const std::string& toml_path) {
  FILE* f = std::fopen(toml_path.c_str(), "rb");
  if (!f) return false;
  std::string text;
  char buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
  std::fclose(f);

  TomlTable root;
  std::map<std::string, TomlTable> tables;
  if (!Toml::parse(text, root, tables)) return false;

  for (const auto& kv : tables) {
    if (kv.first.rfind("hooks.", 0) != 0) continue;
    const TomlTable& t = kv.second;
    HookConfig h;
    h.name = kv.first.substr(6);
    auto gs = [&](const char* k) {
      auto it = t.find(k);
      return it == t.end() ? std::string() : it->second.as_string();
    };
    std::string ev = gs("event");
    if (ev == "session_start") h.event = HookEvent::SessionStart;
    else if (ev == "before_tool_call") h.event = HookEvent::BeforeToolCall;
    else if (ev == "after_tool_call") h.event = HookEvent::AfterToolCall;
    else if (ev == "session_end") h.event = HookEvent::SessionEnd;
    else continue;
    h.command = gs("command");
    if (h.command.empty()) continue;
    auto ti = t.find("timeout");
    if (ti != t.end() && ti->second.type == TomlValue::Type::Int)
      h.timeout_sec = (int)ti->second.i;
    h.blocking = gs("blocking") == "true";
    hooks_.push_back(std::move(h));
  }
  return true;
}

}  // namespace da
