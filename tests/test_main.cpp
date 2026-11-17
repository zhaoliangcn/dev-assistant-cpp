// 极简断言测试主入口
#include "test_common.hpp"

int test_toml();
int test_frontmatter();
int test_token_counter();
int test_gitignore();
int test_llm();
int test_tools();
int test_security();
int test_scheduler();
int test_prompt_pipeline();
int test_agent_tools_registered();
int test_batch_read_files();
int test_compressor_force();
int test_kb_tools();
int test_kb_source_refs();

TestStats g_stats;

int main() {
  test_toml();
  test_frontmatter();
  test_token_counter();
  test_gitignore();
  test_llm();
  test_tools();
  test_security();
  test_scheduler();
  test_prompt_pipeline();
  test_agent_tools_registered();
  test_batch_read_files();
  test_compressor_force();
  test_kb_tools();
  test_kb_source_refs();
  std::printf("passed=%d failed=%d\n", g_stats.passed, g_stats.failed);
  return g_stats.failed == 0 ? 0 : 1;
}
