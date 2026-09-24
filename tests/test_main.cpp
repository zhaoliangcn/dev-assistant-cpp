// 极简断言测试主入口
#include "test_common.hpp"

int test_toml();
int test_frontmatter();
int test_token_counter();
int test_gitignore();
int test_llm();
int test_tools();
int test_security();

TestStats g_stats;

int main() {
  test_toml();
  test_frontmatter();
  test_token_counter();
  test_gitignore();
  test_llm();
  test_tools();
  test_security();
  std::printf("passed=%d failed=%d\n", g_stats.passed, g_stats.failed);
  return g_stats.failed == 0 ? 0 : 1;
}
