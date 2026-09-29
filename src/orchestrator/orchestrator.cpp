#include "orchestrator/orchestrator.hpp"

#include <algorithm>

#include <nlohmann/json.hpp>

#include "utils/atomic_write.hpp"

namespace da {

using json = nlohmann::json;

void Orchestrator::add_task(Task t) {
  by_id_[t.id] = tasks_.size();
  tasks_.push_back(std::move(t));
}

std::vector<size_t> Orchestrator::ready_indices() const {
  std::vector<size_t> out;
  for (size_t i = 0; i < tasks_.size(); i++) {
    if (tasks_[i].state != TaskState::Pending) continue;
    bool ready = true;
    for (const auto& dep : tasks_[i].deps) {
      auto it = by_id_.find(dep);
      if (it == by_id_.end()) continue;  // 未知依赖视为无依赖
      if (tasks_[it->second].state != TaskState::Done) {
        ready = false;
        break;
      }
    }
    if (ready) out.push_back(i);
  }
  // priority 降序，同优先级按 id 升序保证确定性
  std::sort(out.begin(), out.end(), [&](size_t a, size_t b) {
    if (tasks_[a].priority != tasks_[b].priority)
      return tasks_[a].priority > tasks_[b].priority;
    return tasks_[a].id < tasks_[b].id;
  });
  return out;
}

void Orchestrator::cascade_skip(size_t idx) {
  // 下游（依赖 idx 对应任务的）全部级联 skipped
  const std::string& id = tasks_[idx].id;
  for (size_t i = 0; i < tasks_.size(); i++) {
    if (tasks_[i].state != TaskState::Pending) continue;
    if (std::find(tasks_[i].deps.begin(), tasks_[i].deps.end(), id) ==
        tasks_[i].deps.end())
      continue;
    tasks_[i].state = TaskState::Skipped;
    tasks_[i].result = "上游任务失败，级联跳过";
    cascade_skip(i);  // 递归传播
  }
}

void Orchestrator::save_checkpoint(const std::string& dir) const {
  if (dir.empty()) return;
  json arr = json::array();
  for (const auto& t : tasks_) {
    arr.push_back({{"id", t.id},
                   {"prompt", t.prompt},
                   {"deps", t.deps},
                   {"priority", t.priority},
                   {"role", identity_name(t.role)},
                   {"state", task_state_name(t.state)},
                   {"retries_left", t.retries_left},
                   {"result", t.result}});
  }
  atomic_write(dir + "/tasks.json", arr.dump(-1, ' ', false));
}

bool Orchestrator::restore(const std::string& checkpoint_json,
                           std::vector<Task>& out) {
  json arr = json::parse(checkpoint_json, nullptr, false);
  if (!arr.is_array()) return false;
  out.clear();
  for (const auto& j : arr) {
    Task t;
    t.id = j.value("id", "");
    t.prompt = j.value("prompt", "");
    if (j.contains("deps") && j["deps"].is_array())
      for (const auto& d : j["deps"]) t.deps.push_back(d.get<std::string>());
    t.priority = j.value("priority", 0);
    t.role = identity_from_string(j.value("role", "general"));
    std::string st = j.value("state", "pending");
    if (st == "running") t.state = TaskState::Running;
    else if (st == "done") t.state = TaskState::Done;
    else if (st == "failed") t.state = TaskState::Failed;
    else if (st == "skipped") t.state = TaskState::Skipped;
    t.retries_left = j.value("retries_left", 1);
    t.result = j.value("result", "");
    out.push_back(std::move(t));
  }
  return !out.empty();
}

bool Orchestrator::run(TaskRunner runner, const std::string& checkpoint_dir,
                       int checkpoint_every) {
  int completed_since_cp = 0;
  bool any_failed = false;

  while (true) {
    auto ready = ready_indices();
    if (ready.empty()) break;  // 无可执行任务：全 Done/Failed/Skipped 或死锁

    for (size_t idx : ready) {
      Task& t = tasks_[idx];
      t.state = TaskState::Running;
      ToolResult r = runner(t);
      if (r.ok) {
        t.state = TaskState::Done;
        t.result = r.output;
      } else if (t.retries_left > 0) {
        t.retries_left--;
        t.state = TaskState::Pending;  // 重新入队重试
      } else {
        t.state = TaskState::Failed;
        t.result = r.output;
        any_failed = true;
        cascade_skip(idx);
      }
    }

    if (++completed_since_cp >= checkpoint_every) {
      completed_since_cp = 0;
      save_checkpoint(checkpoint_dir);
    }
  }
  save_checkpoint(checkpoint_dir);
  (void)any_failed;
  return true;
}

}  // namespace da
