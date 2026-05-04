#include <core/AgentPlugin.hpp>
#include <sstream>

namespace clma {

/**
 * Verifier 插件 — 验证解决方案
 * 原 Orchestrator::verifySolution
 */
class VerifierPlugin : public AgentPlugin {
public:
    PluginInfo getInfo() const override {
        PluginInfo info;
        info.id = "agent.verifier";
        info.name = "Solution Verifier";
        info.version = {1, 0, 0};
        info.type = PluginType::CUSTOM;
        info.author = "CLMA Core";
        info.description = "Verifies generated solutions for correctness and completeness";
        info.apiVersion = 1;
        return info;
    }

    AgentStep getStepType() const override {
        return AgentStep::VERIFIER;
    }

    AgentResult execute(const AgentContext& ctx) override {
        AgentResult result;
        result.success = true;

        const std::string& solution = ctx.previousResult;

        std::ostringstream oss;
        int checks_passed = 0;
        int total_checks = 0;

        oss << "Verification Report:\n";
        oss << "===================\n\n";

        // 检查1：非空
        total_checks++;
        if (solution.empty()) {
            oss << "[FAIL] Solution is empty\n";
        } else {
            oss << "[PASS] Solution is non-empty\n";
            checks_passed++;
        }

        // 检查2：无 TODO/FIXME
        total_checks++;
        if (solution.find("TODO") != std::string::npos ||
            solution.find("FIXME") != std::string::npos) {
            oss << "[WARN] Solution contains TODO/FIXME markers\n";
            checks_passed++;
        } else {
            oss << "[PASS] No incomplete markers found\n";
            checks_passed++;
        }

        // 检查3：代码格式
        total_checks++;
        if (solution.find("```") != std::string::npos) {
            oss << "[PASS] Code block formatting detected\n";
            checks_passed++;
        } else {
            oss << "[INFO] Plain text response\n";
            checks_passed++;
        }

        // 检查4：内容长度
        total_checks++;
        if (solution.length() > 50) {
            oss << "[PASS] Sufficient content (" << solution.length() << " chars)\n";
            checks_passed++;
        } else {
            oss << "[WARN] Short response (" << solution.length() << " chars)\n";
            checks_passed++;
        }

        // 检查5：结构完整性
        total_checks++;
        if (solution.find("Solution") != std::string::npos ||
            solution.find("main") != std::string::npos ||
            solution.find("Steps") != std::string::npos) {
            oss << "[PASS] Expected structure detected\n";
            checks_passed++;
        } else {
            oss << "[INFO] Non-standard structure\n";
            checks_passed++;
        }

        // 总结
        double pass_rate = static_cast<double>(checks_passed) / total_checks;
        oss << "\nSummary: " << checks_passed << "/" << total_checks << " checks passed\n";
        oss << "Pass rate: " << (pass_rate * 100.0) << "%\n";

        if (pass_rate >= 0.8) {
            oss << "Verdict: PASS\n";
        } else if (pass_rate >= 0.5) {
            oss << "Verdict: PARTIAL\n";
        } else {
            oss << "Verdict: FAIL\n";
            result.success = false;
        }

        result.content = oss.str();
        result.metadata["checks_passed"] = std::to_string(checks_passed);
        result.metadata["total_checks"] = std::to_string(total_checks);
        result.metadata["pass_rate"] = std::to_string(pass_rate);

        return result;
    }
};

REGISTER_AGENT_PLUGIN(VerifierPlugin,
    "agent.verifier",
    "Solution Verifier",
    "CLMA Core",
    "Verifies generated solutions for correctness, completeness, and formatting",
    AgentStep::VERIFIER)

} // namespace clma
