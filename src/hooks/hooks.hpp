#pragma once
// Hooks（对应 Rust hooks/）
// 配置驱动的 shell 钩子：事件触发 → fork-exec 执行 → 捕获输出/超时控制
#include <string>
#include <vector>

namespace da {

enum class HookEvent {
  SessionStart,
  BeforeToolCall,   // 工具调用前（可否决：返回非零则取消）
  AfterToolCall,    // 工具调用后
  SessionEnd,
};

inline const char* hook_event_name(HookEvent e) {
  switch (e) {
    case HookEvent::SessionStart: return "session_start";
    case HookEvent::BeforeToolCall: return "before_tool_call";
    case HookEvent::AfterToolCall: return "after_tool_call";
    case HookEvent::SessionEnd: return "session_end";
  }
  return "?";
}

struct HookConfig {
  std::string name;
  HookEvent event;
  std::string command;      // shell 命令（经 /bin/sh -c）
  int timeout_sec = 30;
  bool blocking = false;    // blocking=true 且退出非零 → 否决事件
};

class HookManager {
public:
  void add_hook(HookConfig h);
  const std::vector<HookConfig>& hooks() const { return hooks_; }

  // 触发事件；payload 会以环境变量 DA_HOOK_PAYLOAD 传入
  // 返回：false 表示某 blocking 钩子否决（仅 BeforeToolCall 有意义）
  bool fire(HookEvent event, const std::string& payload,
            std::string& output) const;

  // 从配置加载 hooks（TOML 子集：[hooks.<name>] event/command/timeout/blocking）
  bool load_config(const std::string& toml_path);

private:
  std::vector<HookConfig> hooks_;
};

}  // namespace da
