#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/functional.h>

#include "core/Types.hpp"
#include "core/RuleEngine.hpp"
#include "core/TokenMonitor.hpp"
#include "core/LoopController.hpp"
#include "core/Orchestrator.hpp"
#include "core/PluginManager.hpp"
#include "core/AgentPlugin.hpp"

namespace py = pybind11;
namespace clma {

// ---------- 转发声明：用于绑定内部访问 ----------
// Orchestrator 的 iterate() 方法需要能获取 getExecutionHistory()

} // namespace clma

PYBIND11_MODULE(clma_core, m) {
    m.doc() = "Closed-Loop Multi-Agent Core Engine Python Bindings";
    m.attr("__version__") = "0.1.0";

    // ============================================================
    // EvaluationScore
    // ============================================================
    py::class_<clma::EvaluationScore>(m, "EvaluationScore",
        "三维评估分数：合理度、可执行度、满意度")
        .def(py::init<>())
        .def(py::init<double, double, double>(),
             py::arg("reasonableness"), py::arg("executability"), py::arg("satisfaction"))
        .def_readwrite("reasonableness", &clma::EvaluationScore::reasonableness, "合理度 (0-1)")
        .def_readwrite("executability", &clma::EvaluationScore::executability, "可执行度 (0-1)")
        .def_readwrite("satisfaction", &clma::EvaluationScore::satisfaction, "满意度 (0-1)")
        .def("overall", &clma::EvaluationScore::overall, "综合得分（三维平均）")
        .def("meets_threshold", &clma::EvaluationScore::meetsThreshold,
             py::arg("threshold"), "检查是否满足阈值")
        .def("__repr__", [](const clma::EvaluationScore& s) {
            return "<EvaluationScore R=" + std::to_string(s.reasonableness) +
                   " E=" + std::to_string(s.executability) +
                   " S=" + std::to_string(s.satisfaction) +
                   " O=" + std::to_string(s.overall()) + ">";
        })
        .def("__str__", [](const clma::EvaluationScore& s) {
            return "EvaluationScore(reasonableness=" + std::to_string(s.reasonableness) +
                   ", executability=" + std::to_string(s.executability) +
                   ", satisfaction=" + std::to_string(s.satisfaction) +
                   ", overall=" + std::to_string(s.overall()) + ")";
        });

    // ============================================================
    // CandidateConfig — 并行候选生成配置
    // ============================================================
    py::class_<clma::CandidateConfig>(m, "CandidateConfig",
        "并行候选生成配置：候选数、启用开关、critic 保留比例、早停参数")
        .def(py::init<>())
        .def_readwrite("num_candidates", &clma::CandidateConfig::num_candidates, "并行候选数")
        .def_readwrite("enabled", &clma::CandidateConfig::enabled, "启用并行候选")
        .def_readwrite("critic_keep_ratio", &clma::CandidateConfig::critic_keep_ratio, "critic 保留比例")
        .def_readwrite("early_stop_epsilon", &clma::CandidateConfig::early_stop_epsilon, "收敛判定阈值")
        .def_readwrite("early_stop_lookback", &clma::CandidateConfig::early_stop_lookback, "连续N次改进小于epsilon则早停")
        .def("__repr__", [](const clma::CandidateConfig& c) {
            return "<CandidateConfig enabled=" + std::to_string(c.enabled) +
                   " N=" + std::to_string(c.num_candidates) + ">";
        });

    // ============================================================
    // DAGConfig — DAG 规划配置
    // ============================================================
    py::class_<clma::DAGConfig>(m, "DAGConfig",
        "DAG 规划配置：启用开关、最大子任务数、自动降级")
        .def(py::init<>())
        .def_readwrite("enabled", &clma::DAGConfig::enabled, "启用 DAG 模式")
        .def_readwrite("max_subtasks", &clma::DAGConfig::max_subtasks, "最大子任务数")
        .def_readwrite("min_subtasks_to_enable", &clma::DAGConfig::min_subtasks_to_enable, "最小子任务数才启用 DAG")
        .def_readwrite("auto_downgrade", &clma::DAGConfig::auto_downgrade, "自动降级线性")
        .def("__repr__", [](const clma::DAGConfig& d) {
            return "<DAGConfig enabled=" + std::to_string(d.enabled) +
                   " max=" + std::to_string(d.max_subtasks) + ">";
        });

    // ============================================================
    // AgentResult
    // ============================================================
    py::class_<clma::AgentResult>(m, "AgentResult", "智能体执行结果")
        .def(py::init<>())
        .def_readwrite("content", &clma::AgentResult::content, "结果内容")
        .def_readwrite("metadata", &clma::AgentResult::metadata, "元数据")
        .def_readwrite("score", &clma::AgentResult::score, "评估分数")
        .def_readwrite("success", &clma::AgentResult::success, "是否成功")
        .def_readwrite("error_message", &clma::AgentResult::error_message, "错误信息")
        .def("__repr__", [](const clma::AgentResult& r) {
            return "<AgentResult success=" + std::string(r.success ? "true" : "false") +
                   " content_len=" + std::to_string(r.content.length()) + ">";
        })
        .def("__str__", [](const clma::AgentResult& r) {
            return "AgentResult(success=" + std::string(r.success ? "true" : "false") +
                   ", content='" + r.content.substr(0, 100) + "...', error='" +
                   r.error_message + "')";
        });

    // ============================================================
    // Rule
    // ============================================================
    py::class_<clma::Rule>(m, "Rule", "规则定义")
        .def(py::init<>())
        .def_readwrite("pattern", &clma::Rule::pattern, "匹配模式")
        .def_readwrite("validation_method", &clma::Rule::validation_method, "验证方法")
        .def_readwrite("recommended_tools", &clma::Rule::recommended_tools, "推荐工具")
        .def_readwrite("weights", &clma::Rule::weights, "评分权重")
        .def_readwrite("threshold", &clma::Rule::threshold, "阈值")
        .def("matches", &clma::Rule::matches, py::arg("query"), "检查查询是否匹配规则")
        .def("__repr__", [](const clma::Rule& r) {
            return "<Rule pattern='" + r.pattern + "' threshold=" +
                   std::to_string(r.threshold) + ">";
        });

    // ============================================================
    // AgentType 枚举
    // ============================================================
    py::enum_<clma::AgentType>(m, "AgentType", "智能体类型")
        .value("REFINER", clma::AgentType::REFINER, "精炼智能体")
        .value("REASONER", clma::AgentType::REASONER, "推理智能体")
        .value("SOLVER", clma::AgentType::SOLVER, "解决智能体")
        .value("VERIFIER", clma::AgentType::VERIFIER, "验证智能体")
        .value("EVALUATOR", clma::AgentType::EVALUATOR, "评估智能体")
        .export_values();

    // ============================================================
    // AgentState 枚举
    // ============================================================
    py::enum_<clma::AgentState>(m, "AgentState", "智能体状态")
        .value("IDLE", clma::AgentState::IDLE, "空闲")
        .value("PROCESSING", clma::AgentState::PROCESSING, "处理中")
        .value("SUCCESS", clma::AgentState::SUCCESS, "成功")
        .value("FAILED", clma::AgentState::FAILED, "失败")
        .export_values();

    // ============================================================
    // TokenUsage
    // ============================================================
    py::class_<clma::TokenUsage>(m, "TokenUsage", "Token 使用记录")
        .def(py::init<>())
        .def(py::init<size_t, size_t, const std::string&, const std::string&>(),
             py::arg("prompt_tokens"), py::arg("completion_tokens"),
             py::arg("agent_type"), py::arg("operation"))
        .def_readwrite("prompt_tokens", &clma::TokenUsage::prompt_tokens, "提示 Token 数")
        .def_readwrite("completion_tokens", &clma::TokenUsage::completion_tokens, "补全 Token 数")
        .def_readwrite("total_tokens", &clma::TokenUsage::total_tokens, "总 Token 数")
        .def_readwrite("agent_type", &clma::TokenUsage::agent_type, "智能体类型")
        .def_readwrite("operation", &clma::TokenUsage::operation, "操作名称")
        .def("__repr__", [](const clma::TokenUsage& t) {
            return "<TokenUsage total=" + std::to_string(t.total_tokens) +
                   " agent=" + t.agent_type + " op=" + t.operation + ">";
        });

    // ============================================================
    // RuleEngine
    // ============================================================
    py::class_<clma::RuleEngine, std::shared_ptr<clma::RuleEngine>>(m, "RuleEngine", "规则引擎")
        .def(py::init<>())
        .def("load_rules_from_file", &clma::RuleEngine::loadRulesFromFile,
             py::arg("filepath"), "从 YAML 文件加载规则")
        .def("load_rules_from_string", &clma::RuleEngine::loadRulesFromString,
             py::arg("yaml_content"), "从 YAML 字符串加载规则")
        .def("find_matching_rules", &clma::RuleEngine::findMatchingRules,
             py::arg("query"), "查找匹配的规则")
        .def("get_best_rule", &clma::RuleEngine::getBestRule,
             py::arg("query"), "获取最佳匹配规则")
        .def("get_all_rules", &clma::RuleEngine::getAllRules,
             py::return_value_policy::reference_internal, "获取所有规则")
        .def("add_rule", &clma::RuleEngine::addRule,
             py::arg("rule"), "添加规则")
        .def("clear_rules", &clma::RuleEngine::clearRules, "清空规则")
        .def("rule_count", &clma::RuleEngine::ruleCount, "规则数量")
        .def("__repr__", [](const clma::RuleEngine& e) {
            return "<RuleEngine rules=" + std::to_string(e.ruleCount()) + ">";
        });

    // ============================================================
    // TokenMonitor
    // ============================================================
    py::class_<clma::TokenMonitor, std::shared_ptr<clma::TokenMonitor>>(m, "TokenMonitor", "Token 监控器")
        .def(py::init<size_t>(), py::arg("total_budget") = 10000)
        .def("record_usage",
             py::overload_cast<const clma::TokenUsage&>(&clma::TokenMonitor::recordUsage),
             py::arg("usage"), "记录 Token 使用")
        .def("record_usage_detailed",
             py::overload_cast<size_t, size_t, const std::string&, const std::string&>(
                 &clma::TokenMonitor::recordUsage),
             py::arg("prompt_tokens"), py::arg("completion_tokens"),
             py::arg("agent_type"), py::arg("operation"),
             "详细记录 Token 使用")
        .def("is_over_budget", &clma::TokenMonitor::isOverBudget, "是否超预算")
        .def("get_remaining_budget", &clma::TokenMonitor::getRemainingBudget, "获取剩余预算")
        .def("get_total_used", &clma::TokenMonitor::getTotalUsed, "获取总使用量")
        .def("get_usage_by_agent", &clma::TokenMonitor::getUsageByAgent, "按智能体统计")
        .def("get_usage_by_operation", &clma::TokenMonitor::getUsageByOperation, "按操作统计")
        .def("get_recent_usage", &clma::TokenMonitor::getRecentUsage,
             py::arg("limit") = 10, "获取最近使用记录")
        .def("suggest_truncation_length", &clma::TokenMonitor::suggestTruncationLength,
             "建议截断长度")
        .def("needs_warning", &clma::TokenMonitor::needsWarning, "是否需要警告")
        .def("reset", &clma::TokenMonitor::reset, "重置监控器")
        .def("set_budget", &clma::TokenMonitor::setBudget, py::arg("budget"), "设置预算")
        .def("get_budget", &clma::TokenMonitor::getBudget, "获取预算")
        .def("get_usage_ratio", &clma::TokenMonitor::getUsageRatio, "获取使用率")
        .def("__repr__", [](const clma::TokenMonitor& m) {
            return "<TokenMonitor used=" + std::to_string(m.getTotalUsed()) +
                   "/" + std::to_string(m.getBudget()) + ">";
        });

    // ============================================================
    // LoopController
    // ============================================================
    py::class_<clma::LoopController, std::shared_ptr<clma::LoopController>> loop_controller(m, "LoopController", "循环控制器");

    py::enum_<clma::LoopController::Mode>(loop_controller, "Mode", "工作模式")
        .value("CLOSED_LOOP", clma::LoopController::Mode::CLOSED_LOOP, "闭环模式")
        .value("OPEN_LOOP", clma::LoopController::Mode::OPEN_LOOP, "开环模式")
        .export_values();

    loop_controller
        .def(py::init<>())
        .def("set_mode", &clma::LoopController::setMode, py::arg("mode"), "设置模式")
        .def("get_mode", &clma::LoopController::getMode, "获取模式")
        .def("add_iteration_result", &clma::LoopController::addIterationResult,
             py::arg("score"), py::arg("token_usage"), "添加迭代结果")
        .def("should_continue", &clma::LoopController::shouldContinue, "是否应继续")
        .def("should_stop", &clma::LoopController::shouldStop, "是否应停止")
        .def("get_iteration_count", &clma::LoopController::getIterationCount, "获取迭代次数")
        .def("reset", &clma::LoopController::reset, "重置控制器")
        .def("set_max_iterations", &clma::LoopController::setMaxIterations,
             py::arg("max_iterations"), "设置最大迭代次数")
        .def("set_satisfaction_threshold", &clma::LoopController::setSatisfactionThreshold,
             py::arg("threshold"), "设置满意度阈值")
        .def("get_best_score", &clma::LoopController::getBestScore, "获取最佳分数")
        .def("get_total_token_usage", &clma::LoopController::getTotalTokenUsage, "获取总 Token 使用")
        .def("get_average_score", &clma::LoopController::getAverageScore, "获取平均分数")
        .def("has_converged", &clma::LoopController::hasConverged,
             py::arg("lookback") = 3, py::arg("epsilon") = 0.01, "检查是否收敛")
        .def("get_iteration_history", &clma::LoopController::getIterationHistory,
             py::return_value_policy::reference_internal, "获取迭代历史")
        .def("apply_token_optimization", &clma::LoopController::applyTokenOptimization,
             "应用 Token 优化")
        .def("set_token_budget", &clma::LoopController::setTokenBudget,
             py::arg("budget"), "设置 Token 预算")
        .def("get_remaining_token_budget", &clma::LoopController::getRemainingTokenBudget,
             "获取剩余 Token 预算")
        .def("set_timeout_seconds", &clma::LoopController::setTimeoutSeconds,
             py::arg("seconds"), "设置超时秒数")
        .def("is_timeout", &clma::LoopController::isTimeout, "检查是否超时")
        .def("__repr__", [](const clma::LoopController& c) {
            std::string mode = c.getMode() == clma::LoopController::Mode::CLOSED_LOOP
                ? "CLOSED" : "OPEN";
            return "<LoopController mode=" + mode +
                   " iter=" + std::to_string(c.getIterationCount()) + ">";
        });

    // ============================================================
    // PluginVersion
    // ============================================================
    py::class_<clma::PluginVersion>(m, "PluginVersion", "插件版本号")
        .def(py::init<>())
        .def(py::init<int, int, int>(),
             py::arg("major"), py::arg("minor"), py::arg("patch"))
        .def_readwrite("major", &clma::PluginVersion::major, "主版本号")
        .def_readwrite("minor", &clma::PluginVersion::minor, "次版本号")
        .def_readwrite("patch", &clma::PluginVersion::patch, "修订号")
        .def("to_string", &clma::PluginVersion::toString, "版本字符串")
        .def("__repr__", [](const clma::PluginVersion& v) {
            return "<PluginVersion " + v.toString() + ">";
        })
        .def("__str__", [](const clma::PluginVersion& v) {
            return v.toString();
        });

    // ============================================================
    // PluginType 枚举
    // ============================================================
    py::enum_<clma::PluginType>(m, "PluginType", "插件类型")
        .value("TOOL", clma::PluginType::TOOL, "工具插件")
        .value("STRATEGY", clma::PluginType::STRATEGY, "策略插件")
        .value("JUDGE", clma::PluginType::JUDGE, "评估插件")
        .value("PROVIDER", clma::PluginType::PROVIDER, "提供商插件")
        .value("CUSTOM", clma::PluginType::CUSTOM, "自定义插件")
        .export_values();

    // ============================================================
    // PluginState 枚举
    // ============================================================
    py::enum_<clma::PluginState>(m, "PluginState", "插件状态")
        .value("UNLOADED", clma::PluginState::UNLOADED, "未加载")
        .value("LOADED", clma::PluginState::LOADED, "已加载")
        .value("INITIALIZED", clma::PluginState::INITIALIZED, "已初始化")
        .value("RUNNING", clma::PluginState::RUNNING, "运行中")
        .value("ERROR", clma::PluginState::ERROR, "错误")
        .value("UNLOADING", clma::PluginState::UNLOADING, "卸载中")
        .export_values();

    // ============================================================
    // PluginInfo
    // ============================================================
    py::class_<clma::PluginInfo>(m, "PluginInfo", "插件元数据")
        .def(py::init<>())
        .def_readwrite("id", &clma::PluginInfo::id, "唯一标识符")
        .def_readwrite("name", &clma::PluginInfo::name, "人类可读名称")
        .def_readwrite("version", &clma::PluginInfo::version, "版本号")
        .def_readwrite("type", &clma::PluginInfo::type, "插件类型")
        .def_readwrite("author", &clma::PluginInfo::author, "作者")
        .def_readwrite("description", &clma::PluginInfo::description, "描述")
        .def_readwrite("dependencies", &clma::PluginInfo::dependencies, "依赖的插件ID列表")
        .def_readwrite("license", &clma::PluginInfo::license, "许可证")
        .def_readwrite("api_version", &clma::PluginInfo::apiVersion, "插件API版本")
        .def("__repr__", [](const clma::PluginInfo& i) {
            return "<PluginInfo id='" + i.id + "' v=" + i.version.toString() + ">";
        })
        .def("__str__", [](const clma::PluginInfo& i) {
            return "PluginInfo(id=" + i.id + ", name=" + i.name +
                   ", version=" + i.version.toString() + ", type=" +
                   std::to_string(static_cast<int>(i.type)) + ")";
        });

    // ============================================================
    // PluginManager
    // ============================================================
    py::class_<clma::PluginManager, std::shared_ptr<clma::PluginManager>>(m, "PluginManager", "插件管理器")
        .def(py::init<>())
        // 目录管理
        .def("add_plugin_directory", &clma::PluginManager::addPluginDirectory,
             py::arg("path"), "添加插件扫描目录")
        .def("clear_plugin_directories", &clma::PluginManager::clearPluginDirectories,
             "清空插件扫描目录")
        .def("get_plugin_directories", &clma::PluginManager::getPluginDirectories,
             "获取插件扫描目录列表")
        // 扫描与加载
        .def("scan_plugins", &clma::PluginManager::scanPlugins,
             "扫描目录，发现可用插件（返回发现的插件数量）")
        .def("load_plugin", &clma::PluginManager::loadPlugin,
             py::arg("plugin_id"), "加载单个插件")
        .def("load_all", &clma::PluginManager::loadAll,
             "加载所有已发现的插件")
        .def("unload_plugin", &clma::PluginManager::unloadPlugin,
             py::arg("plugin_id"), "卸载单个插件（含依赖它的插件）")
        .def("unload_all", &clma::PluginManager::unloadAll,
             "卸载所有插件")
        // 生命周期
        .def("initialize_plugin", [](clma::PluginManager& pm, const std::string& plugin_id) {
                 return pm.initializePlugin(plugin_id, std::any{});
             },
             py::arg("plugin_id"), "初始化插件（无自定义配置）")
        .def("start_plugin", &clma::PluginManager::startPlugin,
             py::arg("plugin_id"), "启动插件")
        .def("stop_plugin", &clma::PluginManager::stopPlugin,
             py::arg("plugin_id"), "停止插件")
        // 查询
        .def("list_plugins", &clma::PluginManager::listPlugins,
             py::arg("type") = static_cast<clma::PluginType>(-1),
             "获取插件列表（可选按类型过滤）")
        .def("list_plugins_by_state", &clma::PluginManager::listPluginsByState,
             py::arg("state"), "按状态获取插件列表")
        .def("get_plugin_state", &clma::PluginManager::getPluginState,
             py::arg("plugin_id"), "获取插件状态")
        .def("is_plugin_loaded", &clma::PluginManager::isPluginLoaded,
             py::arg("plugin_id"), "检查插件是否已加载")
        .def("get_plugin_count", &clma::PluginManager::getPluginCount,
             "获取已加载的插件数量")
        .def("get_plugin_dependencies", &clma::PluginManager::getPluginDependencies,
             py::arg("plugin_id"), "获取插件的依赖树")
        .def("get_plugin_dependents", &clma::PluginManager::getPluginDependents,
             py::arg("plugin_id"), "获取依赖此插件的插件列表")
        // 热更新
        .def("hot_reload", &clma::PluginManager::hotReload,
             py::arg("plugin_id"), "热更新插件")
        .def("has_new_version", &clma::PluginManager::hasNewVersion,
             py::arg("plugin_id"), "检查插件是否有新版本")
        // 版本
        .def("set_min_api_version", &clma::PluginManager::setMinApiVersion,
             py::arg("version"), "设置最低兼容 API 版本")
        .def("get_min_api_version", &clma::PluginManager::getMinApiVersion,
             "获取最低兼容 API 版本")
        // 配置持久化
        .def("save_config", &clma::PluginManager::saveConfig,
             py::arg("file_path") = "plugins_config.json", "保存插件配置到文件")
        .def("load_config", &clma::PluginManager::loadConfig,
             py::arg("file_path") = "plugins_config.json", "从文件加载插件配置")
        // 错误恢复
        .def("attempt_recovery", &clma::PluginManager::attemptRecovery,
             py::arg("plugin_id"), "尝试恢复处于 ERROR 状态的插件")
        .def("__repr__", [](const clma::PluginManager& pm) {
            return "<PluginManager dirs=" + std::to_string(pm.getPluginDirectories().size()) +
                   " plugins=" + std::to_string(pm.getPluginCount()) + ">";
        });

    // ============================================================
    // Orchestrator
    // ============================================================
    py::class_<clma::Orchestrator, std::shared_ptr<clma::Orchestrator>>(m, "Orchestrator", "主编排器")
        .def(py::init<>())
        .def("register_agent", &clma::Orchestrator::registerAgent,
             py::arg("agent_type"), py::arg("callback"),
             "注册智能体回调（接收 query, method 字符串，返回 AgentResult）")
        .def("set_rule_engine", &clma::Orchestrator::setRuleEngine,
             py::arg("rule_engine"), "设置规则引擎")
        .def("set_token_monitor", &clma::Orchestrator::setTokenMonitor,
             py::arg("token_monitor"), "设置 Token 监控器")
        .def("set_loop_controller", &clma::Orchestrator::setLoopController,
             py::arg("loop_controller"), "设置循环控制器")
        .def("process_query", &clma::Orchestrator::processQuery,
             py::arg("user_query"), "处理用户查询（主入口）")
        .def("set_loop_mode", &clma::Orchestrator::setLoopMode,
             py::arg("mode"), "设置工作模式（开环/闭环）")
        .def("get_statistics", &clma::Orchestrator::getStatistics,
             "获取统计信息")
        .def("get_execution_history", &clma::Orchestrator::getExecutionHistory,
             py::return_value_policy::reference_internal, "获取执行历史")
        .def("clear_execution_history", &clma::Orchestrator::clearExecutionHistory,
             "清空执行历史（跨查询隔离）")
        .def("reset", &clma::Orchestrator::reset, "重置编排器")
        .def("set_max_iterations", &clma::Orchestrator::setMaxIterations,
             py::arg("max_iterations"), "设置最大迭代次数")
        .def("set_satisfaction_threshold", &clma::Orchestrator::setSatisfactionThreshold,
             py::arg("threshold"), "设置满意度阈值")
        .def("set_token_budget", &clma::Orchestrator::setTokenBudget,
             py::arg("budget"), "设置 Token 预算")
        .def("get_total_token_usage", &clma::Orchestrator::getTotalTokenUsage,
             "获取总 Token 使用量")
        .def("get_current_mode", &clma::Orchestrator::getCurrentMode,
             "获取当前模式")
        .def("get_plugin_manager", &clma::Orchestrator::getPluginManager,
             "获取 PluginManager 实例（可能为 None）")
        .def("register_plugin_manager", &clma::Orchestrator::registerPluginManager,
             py::arg("plugin_manager"), "注册 PluginManager")
        .def("load_plugin_agents", &clma::Orchestrator::loadPluginAgents,
             "从 PluginManager 加载 Agent 插件")
        // 并行候选生成
        .def("set_candidate_config", &clma::Orchestrator::setCandidateConfig,
             py::arg("config"), "设置并行候选配置")
        .def("get_candidate_config", &clma::Orchestrator::getCandidateConfig,
             py::return_value_policy::reference_internal, "获取并行候选配置")
        // 缓存管理
        .def("clear_cache", &clma::Orchestrator::clearCache, "清空查询缓存")
        .def("set_cache_enabled", &clma::Orchestrator::setCacheEnabled,
             py::arg("enabled"), "启用/禁用缓存")
        .def("is_cache_enabled", &clma::Orchestrator::isCacheEnabled, "缓存是否启用")
        // DAG 规划
        .def("register_planner", &clma::Orchestrator::registerPlanner,
             py::arg("callback"), "注册 Planner 回调（任务分解用）")
        .def("set_dag_mode", &clma::Orchestrator::setDagMode,
             py::arg("enabled"), "启用/禁用 DAG 模式")
        .def("is_dag_mode", &clma::Orchestrator::isDagMode, "当前是否 DAG 模式")
        .def("set_dag_config", &clma::Orchestrator::setDAGConfig,
             py::arg("config"), "设置 DAG 配置")
        .def("get_dag_config", &clma::Orchestrator::getDAGConfig,
             py::return_value_policy::reference_internal, "获取 DAG 配置")
        .def("has_dag_result", &clma::Orchestrator::hasDagResult, "是否有 DAG 结果")
        .def("get_dag_status", &clma::Orchestrator::getDagStatus, "获取 DAG 状态")
        .def("process_query_dag", &clma::Orchestrator::processQueryDag,
             py::arg("query"), "以 DAG 模式处理查询并返回结果")
        .def("__repr__", [](const clma::Orchestrator& o) {
            return "<Orchestrator agents=" + std::to_string(4) + // 固定数量展示
                   " history=" + std::to_string(o.getExecutionHistory().size()) + ">";
        });
}
