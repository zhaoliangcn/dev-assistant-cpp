#include "utils/frontmatter.hpp"
#include "test_common.hpp"

int test_frontmatter() {
  using namespace da;
  std::string doc = R"(---
name: my-skill
description: "测试技能"
tags:
  - cpp
  - agent
enabled: true
---
正文内容第一行
第二行)";
  Frontmatter fm;
  std::string body;
  EXPECT(parse_frontmatter(doc, fm, body));
  EXPECT_EQ(fm.get_string("name"), "my-skill");
  EXPECT_EQ(fm.get_string("description"), "测试技能");
  auto tags = fm.get_array("tags");
  EXPECT_EQ(tags.size(), 2u);
  EXPECT_EQ(tags[0], "cpp");
  EXPECT_EQ(tags[1], "agent");
  EXPECT_EQ(fm.get_string("enabled"), "true");
  EXPECT(body.find("正文内容第一行") != std::string::npos);
  EXPECT(body.find("第二行") != std::string::npos);
  EXPECT(body.find("---") == std::string::npos);
  return 0;
}
