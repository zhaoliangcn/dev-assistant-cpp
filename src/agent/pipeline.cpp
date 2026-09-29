#include "agent/pipeline.hpp"
#include "utils/utf8.hpp"

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
                                  PipelineStage::Tester,
                                  PipelineStage::Repair,
                                  PipelineStage::Document};

  while (stage_idx < 6) {
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
      case PipelineStage::Repair: {
        // 汇总审查/测试未决问题做最终修复（对齐 Rust 六阶段中的 Repair）
        std::string issues;
        for (const StageOutput* o = last_of(PipelineStage::Reviewer); o;
             o = nullptr)
          if (!o->ok) issues += "审查意见: " + o->output + "\n";
        if (const StageOutput* t = last_of(PipelineStage::Tester); t && !t->ok)
          issues += "测试报告: " + t->output + "\n";
        const StageOutput* impl = last_of(PipelineStage::Implementer);
        prompt = "目标: " + objective + "\n实现产出如下:\n" +
                 (impl ? impl->output : "") + "\n" +
                 (issues.empty()
                      ? "审查与测试均已通过，请做最终检查与收尾（无未决问题则说明）。"
                      : "未决问题如下，请逐一修复:\n" + issues);
        break;
      }
      case PipelineStage::Document: {
        // 记录：汇总全程产出生成完成报告（对齐 Rust 六阶段中的 Document）
        std::string summary;
        for (const auto& o : out)
          summary += std::string("- [") + pipeline_stage_name(o.stage) + "] " +
                     (o.ok ? "通过" : "未通过") + ": " +
                     truncate_utf8(o.output, 200) + "\n";
        prompt = "目标: " + objective +
                 "\n各阶段产出摘要如下:\n" + summary +
                 "\n请生成任务完成报告：做了什么、改动清单、遗留风险。";
        break;
      }
    }

    StageOutput so;
    so.stage = s;
    // 阶段身份映射（对齐 Rust identity.rs 各阶段角色）
    Identity role = Identity::General;
    switch (s) {
      case PipelineStage::Architect: role = Identity::Architect; break;
      case PipelineStage::Implementer: role = Identity::Implementer; break;
      case PipelineStage::Reviewer: role = Identity::Reviewer; break;
      case PipelineStage::Tester: role = Identity::Tester; break;
      case PipelineStage::Repair: role = Identity::Debugger; break;
      case PipelineStage::Document: role = Identity::Reviewer; break;
    }
    ToolResult r = runner(prompt, role, out);
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
