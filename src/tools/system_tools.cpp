#include "tools/system_tools.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <thread>
#include <vector>

namespace da {

namespace {

std::string arg_str(const nlohmann::json& args, const char* key,
                    const std::string& def = "") {
  if (!args.contains(key)) return def;
  const auto& v = args[key];
  if (v.is_string()) return v.get<std::string>();
  return def;
}

long arg_long(const nlohmann::json& args, const char* key, long def) {
  if (!args.contains(key)) return def;
  const auto& v = args[key];
  if (v.is_number_integer()) return v.get<long>();
  return def;
}

// exec_command：不经 shell 解释，直接 fork+exec（防注入）
// setpgid 建立进程组，超时 kill(-pgid) 整组
ToolResult exec_command_tool(const nlohmann::json& args, ToolContext& ctx) {
  std::string program = arg_str(args, "command");
  if (program.empty()) return {false, "command 不能为空"};

  std::vector<std::string> argv;
  if (args.contains("args") && args["args"].is_array()) {
    for (const auto& a : args["args"])
      if (a.is_string()) argv.push_back(a.get<std::string>());
  }

  long timeout_sec = arg_long(args, "timeout", 300);

  int out_pipe[2];
  if (::pipe(out_pipe) != 0) return {false, "pipe 创建失败"};

  pid_t pid = ::fork();
  if (pid < 0) return {false, "fork 失败"};
  if (pid == 0) {
    // 子进程：建立进程组、重定向 stdout/stderr、执行
    ::setpgid(0, 0);
    ::close(out_pipe[0]);
    ::dup2(out_pipe[1], STDOUT_FILENO);
    ::dup2(out_pipe[1], STDERR_FILENO);
    ::close(out_pipe[1]);
    std::string ws = ctx.security->workspace();
    if (!ws.empty()) ::chdir(ws.c_str());
    std::vector<char*> cargv;
    cargv.push_back(const_cast<char*>(program.c_str()));
    for (auto& a : argv) cargv.push_back(const_cast<char*>(a.c_str()));
    cargv.push_back(nullptr);
    ::execvp(program.c_str(), cargv.data());
    ::_exit(127);  // exec 失败
  }

  // 父进程
  ::setpgid(pid, pid);  // 双保险设置进程组
  ::close(out_pipe[1]);

  std::string output;
  char buf[4096];
  fcntl(out_pipe[0], F_SETFL, O_NONBLOCK);
  auto deadline = std::chrono::steady_clock::now() +
                  std::chrono::seconds(timeout_sec);
  bool timed_out = false;
  while (true) {
    ssize_t n = ::read(out_pipe[0], buf, sizeof buf);
    if (n > 0) {
      output.append(buf, n);
      if (output.size() > 256 * 1024) { output.resize(256 * 1024); break; }
    } else if (n == 0) {
      break;  // EOF
    } else {
      if (errno != EAGAIN && errno != EINTR) break;
      // 非阻塞轮询 + 超时检查
      if (std::chrono::steady_clock::now() > deadline) {
        timed_out = true;
        ::kill(-pid, SIGKILL);
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      // 检查子进程是否退出
      int status;
      pid_t r = ::waitpid(pid, &status, WNOHANG);
      if (r == pid) {
        // 排空管道
        while ((n = ::read(out_pipe[0], buf, sizeof buf)) > 0)
          output.append(buf, n);
        break;
      }
    }
  }
  ::close(out_pipe[0]);

  int status = 0;
  if (!timed_out) ::waitpid(pid, &status, 0);
  if (timed_out) {
    ::waitpid(pid, &status, 0);
    return {false, "命令超时（" + std::to_string(timeout_sec) +
                       "s）已终止进程组\n" + output};
  }
  int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  std::string head = exit_code == 0 ? "" : "exit=" + std::to_string(exit_code) + "\n";
  return {exit_code == 0, head + output};
}

ToolResult list_dir_tool(const nlohmann::json& args, ToolContext& ctx) {
  std::string path = arg_str(args, "path", ".");
  if (path[0] != '/') path = ctx.workspace + "/" + path;
  DIR* d = ::opendir(path.c_str());
  if (!d) return {false, "无法打开目录: " + path};
  nlohmann::json entries = nlohmann::json::array();
  struct dirent* e;
  while ((e = ::readdir(d)) != nullptr) {
    std::string name = e->d_name;
    if (name == "." || name == "..") continue;
    std::string full = path + "/" + name;
    struct stat st;
    std::string kind = ::lstat(full.c_str(), &st) == 0
                           ? (S_ISDIR(st.st_mode) ? "dir"
                              : S_ISLNK(st.st_mode) ? "link" : "file")
                           : "?";
    entries.push_back({{"name", name}, {"kind", kind}});
  }
  ::closedir(d);
  return {true, entries.dump()};
}

}  // namespace

void register_system_tools(ToolRegistry& reg) {
  {
    ToolDefinition t;
    t.name = "exec_command";
    t.description =
        "执行命令（不经 shell 解释，避免注入）；进程组运行，超时整组终止";
    t.parameters = {{"type", "object"},
                    {"properties",
                     {{"command", {{"type", "string"}, {"description", "可执行程序"}}},
                      {"args", {{"type", "array"}, {"items", {{"type", "string"}}}}},
                      {"timeout", {{"type", "integer"}, {"description", "秒，默认 300"}}}}},
                    {"required", {"command"}}};
    t.needs_approval = true;
    t.handler = exec_command_tool;
    reg.register_tool(std::move(t));
  }
  {
    ToolDefinition t;
    t.name = "list_dir";
    t.description = "列出目录内容（名称 + 类型）";
    t.parameters = {{"type", "object"},
                    {"properties", {{"path", {{"type", "string"}}}}},
                    {"required", {}}};
    t.handler = list_dir_tool;
    reg.register_tool(std::move(t));
  }
}

}  // namespace da
