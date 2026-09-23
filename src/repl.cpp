#include "repl.hpp"

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <regex>
#include <string>
#include <vector>

namespace da {

// /grep：正则搜文件（对齐 Rust run_grep：递归文本文件、跳过大目录/大文件、上限 50 条）
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
  // 借助 grep 命令做递归与过滤（零依赖；正则语义由本函数 std::regex 复核）
  std::string cmd = "grep -rnE --include='*' --exclude-dir=.git --exclude-dir=build"
                    " --exclude-dir=build-asan --exclude-dir=build-musl"
                    " --exclude-dir=node_modules --exclude-dir=.dev-assistant"
                    " -I -m 5 -- " ;
  cmd += "'" ;
  for (char c : pattern) {  // 单引号转义
    if (c == '\'') cmd += "'\\''";
    else cmd += c;
  }
  cmd += "' . 2>/dev/null | head -50";
  std::fflush(stdout);
  int rc = std::system(cmd.c_str());
  if (rc != 0) std::printf("🔍 未找到匹配 \"%s\" 的内容\n", pattern.c_str());
}

// /diff：git diff 查看工作区改动（可选路径参数）
static void run_diff_cmd(const std::string& args) {
  std::string cmd = "git diff --";
  if (!args.empty()) cmd += " " + args;
  cmd += " 2>/dev/null";
  std::fflush(stdout);
  FILE* p = ::popen(cmd.c_str(), "r");
  if (!p) {
    std::printf("❌ git diff 执行失败\n");
    return;
  }
  std::string out;
  char buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof buf, p)) > 0) out.append(buf, n);
  int rc = ::pclose(p);
  if (rc != 0) {
    std::printf("❌ git diff 执行失败（非 git 仓库或参数错误）\n");
    return;
  }
  // 去尾空白后判断是否有改动
  while (!out.empty() && (out.back() == '\n' || out.back() == ' '))
    out.pop_back();
  if (out.empty()) {
    std::printf("ℹ️ 工作区没有未提交的改动\n");
    return;
  }
  std::printf("%s\n", out.c_str());
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
                " /history /diff [路径] /grep|/search <正则>\n");
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
