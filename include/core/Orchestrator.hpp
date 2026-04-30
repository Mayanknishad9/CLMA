#ifndef CLOSED_LOOP_ORCHESTRATOR_HPP
#define CLOSED_LOOP_ORCHESTRATOR_HPP

#include "core/Types.hpp"
#include "core/RuleEngine.hpp"
#include "core/TokenMonitor.hpp"
#include "core/LoopController.hpp"
#include "core/PluginManager.hpp"
#include "core/AgentPlugin.hpp"
#include <string>
#include <memory>
#include <functional>
#include <map>
#include <unordered_map>

namespace clma {

// 智能体回调类型
using AgentCallback = std::function<AgentResult(const std::string&, const std::string&)>;

class Orchestrator {
public:
    Orchestrator();
    
    // 注册智能体回调
    void registerAgent(AgentType agent_type, AgentCallback callback);
    
    // 注册 PluginManager — 启用插件化 Agent 加载
    void registerPluginManager(std::shared_ptr<PluginManager> plugin_manager);
    
    // 获取 PluginManager（可为空，供 UI/外部访问插件状态）
    std::shared_ptr<PluginManager> getPluginManager() const;
    
    // 从 PluginManager 中自动发现并加载所有 Agent 插件
    // 扫描已加载的插件，匹配 AgentStep 类型，注册为对应的 AgentType 回调
    size_t loadPluginAgents();
    
    // 设置规则引擎
    void setRuleEngine(std::shared_ptr<RuleEngine> rule_engine);
    
    // 设置Token监控器
    void setTokenMonitor(std::shared_ptr<TokenMonitor> token_monitor);
    
    // 设置循环控制器
    void setLoopController(std::shared_ptr<LoopController> loop_controller);
    
    // 主处理函数
    AgentResult processQuery(const std::string& user_query);
    
    // 设置工作模式（开环/闭环）
    void setLoopMode(LoopController::Mode mode);
    
    // 获取统计信息
    std::map<std::string, size_t> getStatistics() const;
    
    // 获取执行历史
    const std::vector<std::pair<std::string, AgentResult>>& getExecutionHistory() const;
    
    // 重置编排器
    void reset();
    
    // 设置最大迭代次数
    void setMaxIterations(size_t max_iterations);
    
    // 设置满意度阈值
    void setSatisfactionThreshold(double threshold);
    
    // 设置Token预算
    void setTokenBudget(size_t budget);
    
    // 获取总Token使用量
    size_t getTotalTokenUsage() const;
    
    // 获取当前模式
    LoopController::Mode getCurrentMode() const;
    
    // ==================== 并行候选生成 ====================
    void setCandidateConfig(const CandidateConfig& config);
    const CandidateConfig& getCandidateConfig() const;
    
    // ==================== 缓存管理 ====================
    void clearCache();
    void setCacheEnabled(bool enabled);
    bool isCacheEnabled() const;
    
    // ==================== DAG 规划 ====================
    // 注册 Planner 回调（用于任务分解）
    void registerPlanner(AgentCallback planner_callback);
    
    // DAG 模式开关
    void setDagMode(bool enabled);
    bool isDagMode() const;
    
    // DAG 配置
    void setDAGConfig(const DAGConfig& config);
    const DAGConfig& getDAGConfig() const;
    
    // DAG 状态查询
    bool hasDagResult() const;
    std::map<std::string, std::string> getDagStatus() const;

    // DAG 模式处理（对外暴露以便 Python stream 调用）
    AgentResult processQueryDag(const std::string& user_query);

private:
    // 智能体回调映射
    std::map<AgentType, AgentCallback> agents_;
    
    // 组件
    std::shared_ptr<RuleEngine> rule_engine_;
    std::shared_ptr<TokenMonitor> token_monitor_;
    std::shared_ptr<LoopController> loop_controller_;
    
    // PluginManager — 可选，用于插件化 Agent 加载
    std::shared_ptr<PluginManager> plugin_manager_;
    
    // AgentStep → AgentType 映射（loadPluginAgents 时建立）
    std::unordered_map<AgentStep, AgentType> step_to_type_;
    
    // 执行历史
    std::vector<std::pair<std::string, AgentResult>> execution_history_;
    
    // 统计信息
    std::map<std::string, size_t> statistics_;
    
    // 私有辅助方法
    // 建立 AgentStep → AgentType 默认映射表
    void initStepToTypeMapping();
    
    // 初始化默认组件（如果未设置）
    void initializeDefaultComponents();
    
    // 执行单次迭代
    AgentResult executeIteration(const std::string& user_query, 
                                 const Rule& rule,
                                 const std::string& refined_query);
    
    // 精炼问题
    std::string refineQuery(const std::string& user_query, const Rule& rule);
    
    // 推理解决方案
    AgentResult reasonSolution(const std::string& refined_query, const Rule& rule);
    
    // 执行解决方案
    AgentResult executeSolution(const AgentResult& reasoned_solution, const Rule& rule);
    
    // 验证解决方案
    AgentResult verifySolution(const AgentResult& executed_solution, const Rule& rule);
    
    // 评估结果
    EvaluationScore evaluateResult(const AgentResult& verified_solution, 
                                  const Rule& rule);
    
    // 记录Token使用
    void recordTokenUsage(size_t prompt_tokens, size_t completion_tokens, 
                          const std::string& agent_type, const std::string& operation);
    
    // 更新统计信息
    void updateStatistics(const std::string& key, size_t value = 1);
    
    // 并行候选生成
    CandidateConfig candidate_config_;
    
    // 查询缓存
    std::unordered_map<size_t, AgentResult> query_cache_;
    bool cache_enabled_ = true;
    
    // 缓存方法
    size_t hashQuery(const std::string& q) const;
    
    // 从候选列表中选择最佳
    AgentResult selectBestCandidate(const std::vector<AgentResult>& candidates,
                                    const AgentResult& reasoned_solution,
                                    const Rule& rule);
    
    // 生成N个独立候选
    std::vector<AgentResult> generateCandidates(const std::string& refined_query,
                                                 const Rule& rule,
                                                 size_t N);
    
    // 线性迭代（当并行候选禁用时使用）
    AgentResult executeIterationLinear(const std::string& user_query,
                                       const Rule& rule,
                                       const std::string& refined_query);
    
    // 并行迭代（当并行候选启用时使用）
    AgentResult executeIterationParallel(const std::string& user_query,
                                          const Rule& rule,
                                          const std::string& refined_query);
    
    // ==================== DAG 规划私有方法 ====================
    // Planner 回调（用于任务分解）
    AgentCallback planner_callback_;
    
    // DAG 配置
    DAGConfig dag_config_;
    
    // DAG 当前任务图
    TaskGraph current_dag_;
    
    // DAG checkpoint（每个 task 执行前保存的 results）
    std::map<std::string, AgentResult> dag_checkpoints_;
    
    // DAG 是否已产生结果
    bool dag_has_result_ = false;
    
    // 执行单个 DAG chunk（复用 executeIterationLinear）
    AgentResult executeDAGChunk(const std::string& task_description,
                                const std::string& context);
    
    // 回退单个任务
    void rollbackTask(TaskNode& node);
    
    // 获取 task 的上下文（前驱 results）
    std::string getTaskContext(const TaskGraph& graph, const TaskNode& node) const;
};

} // namespace clma

#endif // CLOSED_LOOP_ORCHESTRATOR_HPP