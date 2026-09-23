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
    std::printf("命令: /exit /quit /status /model [name] /skills /memory\n");
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
