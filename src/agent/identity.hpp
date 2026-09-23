#pragma once
// 身份系统（对应 Rust agent/identity.rs）
// 5 种角色，各自配 system prompt 附加段与默认工具集
#include <string>
#include <vector>

namespace da {

enum class Identity {
  General,      // 通用
  Architect,    // 架构师：设计/拆解
  Implementer,  // 实现者：写代码
  Reviewer,     // 审查者：code review
  Tester,       // 测试者：写测试/跑测试
  Debugger,     // 调试者：定位修复
};

inline const char* identity_name(Identity id) {
  switch (id) {
    case Identity::General: return "general";
    case Identity::Architect: return "architect";
    case Identity::Implementer: return "implementer";
    case Identity::Reviewer: return "reviewer";
    case Identity::Tester: return "tester";
    case Identity::Debugger: return "debugger";
  }
  return "general";
}

// 从字符串解析（编排器/CLI 用）
Identity identity_from_string(const std::string& s);

// 该身份附加的 system prompt 段
std::string identity_prompt(Identity id);

// 该身份的默认工具集（空 = 全部工具）
std::vector<std::string> identity_default_tools(Identity id);

}  // namespace da
