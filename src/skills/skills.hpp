#pragma once
// 技能系统（对应 Rust skills/）
// 扫描 skills/*/SKILL.md，解析 YAML frontmatter，按名称排序注入 system prompt
#include <string>
#include <vector>

#include "utils/frontmatter.hpp"

namespace da {

struct Skill {
  std::string name;         // 目录名
  std::string title;        // frontmatter name
  std::string description;  // frontmatter description
  std::vector<std::string> tags;
  bool disabled = false;
  std::string body;         // 正文（指令内容）
};

class SkillRegistry {
public:
  // 扫描目录下每个子目录的 SKILL.md；返回加载数量
  int scan(const std::string& skills_dir);

  const std::vector<Skill>& skills() const { return skills_; }

  // 按名称查找
  const Skill* find(const std::string& name) const;

  // 生成注入 system prompt 的技能说明段（按名称排序）
  std::string render_prompt_section() const;

  // 激活某技能：返回其正文（无则空）
  std::string activate(const std::string& name) const;

private:
  std::vector<Skill> skills_;
};

}  // namespace da
