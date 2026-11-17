#pragma once
// 系统提示词构建（对应 Rust prompt.rs：角色 + 环境信息 + 技能注入 + 身份段）
#include <string>

#include "agent/identity.hpp"
#include "skills/skills.hpp"

namespace da {

// skills 传 nullptr 则跳过技能段；
// identity 非 General 时在末尾追加身份指令与子代理执行规则
std::string build_system_prompt(const SkillRegistry* skills,
                                Identity identity = Identity::General);

}  // namespace da
