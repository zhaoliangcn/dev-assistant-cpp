#include "config/toml.hpp"
#include "test_common.hpp"

int test_toml() {
  using namespace da;
  std::string src = R"(
# 注释
api_url = "https://api.example.com/v1"  # 行内注释
max_turns = 20
temperature = 0.7
stream = true
models = ["deepseek-chat", "glm-4"]

[openai]
api_key = "sk-xxx"
base = "https://api.deepseek.com"

[dream]
enabled = false
interval_hours = 24
)";
  TomlTable root;
  std::map<std::string, TomlTable> tables;
  EXPECT(Toml::parse(src, root, tables));

  EXPECT_EQ(Toml::get(root, tables, "api_url")->as_string(),
            "https://api.example.com/v1");
  EXPECT_EQ(Toml::get(root, tables, "max_turns")->type,
            TomlValue::Type::Int);
  EXPECT_EQ(Toml::get(root, tables, "max_turns")->i, 20);
  EXPECT_EQ(Toml::get(root, tables, "temperature")->type,
            TomlValue::Type::Double);
  EXPECT(Toml::get(root, tables, "stream")->b == true);
  EXPECT_EQ(Toml::get(root, tables, "models")->arr.size(), 2u);
  EXPECT_EQ(Toml::get(root, tables, "models")->arr[0].as_string(),
            "deepseek-chat");

  EXPECT_EQ(Toml::get(root, tables, "openai.api_key")->as_string(), "sk-xxx");
  EXPECT_EQ(Toml::get(root, tables, "openai.base")->as_string(),
            "https://api.deepseek.com");
  EXPECT_EQ(Toml::get(root, tables, "dream.enabled")->b, false);
  EXPECT_EQ(Toml::get(root, tables, "dream.interval_hours")->i, 24);
  EXPECT_EQ(Toml::get(root, tables, "nonexist"), nullptr);
  return 0;
}
