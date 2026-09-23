#pragma once
// 子代理工具（对应 Rust tools/subagent.rs）
// spawn_subagent：池内新 turn，深度 ≤3，独立上下文，受限工具集
// 结果文本回填父上下文
#include <string>

#include "tools/registry.hpp"

namespace da {

inline constexpr int kMaxSubagentDepth = 3;

// 供 Agent 层注入的回调：以指定身份/深度执行一次子任务，
// 返回子代理的最终文本输出（失败返回 ok=false）
using SubagentRunner = std::function<ToolResult(
    const std::string& prompt, const std::string& role, int depth)>;

// 注册 spawn_subagent 工具；runner 由 Agent 层提供
void register_subagent_tool(ToolRegistry& reg, SubagentRunner runner);

}  // namespace da
