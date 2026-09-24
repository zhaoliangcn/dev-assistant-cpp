// main.cpp：CLI 参数解析 / 运行模式分发（对应 Rust main.rs；
// 支持 --project / --config / --model 长参数，对齐 Rust 版）
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>

#include "app.hpp"
#include "repl.hpp"
#include "web/web.hpp"

namespace {

void print_usage() {
  std::printf(
      "dev-assistant-cpp — 代码库级 AI 编程助手（C++ 版）\n\n"
      "用法:\n"
      "  dev-assistant [选项]                交互式 REPL\n"
      "  dev-assistant [选项] -m <消息>      单次执行\n"
      "  dev-assistant [选项] --web [--port N]  Web 模式（默认 127.0.0.1:8080）\n"
      "  dev-assistant init                  生成配置模板\n"
      "  dev-assistant --help                本帮助\n"
      "\n选项:\n"
      "  --project <dir>   项目工作目录（默认当前目录；工具/配置/会话日志指向该目录）\n"
      "  --config <path>   模型配置文件（默认 <项目目录>/.dev-assistant-models.toml）\n"
      "  --model <name>    启动即切换模型（按名称或模型 ID）\n"
      "  --max-tokens <n>  上下文窗口预算（默认 262144；仅本地预算，不发给 LLM API）\n"
      "  --no-hooks        禁用钩子（session-start 等钩子不执行）\n"
      "  --no-approval     无审批模式（写类/执行类工具直接执行，不弹确认）\n"
      "  --hooks-dry-run   预览将执行的钩子（不实际执行）\n"
      "  --verbose         启用详细日志输出\n");
}

void cmd_init() {
  const char* tpl =
      "# dev-assistant 配置\n"
      "api_url = \"${API_URL:-https://api.deepseek.com/v1/chat/completions}\"\n"
      "api_key = \"${API_KEY}\"\n"
      "model = \"deepseek-chat\"\n"
      "max_turns = 40\n";
  std::FILE* f = std::fopen(".dev-assistant-models.toml", "wx");
  if (!f) {
    std::fprintf(stderr, "配置文件已存在: .dev-assistant-models.toml\n");
    return;
  }
  std::fputs(tpl, f);
  std::fclose(f);
  std::printf("已生成 .dev-assistant-models.toml，请设置 API_KEY 环境变量或编辑文件。\n");
}

// 基于启动目录把相对路径绝对化（对应 Rust to_restart_args 的 absolutize，
// 避免 chdir 到 --project 后相对路径的解析基准改变）
std::string absolutize(const std::string& p, const std::string& startup_cwd) {
  if (p.empty() || p[0] == '/') return p;
  return startup_cwd + "/" + p;
}

}  // namespace

int main(int argc, char** argv) {
  // 启动 cwd：--project / --config 相对路径的解析基准
  char cwd_buf[4096];
  std::string startup_cwd = ::getcwd(cwd_buf, sizeof cwd_buf) ? cwd_buf : ".";

  // 先扫描全部参数，提取 --project/--config/--model（支持 --x <v> 与 --x=v），
  // 其余参数原样保留到 rest，供模式分派
  std::string project, config_path, model_name;
  long max_tokens = 0;
  bool no_hooks = false, hooks_dry_run = false, verbose = false;
  bool no_approval = false;
  std::vector<std::string> rest;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    std::string inline_v;
    bool has_inline = false;
    size_t eq = a.find('=');
    if (eq != std::string::npos && a.rfind("--", 0) == 0) {
      inline_v = a.substr(eq + 1);
      a = a.substr(0, eq);
      has_inline = true;
    }
    if (a == "--no-hooks") { no_hooks = true; continue; }
    if (a == "--no-approval") { no_approval = true; continue; }
    if (a == "--hooks-dry-run") { hooks_dry_run = true; continue; }
    if (a == "--verbose") { verbose = true; continue; }
    if (a == "--project" || a == "--config" || a == "--model" ||
        a == "--max-tokens") {
      std::string* out = a == "--project" ? &project
                        : a == "--config" ? &config_path
                        : a == "--model"  ? &model_name
                                          : nullptr;
      if (has_inline) {
        if (!out) max_tokens = std::atol(inline_v.c_str());
        else *out = inline_v;
      } else if (i + 1 < argc) {
        if (!out) max_tokens = std::atol(argv[++i]);
        else *out = argv[i];
      } else {
        std::fprintf(stderr, "%s 需要参数\n", a.c_str());
        return 2;
      }
    } else {
      rest.push_back(has_inline ? std::string(argv[i]) : a);
    }
  }

  // --project：切换到目标项目目录（工具/配置/会话日志随之指向该目录）
  if (!project.empty()) {
    std::string target = absolutize(project, startup_cwd);
    if (::chdir(target.c_str()) != 0) {
      std::fprintf(stderr, "无法切换到项目目录: %s\n", target.c_str());
      return 1;
    }
  }
  // --config：相对路径基于启动 cwd 绝对化（chdir 之后基准已变）
  if (!config_path.empty()) config_path = absolutize(config_path, startup_cwd);

  da::App app;
  // 无审批模式：须在 init() 前设置（init 内构造 ApprovalManager 时生效）
  app.set_no_approval(no_approval);

  // --model/--max-tokens：init 完成后启动即生效；失败则明确报错退出
  auto apply_model = [&]() -> bool {
    if (max_tokens > 0) app.set_max_tokens(max_tokens);
    // 钩子开关（对应 Rust hooks_enabled / dry_run）
    app.hooks().set_enabled(!no_hooks);
    app.hooks().set_dry_run(hooks_dry_run);
    app.set_verbose(verbose);
    if (model_name.empty()) return true;
    if (app.llm().switch_model(model_name)) return true;
    std::fprintf(stderr, "未找到模型: %s（可用 /model 查看模型列表）\n",
                 model_name.c_str());
    return false;
  };
  auto init_fail = [](const std::string& cfg) {
    std::fprintf(stderr, "未找到配置: %s（运行 `dev-assistant init` 生成）\n",
                 cfg.empty() ? ".dev-assistant-models.toml" : cfg.c_str());
    return 1;
  };

  if (!rest.empty()) {
    const std::string& c0 = rest[0];
    if (c0 == "--help" || c0 == "-h") {
      print_usage();
      return 0;
    }
    if (c0 == "init") {
      cmd_init();
      return 0;
    }
    if (c0 == "--web") {
      int port = 8080;
      if (rest.size() >= 3 && rest[1] == "--port")
        port = std::atoi(rest[2].c_str());
      if (!app.init(config_path)) return init_fail(config_path);
      if (!apply_model()) return 1;
      return da::run_web(app, port);
    }
    if (c0 == "-m") {
      if (rest.size() < 2) {
        std::fprintf(stderr, "-m 需要消息参数\n");
        return 2;
      }
      std::string msg = rest[1];
      for (size_t i = 2; i < rest.size(); i++) msg += " " + rest[i];
      if (!app.init(config_path)) return init_fail(config_path);
      if (!apply_model()) return 1;
      return app.process_message(msg);
    }
  }

  // REPL 模式
  if (!app.init(config_path)) return init_fail(config_path);
  if (!apply_model()) return 1;
  return da::run_repl(app);
}
