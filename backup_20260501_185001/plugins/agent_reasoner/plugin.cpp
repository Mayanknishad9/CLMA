#include <core/AgentPlugin.hpp>
#include <sstream>

namespace clma {

/**
 * Reasoner 插件 — 生成推理方案
 * 原 Orchestrator::reasonSolution
 */
class ReasonerPlugin : public AgentPlugin {
public:
    PluginInfo getInfo() const override {
        PluginInfo info;
        info.id = "agent.reasoner";
        info.name = "Solution Reasoner";
        info.version = {1, 0, 0};
        info.type = PluginType::CUSTOM;
        info.author = "CLMA Core";
        info.description = "Generates reasoning plans for refined queries";
        info.apiVersion = 1;
        return info;
    }

    AgentStep getStepType() const override {
        return AgentStep::REASONER;
    }

    AgentResult execute(const AgentContext& ctx) override {
        AgentResult result;
        result.success = true;

        const std::string& query = ctx.refinedQuery;
        const Rule& rule = ctx.currentRule;

        std::ostringstream oss;
        oss << "Reasoning Plan for: " << query << "\n\n";
        oss << "Steps:\n";

        bool has_programming = query.find("write") != std::string::npos ||
                               query.find("implement") != std::string::npos ||
                               query.find("code") != std::string::npos ||
                               query.find("script") != std::string::npos;

        bool has_data = query.find("data") != std::string::npos ||
                        query.find("analyze") != std::string::npos;

        bool has_explain = query.find("explain") != std::string::npos ||
                           query.find("describe") != std::string::npos;

        int step_num = 1;

        if (has_programming || has_data) {
            oss << step_num++ << ". Analyze requirements and identify input/output\n";
            oss << step_num++ << ". Design algorithm or data pipeline\n";
            oss << step_num++ << ". Implement solution\n";
            oss << step_num++ << ". Test with sample cases\n";
        } else if (has_explain) {
            oss << step_num++ << ". Identify key concepts to explain\n";
            oss << step_num++ << ". Structure explanation with examples\n";
            oss << step_num++ << ". Provide clear summary\n";
        } else {
            oss << step_num++ << ". Parse and understand the query\n";
            oss << step_num++ << ". Determine the best approach\n";
            oss << step_num++ << ". Generate structured response\n";
        }

        // 使用 validation_method 作为规则检查提示
        if (!rule.validation_method.empty() && rule.validation_method != "basic") {
            oss << step_num++ << ". Apply validation: " << rule.validation_method << "\n";
        }

        result.content = oss.str();
        result.metadata["reasoning_steps"] = std::to_string(step_num - 1);

        return result;
    }
};

REGISTER_AGENT_PLUGIN(ReasonerPlugin,
    "agent.reasoner",
    "Solution Reasoner",
    "CLMA Core",
    "Generates reasoning plans and analysis for refined queries",
    AgentStep::REASONER)

} // namespace clma
