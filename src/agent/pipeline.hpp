#pragma once
// 流水线（对应 Rust agent/pipeline.rs）
// 按 Architect → Implementer → Reviewer → Tester → Repair → Document 顺序
// 执行六阶段（对齐 Rust 版），每阶段由子代理完成；
// Reviewer 阶段失败可回退重跑 Implementer
#include <functional>
#include <string>
#include <vector>

#include "agent/identity.hpp"
#include "tools/registry.hpp"

namespace da {

enum class PipelineStage {
  Architect,
  Implementer,
  Reviewer,
  Tester,
  Repair,    // 修复：汇总审查/测试未决问题做最终修复
  Document,  // 记录：生成任务完成报告
};

inline const char* pipeline_stage_name(PipelineStage s) {
  switch (s) {
    case PipelineStage::Architect: return "architect";
    case PipelineStage::Implementer: return "implementer";
    case PipelineStage::Reviewer: return "reviewer";
    case PipelineStage::Tester: return "tester";
    case PipelineStage::Repair: return "repair";
    case PipelineStage::Document: return "document";
  }
  return "?";
}

struct StageOutput {
  PipelineStage stage;
  bool ok = false;
  std::string output;
};

// 阶段执行回调（Agent 层提供：以指定身份运行一个子代理 turn）
using StageRunner =
    std::function<ToolResult(const std::string& prompt, Identity role,
                             const std::vector<StageOutput>& prior)>;

class Pipeline {
public:
  explicit Pipeline(int max_rework = 1) : max_rework_(max_rework) {}

  // 顺序执行 6 阶段；Reviewer 不通过时回退重跑 Implementer（最多 max_rework 次）
  // 返回全部阶段是否最终通过
  bool run(const std::string& objective, StageRunner runner,
           std::vector<StageOutput>& out) const;

  void set_max_rework(int n) { max_rework_ = n; }

private:
  int max_rework_;
};

}  // namespace da
