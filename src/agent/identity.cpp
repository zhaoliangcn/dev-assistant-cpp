#include "agent/identity.hpp"

namespace da {

Identity identity_from_string(const std::string& s) {
  if (s == "architect") return Identity::Architect;
  if (s == "implementer") return Identity::Implementer;
  if (s == "reviewer") return Identity::Reviewer;
  if (s == "tester") return Identity::Tester;
  if (s == "debugger") return Identity::Debugger;
  return Identity::General;
}

std::string identity_prompt(Identity id) {
  switch (id) {
    case Identity::Architect:
      return "你当前身份是【架构师】：负责理解需求、设计模块划分与数据流、"
             "拆解为可执行的任务清单。不要直接写实现代码，输出设计方案与任务分解。\n";
    case Identity::Implementer:
      return "你当前身份是【实现者】：负责按设计编写代码。遵循项目现有风格，"
             "优先使用已有工具完成读写与验证；改动尽量小而聚焦。\n";
    case Identity::Reviewer:
      return "你当前身份是【审查者】：对代码变更做正确性/安全/可维护性审查，"
             "按严重程度列出问题并给出具体修改建议。不直接修改代码。\n";
    case Identity::Tester:
      return "你当前身份是【测试者】：为变更编写或运行测试，报告通过/失败结果，"
             "失败时给出最小复现信息。如实报告，不虚报通过。\n";
    case Identity::Debugger:
      return "你当前身份是【调试者】：负责定位缺陷根因。先复现，再缩小范围，"
             "确认根因后再提出修复；禁止未定位就盲改。\n";
    case Identity::General:
      return "";
  }
  return "";
}

std::vector<std::string> identity_default_tools(Identity id) {
  switch (id) {
    case Identity::Architect:
      return {"read_file", "glob", "grep", "list_dir"};
    case Identity::Reviewer:
      return {"read_file", "glob", "grep", "list_dir"};
    case Identity::Implementer:
      return {};  // 全部
    case Identity::Tester:
      return {"read_file", "write_file", "edit_file", "exec_command", "glob",
              "grep", "list_dir"};
    case Identity::Debugger:
      return {"read_file", "exec_command", "grep", "glob", "list_dir"};
    case Identity::General:
      return {};  // 全部
  }
  return {};
}

}  // namespace da
