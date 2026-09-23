#pragma once
// 系统提示词构建（对应 Rust prompt.rs：角色 + 环境信息 + 技能注入）
#include <string>

#include "skills/skills.hpp"

namespace da {

// skills 传 nullptr 则跳过技能段
std::string build_system_prompt(const SkillRegistry* skills);

}  // namespace da
