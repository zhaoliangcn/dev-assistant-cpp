#pragma once
// 调度器（对应 Rust scheduler/）
// 时间轮：60 槽秒级轮 + cron 子集（分/时/日/月/周 五段）+ 任务 JSONL 存储
#include <chrono>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace da {

// cron 五段子集：分 时 日 月 周；支持 * / 数字 / 逗号列表
struct CronSpec {
  bool every_minute = false;
  int minute = -1, hour = -1, day = -1, month = -1, weekday = -1;  // -1 = 不限
  // 解析失败返回 false
  static bool parse(const std::string& spec, CronSpec& out);
  // 判断给定时刻是否匹配
  bool matches(const std::tm& t) const;
};

struct ScheduledTask {
  std::string id;
  std::string name;
  std::string cron;     // cron 表达式（一次性任务为空）
  long run_at = 0;      // 一次性任务：epoch 秒
  std::string prompt;   // 到点触发的 Agent 输入
  bool enabled = true;
};

class Scheduler {
public:
  // 到点回调：投递到 Agent 线程池执行 prompt
  using FireCallback = std::function<void(const ScheduledTask&)>;

  explicit Scheduler(FireCallback on_fire) : on_fire_(std::move(on_fire)) {}

  void add_task(ScheduledTask t);
  bool remove_task(const std::string& id);
  std::vector<ScheduledTask> tasks() const;

  // 任务存储：JSONL 持久化（对应 scheduled_tasks.jsonl）
  bool save(const std::string& path) const;
  bool load(const std::string& path);

  // tick：由调度线程周期调用（间隔 1s）；
  // 返回本次触发的任务数
  int tick();

  // 调度线程主循环（阻塞，直到 stop() 被调用）
  void run_loop();
  void stop() { running_ = false; }
  bool running() const { return running_; }

private:
  void fire(const ScheduledTask& t);

  std::vector<ScheduledTask> tasks_;
  FireCallback on_fire_;
  bool running_ = false;
  long last_tick_sec_ = 0;  // 上次 tick 的 epoch 秒
};

}  // namespace da
