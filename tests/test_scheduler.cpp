// CronSpec 单测：列表/步进全量匹配（C3 回归）+ parse 独立性（C2）
#include "test_common.hpp"

#include <ctime>

#include "scheduler/scheduler.hpp"

using namespace da;

static std::tm make_tm(int min, int hour, int mday, int mon, int wday) {
  std::tm t{};
  t.tm_min = min;
  t.tm_hour = hour;
  t.tm_mday = mday;
  t.tm_mon = mon - 1;
  t.tm_wday = wday;
  return t;
}

int test_scheduler() {
  // ---- C3：逗号列表全量匹配（原实现只取第一个值）----
  {
    CronSpec s;
    EXPECT(CronSpec::parse("1,15,30 * * * *", s));
    EXPECT(s.matches(make_tm(1, 10, 1, 1, 3)));
    EXPECT(s.matches(make_tm(15, 10, 1, 1, 3)));
    EXPECT(s.matches(make_tm(30, 10, 1, 1, 3)));
    EXPECT(!s.matches(make_tm(16, 10, 1, 1, 3)));
  }
  // ---- C3：步进 */5 全量匹配 ----
  {
    CronSpec s;
    EXPECT(CronSpec::parse("*/5 * * * *", s));
    EXPECT(s.matches(make_tm(0, 10, 1, 1, 3)));
    EXPECT(s.matches(make_tm(5, 10, 1, 1, 3)));
    EXPECT(s.matches(make_tm(55, 10, 1, 1, 3)));
    EXPECT(!s.matches(make_tm(7, 10, 1, 1, 3)));
  }
  // ---- 多字段组合 ----
  {
    CronSpec s;
    EXPECT(CronSpec::parse("0 9 * * 1-5", s) == false);  // 区间语法不在子集内，明确拒绝
    EXPECT(CronSpec::parse("0 9 * * 1,2,3,4,5", s));
    EXPECT(s.matches(make_tm(0, 9, 1, 1, 1)));   // 周一
    EXPECT(!s.matches(make_tm(0, 9, 1, 1, 0)));  // 周日
    EXPECT(!s.matches(make_tm(0, 8, 1, 1, 1)));  // 小时不符
  }
  // ---- 混合列表 + 星号字段 ----
  {
    CronSpec s;
    EXPECT(CronSpec::parse("0,30 8,12 1,15 * *", s));
    EXPECT(s.matches(make_tm(0, 8, 1, 5, 3)));
    EXPECT(s.matches(make_tm(30, 12, 15, 5, 3)));
    EXPECT(!s.matches(make_tm(15, 8, 1, 5, 3)));
    EXPECT(!s.matches(make_tm(0, 9, 1, 5, 3)));
    EXPECT(!s.matches(make_tm(0, 8, 2, 5, 3)));
  }
  // ---- every_minute 标志 ----
  {
    CronSpec s;
    EXPECT(CronSpec::parse("* * * * *", s));
    EXPECT(s.every_minute);
    EXPECT(s.matches(make_tm(37, 23, 31, 12, 6)));
  }
  // ---- 非法表达式 ----
  {
    CronSpec s;
    EXPECT(!CronSpec::parse("60 * * * *", s));    // 分钟越界
    EXPECT(!CronSpec::parse("* 24 * * *", s));    // 小时越界
    EXPECT(!CronSpec::parse("* * * *", s));       // 段数不足
    EXPECT(!CronSpec::parse("a * * * *", s));     // 非数字
  }
  return g_stats.failed;
}
