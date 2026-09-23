#include "utils/gitignore.hpp"
#include "test_common.hpp"

int test_gitignore() {
  using namespace da;
  GitignoreMatcher m;
  m.add_rules("*.log\nbuild/\n!keep.log\ntemp/*.tmp\ndocs/**/draft.md\n");
  EXPECT(m.is_ignored("a.log"));
  EXPECT(m.is_ignored("sub/b.log"));
  EXPECT(m.is_ignored("build/x.o"));
  EXPECT(m.is_ignored("build/sub/x.o"));
  EXPECT(!m.is_ignored("keep.log"));        // 取反规则
  EXPECT(m.is_ignored("temp/x.tmp"));
  EXPECT(!m.is_ignored("temp/x.txt"));
  EXPECT(m.is_ignored("docs/a/draft.md"));  // ** 跨段
  EXPECT(m.is_ignored("docs/draft.md"));    // ** 零段
  EXPECT(!m.is_ignored("src/main.cpp"));
  EXPECT(GitignoreMatcher::is_default_ignored(".git/config"));
  EXPECT(GitignoreMatcher::is_default_ignored("project/target/debug"));
  return 0;
}
