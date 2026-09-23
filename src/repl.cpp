#include "repl.hpp"

#include <cstdio>
#include <iostream>
#include <string>

namespace da {

static bool handle_slash(const std::string& line, App& app) {
  if (line == "/exit" || line == "/quit") return true;
  if (line == "/status") {
    auto& m = app.llm().config().current_model();
    std::printf("模型: %s\nAPI: %s\n", m.model.c_str(), m.api_url.c_str());
    return false;
  }
  if (line.rfind("/model ", 0) == 0) {
    app.llm().switch_model(line.substr(7));
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
    std::printf("命令: /exit /quit /status /model <name> /skills /memory\n");
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
  return 0;
}

}  // namespace da
