#pragma once
// 共享断言宏：test_main.cpp 定义 g_stats，各测试文件 include
#include <cstdio>

struct TestStats { int passed = 0, failed = 0; };
extern TestStats g_stats;

#define EXPECT(cond)                                                        \
  do {                                                                      \
    if (cond) { g_stats.passed++; }                                         \
    else {                                                                  \
      g_stats.failed++;                                                     \
      std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);  \
    }                                                                       \
  } while (0)

#define EXPECT_EQ(a, b) EXPECT((a) == (b))
