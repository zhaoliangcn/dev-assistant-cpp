#include "agent/token_counter.hpp"
#include "test_common.hpp"

int test_token_counter() {
  using namespace da;
  // ASCII: 8 字符 × 0.75 = 6
  EXPECT_EQ(estimate_tokens("abcdefgh"), 6u);
  // 空串
  EXPECT_EQ(estimate_tokens(""), 0u);
  // 4 个 CJK 字符 = 8 tokens
  EXPECT_EQ(estimate_tokens("你好世界"), 8u);
  // 混合：2 CJK(4) + 4 ascii(3) = 7
  EXPECT_EQ(estimate_tokens("你好abcd"), 7u);
  return 0;
}
