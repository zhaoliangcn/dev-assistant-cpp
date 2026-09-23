#pragma once
// 编排器（对应 Rust orchestrator/）
// 任务依赖图：拓扑排序 → 就绪队列按优先级出队 → 失败重试 → 上游失败级联 skip
// checkpoint：每 N 个任务落盘，支持恢复
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "agent/identity.hpp"
#include "tools/registry.hpp"

namespace da {

enum class TaskState { Pending, Running, Done, Failed, Skipped };

inline const char* task_state_name(TaskState s) {
  switch (s) {
    case TaskState::Pending: return "pending";
    case TaskState::Running: return "running";
    case TaskState::Done: return "done";
    case TaskState::Failed: return "failed";
    case TaskState::Skipped: return "skipped";
  }
  return "?";
}

struct Task {
  std::string id;
  std::string prompt;             // 任务描述（作为子代理输入）
  std::vector<std::string> deps;  // 上游任务 id
  int priority = 0;               // 越大越先出队
  Identity role = Identity::General;
  TaskState state = TaskState::Pending;
  int retries_left = 1;
  std::string result;  // 成功后的产出文本
};

// 任务执行回调：由 Agent 层提供（通常转发给子代理）
using TaskRunner = std::function<ToolResult(const Task& task)>;

class Orchestrator {
public:
  void add_task(Task t);

  // 执行整个任务图；每 completed 批次落盘 checkpoint
  // 返回：成功 true（允许存在 skipped，但所有任务必须有终态）
  bool run(TaskRunner runner, const std::string& checkpoint_dir = "",
           int checkpoint_every = 2);

  // 恢复：从 checkpoint 文本重建任务状态
  static bool restore(const std::string& checkpoint_json,
                      std::vector<Task>& out);

  const std::vector<Task>& tasks() const { return tasks_; }

private:
  // 就绪队列：Pending 且依赖全部 Done；按 priority 降序、id 升序
  std::vector<size_t> ready_indices() const;
  // 上游失败/跳过时级联标记
  void cascade_skip(size_t idx);
  void save_checkpoint(const std::string& dir) const;

  std::vector<Task> tasks_;
  std::map<std::string, size_t> by_id_;
};

}  // namespace da
