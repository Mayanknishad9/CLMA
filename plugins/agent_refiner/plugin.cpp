#include <core/AgentPlugin.hpp>
#include <sstream>

namespace clma {

/**
 * Refiner 插件 — 精炼用户查询
 * 原 Orchestrator::refineQuery
 */
class RefinerPlugin : public AgentPlugin {
public:
    PluginInfo getInfo() const override {
        PluginInfo info;
        info.id = "agent.refiner";
        info.name = "Query Refiner";
        info.version = {1, 0, 0};
        info.type = PluginType::CUSTOM;
        info.author = "CLMA Core";
        info.description = "Refines user queries by extracting key requirements";
        info.apiVersion = 1;
        return info;
    }

    AgentStep getStepType() const override {
        return AgentStep::REFINER;
    }

    AgentResult execute(const AgentContext& ctx) override {
        AgentResult result;
        result.success = true;

        const std::string& query = ctx.userQuery;
        const Rule& rule = ctx.currentRule;

        // 精炼逻辑
        std::string refined = query;

        // 1. 去除引号
        if (!refined.empty() && (refined[0] == '\'' || refined[0] == '"')) {
            refined = refined.substr(1);
        }
        if (!refined.empty() && (refined.back() == '\'' || refined.back() == '"')) {
            refined.pop_back();
        }

        // 2. 检测语言并添加标签
        std::string detected_lang;
        if (refined.find("python") != std::string::npos ||
            refined.find("Python") != std::string::npos) {
            detected_lang = "python";
        } else if (refined.find("c++") != std::string::npos ||
                   refined.find("C++") != std::string::npos ||
                   refined.find("cpp") != std::string::npos) {
            detected_lang = "cpp";
        } else if (refined.find("bash") != std::string::npos ||
                   refined.find("shell") != std::string::npos) {
            detected_lang = "bash";
        }

        if (!detected_lang.empty()) {
            refined = "[" + detected_lang + "] " + refined;
        }

        // 3. 应用规则的验证方法作为精炼提示（如果存在规则匹配）
        if (!rule.validation_method.empty() && rule.validation_method != "basic") {
            refined = "[validate:" + rule.validation_method + "] " + refined;
        }

        result.content = refined;
        result.metadata["detected_language"] = detected_lang;

        return result;
    }
};

REGISTER_AGENT_PLUGIN(RefinerPlugin,
    "agent.refiner",
    "Query Refiner",
    "CLMA Core",
    "Refines user queries by extracting key requirements and applying domain-specific rules",
    AgentStep::REFINER)

} // namespace clma
