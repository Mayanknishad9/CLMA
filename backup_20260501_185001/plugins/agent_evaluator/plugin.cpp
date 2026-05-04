#include <core/AgentPlugin.hpp>
#include <sstream>
#include <algorithm>
#include <iomanip>

namespace clma {

/**
 * Evaluator 插件 — 评分评估
 * 原 Orchestrator::evaluateResult
 */
class EvaluatorPlugin : public AgentPlugin {
public:
    PluginInfo getInfo() const override {
        PluginInfo info;
        info.id = "agent.evaluator";
        info.name = "Result Evaluator";
        info.version = {1, 0, 0};
        info.type = PluginType::CUSTOM;
        info.author = "CLMA Core";
        info.description = "Evaluates and scores verification results";
        info.apiVersion = 1;
        return info;
    }

    AgentStep getStepType() const override {
        return AgentStep::EVALUATOR;
    }

    AgentResult execute(const AgentContext& ctx) override {
        AgentResult result;
        result.success = true;

        const std::string& verification = ctx.previousResult;
        const Rule& rule = ctx.currentRule;
        size_t content_length = verification.length();

        // 使用 rule.weights 来调整各维度的权重
        double reasonableness_weight = 1.0;
        double executability_weight = 1.0;
        double satisfaction_weight = 1.0;

        auto rw = rule.weights.find("reasonableness");
        auto ew = rule.weights.find("executability");
        auto sw = rule.weights.find("satisfaction");

        if (rw != rule.weights.end()) reasonableness_weight = rw->second;
        if (ew != rule.weights.end()) executability_weight = ew->second;
        if (sw != rule.weights.end()) satisfaction_weight = sw->second;

        EvaluationScore score;

        // 合理性 — 基于内容长度和结构
        score.reasonableness = std::min(1.0,
            0.5 + std::min(0.4, static_cast<double>(content_length) / 500.0));

        // 可执行性
        bool has_pass = verification.find("PASS") != std::string::npos;
        bool has_structure = verification.find("Summary") != std::string::npos ||
                             verification.find("Verdict") != std::string::npos;

        score.executability = 0.6;
        if (has_pass) score.executability += 0.2;
        if (has_structure) score.executability += 0.15;
        score.executability = std::min(1.0, score.executability * executability_weight);

        // 满意度 — 基于通过率
        double pass_rate = 0.5;
        auto pass_pos = verification.find("checks passed");
        if (pass_pos != std::string::npos) {
            size_t start = pass_pos > 10 ? pass_pos - 10 : 0;
            std::string context_str = verification.substr(start, pass_pos - start + 20);
            try {
                auto slash = context_str.find('/');
                if (slash != std::string::npos) {
                    double passed = std::stod(context_str.substr(0, slash));
                    double total = std::stod(context_str.substr(slash + 1));
                    if (total > 0) pass_rate = passed / total;
                }
            } catch (...) {}
        }

        score.satisfaction = std::min(1.0,
            (0.3 + 0.5 * pass_rate) * satisfaction_weight);

        // 应用阈值
        score.reasonableness = std::max(0.0, std::min(1.0, score.reasonableness));
        score.executability = std::max(0.0, std::min(1.0, score.executability));
        score.satisfaction = std::max(0.0, std::min(1.0, score.satisfaction));

        result.score = score;

        // 生成评分报告
        std::ostringstream oss;
        oss << "Evaluation Score:\n";
        oss << "=================\n";
        oss << "Reasonableness:  " << std::fixed << std::setprecision(2) << score.reasonableness << "\n";
        oss << "Executability:   " << std::fixed << std::setprecision(2) << score.executability << "\n";
        oss << "Satisfaction:    " << std::fixed << std::setprecision(2) << score.satisfaction << "\n";
        oss << "Overall:         " << std::fixed << std::setprecision(2) << score.overall() << "\n";
        oss << "Threshold:       " << std::fixed << std::setprecision(2) << rule.threshold << "\n";
        oss << "\nVerdict: " << (score.overall() >= rule.threshold ? "SATISFIED" : "NEEDS_IMPROVEMENT");

        result.content = oss.str();
        result.metadata["overall_score"] = std::to_string(score.overall());
        result.metadata["reasonableness"] = std::to_string(score.reasonableness);
        result.metadata["executability"] = std::to_string(score.executability);
        result.metadata["satisfaction"] = std::to_string(score.satisfaction);

        return result;
    }
};

REGISTER_AGENT_PLUGIN(EvaluatorPlugin,
    "agent.evaluator",
    "Result Evaluator",
    "CLMA Core",
    "Evaluates solution quality and generates multi-dimensional scores",
    AgentStep::EVALUATOR)

} // namespace clma
