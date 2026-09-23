#include "agent/pipeline.hpp"

namespace da {

bool Pipeline::run(const std::string& objective, StageRunner runner,
                   std::vector<StageOutput>& out) const {
  out.clear();
  auto last_of = [&](PipelineStage s) -> const StageOutput* {
    for (auto it = out.rbegin(); it != out.rend(); ++it)
      if (it->stage == s) return &*it;
    return nullptr;
  };

  int rework = 0;
  size_t stage_idx = 0;
  const PipelineStage stages[] = {PipelineStage::Architect,
                                  PipelineStage::Implementer,
                                  PipelineStage::Reviewer,
                                  PipelineStage::Tester};

  while (stage_idx < 4) {
    PipelineStage s = stages[stage_idx];
    std::string prompt;
    switch (s) {
      case PipelineStage::Architect:
        prompt = "目标: " + objective +
                 "\n请做架构设计：模块划分、数据流、任务拆解。";
        break;
      case PipelineStage::Implementer: {
        const StageOutput* arch = last_of(PipelineStage::Architect);
        prompt = "目标: " + objective + "\n架构设计如下:\n" +
                 (arch ? arch->output : "") +
                 "\n请按设计实现代码。";
        // 若是审查回退，附上审查意见
        const StageOutput* rev = last_of(PipelineStage::Reviewer);
        if (rev && !rev->ok)
          prompt += "\n上一轮审查未通过，审查意见如下，请修复:\n" + rev->output;
        break;
      }
      case PipelineStage::Reviewer: {
        const StageOutput* impl = last_of(PipelineStage::Implementer);
        prompt = "目标: " + objective + "\n实现产出如下:\n" +
                 (impl ? impl->output : "") +
                 "\n请审查正确性/安全/可维护性。通过则明确输出 PASS，否则列出问题。";
        break;
      }
      case PipelineStage::Tester: {
        const StageOutput* impl = last_of(PipelineStage::Implementer);
        prompt = "目标: " + objective + "\n实现产出如下:\n" +
                 (impl ? impl->output : "") +
                 "\n请编写/运行测试并如实报告结果。";
        break;
      }
    }

    StageOutput so;
    so.stage = s;
    ToolResult r = runner(prompt, Identity::General, out);
    // 阶段身份映射：Reviewer 阶段用 Reviewer 身份
    so.ok = r.ok;
    so.output = r.output;
    out.push_back(so);

    // Reviewer 不通过 → 回退 Implementer（限次）
    if (s == PipelineStage::Reviewer && !r.ok && rework < max_rework_) {
      rework++;
      stage_idx = 1;  // 回到 Implementer
      continue;
    }
    stage_idx++;
  }

  // 全部阶段 ok 才算通过
  for (const auto& o : out)
    if (!o.ok) return false;
  return true;
}

}  // namespace da
