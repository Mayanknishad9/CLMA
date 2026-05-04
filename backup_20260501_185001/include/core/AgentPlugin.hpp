#ifndef CLM_AGENT_PLUGIN_HPP
#define CLM_AGENT_PLUGIN_HPP

#include "core/PluginInterface.hpp"
#include "core/Types.hpp"
#include <string>
#include <any>

namespace clma {

// 智能体执行的上下文
struct AgentContext {
    std::string userQuery;           // 原始用户查询
    std::string refinedQuery;        // 精炼后的查询（供后续阶段使用）
    Rule currentRule;                // 当前规则
    std::string previousResult;      // 前一阶段的结果（推理/执行/验证结果）
    std::vector<std::pair<std::string, AgentResult>> history;  // 完整执行历史
    std::map<std::string, size_t> statistics;  // 当前统计信息
    size_t iterationIndex = 0;       // 当前迭代次数
};

// Agent 插件类型枚举（对应 Orchestrator 的 5 个步骤）
enum class AgentStep {
    REFINER,     // refineQuery
    REASONER,    // reasonSolution
    SOLVER,      // executeSolution
    VERIFIER,    // verifySolution
    EVALUATOR    // evaluateResult
};

// Agent 插件接口 — 每个步骤都实现此接口
class AgentPlugin : public PluginInterface {
public:
    ~AgentPlugin() override;

    // === Agent 核心 ===
    // 输入上下文，输出 AgentResult
    virtual AgentResult execute(const AgentContext& context) = 0;

    // 获取此插件对应的步骤类型
    virtual AgentStep getStepType() const = 0;

    // RTTI 替代：标识这是一个 AgentPlugin
    bool isAgentPlugin() const override;

    // === PluginInterface 实现 ===
    PluginInfo getInfo() const override = 0;
    bool initialize(std::any config = {}) override;
    bool start() override;
    void stop() override;
    void shutdown() override;
    PluginState getState() const override;
    bool isHealthy() const override;
    bool configure(const std::string& key, std::any value) override;
    std::any getConfig(const std::string& key) const override;
    std::string getLastError() const override;
    void clearError() override;
    void setEventListener(PluginEventListener* listener) override;

protected:
    void setLastError(const std::string& err) { lastError_ = err; }
    PluginEventListener* getListener() const { return listener_; }

    PluginState state_ = PluginState::UNLOADED;
    std::string lastError_;
    PluginEventListener* listener_ = nullptr;
};

// 插件注册宏 — 每个 AgentPlugin .so 文件只需调用
#define REGISTER_AGENT_PLUGIN(PluginClass, PluginIdStr, PluginNameStr,       \
                               PluginAuthorStr, PluginDescStr, StepType)      \
extern "C" {                                                                   \
    ::clma::PluginInterface* createPlugin() { return new PluginClass(); }      \
    void destroyPlugin(::clma::PluginInterface* p) { delete p; }              \
    ::clma::PluginInfo getPluginInfo() {                                       \
        ::clma::PluginInfo info;                                               \
        info.id = PluginIdStr;                                                 \
        info.name = PluginNameStr;                                             \
        info.version = {1, 0, 0};                                              \
        info.type = ::clma::PluginType::CUSTOM;                                \
        info.author = PluginAuthorStr;                                         \
        info.description = PluginDescStr;                                      \
        info.apiVersion = 1;                                                   \
        return info;                                                           \
    }                                                                          \
}

} // namespace clma

#endif // CLM_AGENT_PLUGIN_HPP
