// main.cpp：CLI 参数解析 / 运行模式分发（对应 Rust main.rs）
#include <cstdio>
#include <cstring>
#include <string>

#include "app.hpp"
#include "repl.hpp"
#include "web/web.hpp"

static void print_usage() {
  std::printf(
      "dev-assistant-cpp — 代码库级 AI 编程助手（C++ 版）\n\n"
      "用法:\n"
      "  dev-assistant                交互式 REPL\n"
      "  dev-assistant -m <消息>      单次执行\n"
      "  dev-assistant --web [--port N]  Web 模式（默认 127.0.0.1:8080）\n"
      "  dev-assistant init           生成配置模板\n"
      "  dev-assistant --help         本帮助\n");
}

static void cmd_init() {
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

int main(int argc, char** argv) {
  da::App app;

  if (argc >= 2) {
    if (std::strcmp(argv[1], "--help") == 0 || std::strcmp(argv[1], "-h") == 0) {
      print_usage();
      return 0;
    }
    if (std::strcmp(argv[1], "init") == 0) {
      cmd_init();
      return 0;
    }
    if (std::strcmp(argv[1], "--web") == 0) {
      int port = 8080;
      if (argc >= 4 && std::strcmp(argv[2], "--port") == 0)
        port = std::atoi(argv[3]);
      if (!app.init()) {
        std::fprintf(stderr, "未找到配置 .dev-assistant-models.toml（运行 `dev-assistant init` 生成）\n");
        return 1;
      }
      return da::run_web(app, port);
    }
    if (std::strcmp(argv[1], "-m") == 0) {
      if (argc < 3) {
        std::fprintf(stderr, "-m 需要消息参数\n");
        return 2;
      }
      std::string msg = argv[2];
      for (int i = 3; i < argc; i++) msg += " " + std::string(argv[i]);
      if (!app.init()) {
        std::fprintf(stderr, "未找到配置 .dev-assistant-models.toml（运行 `dev-assistant init` 生成）\n");
        return 1;
      }
      return app.process_message(msg);
    }
  }

  // REPL 模式
  if (!app.init()) {
    std::fprintf(stderr, "未找到配置 .dev-assistant-models.toml（运行 `dev-assistant init` 生成）\n");
    return 1;
  }
  return da::run_repl(app);
}
