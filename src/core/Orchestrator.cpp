#include "core/Orchestrator.hpp"
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <iostream>
#include <unordered_map>
#include <yaml-cpp/yaml.h>

namespace clma {

Orchestrator::Orchestrator() {
    initializeDefaultComponents();
    initStepToTypeMapping();
}

void Orchestrator::registerAgent(AgentType agent_type, AgentCallback callback) {
    agents_[agent_type] = callback;
}

void Orchestrator::registerPluginManager(std::shared_ptr<PluginManager> plugin_manager) {
    plugin_manager_ = std::move(plugin_manager);
}

std::shared_ptr<PluginManager> Orchestrator::getPluginManager() const {
    return plugin_manager_;
}

void Orchestrator::initStepToTypeMapping() {
    step_to_type_[AgentStep::REFINER]   = AgentType::REFINER;
    step_to_type_[AgentStep::REASONER]  = AgentType::REASONER;
    step_to_type_[AgentStep::SOLVER]    = AgentType::SOLVER;
    step_to_type_[AgentStep::VERIFIER]  = AgentType::VERIFIER;
    step_to_type_[AgentStep::EVALUATOR] = AgentType::EVALUATOR;
}

size_t Orchestrator::loadPluginAgents() {
    if (!plugin_manager_) {
        return 0;
    }
    
    size_t loaded = 0;
    
    // 获取所有已加载的插件
    auto all_plugins = plugin_manager_->listPlugins();
    
    for (const auto& info : all_plugins) {
        // 只处理 CUSTOM 类型插件（Agent 插件使用 CUSTOM 类型）
        if (info.type != PluginType::CUSTOM) {
            continue;
        }
        
        // 确保插件已加载
        if (!plugin_manager_->isPluginLoaded(info.id)) {
            continue;
        }
        
        // 初始化并启动插件（确保 instance 创建完毕）
        // 如果已经初始化/启动，这些调用会优雅地跳过
        plugin_manager_->initializePlugin(info.id);
        plugin_manager_->startPlugin(info.id);
        
        // 获取插件实例并检查是否为 AgentPlugin
        PluginInterface* plugin = plugin_manager_->getPlugin(info.id);
        if (!plugin) {
            continue;
        }
        
        // 使用 isAgentPlugin() 检查（避免跨 .so RTTI dynamic_cast 问题）
        if (!plugin->isAgentPlugin()) {
            plugin_manager_->releasePlugin(info.id);
            continue;
        }
        
        // 安全转换：通过 isAgentPlugin 确认后可以 static_cast
        AgentPlugin* agent_plugin = static_cast<AgentPlugin*>(plugin);
        AgentStep step = agent_plugin->getStepType();
        
        // 先 release，因为后续回调中会重新 get
        plugin_manager_->releasePlugin(info.id);
        
        // 查找 AgentType 映射
        auto it = step_to_type_.find(step);
        if (it == step_to_type_.end()) {
            continue;  // 未知的 step 类型，跳过
        }
        
        AgentType type = it->second;
        
        // 已注册的回调不覆盖（手动 registerAgent 优先级更高）
        if (agents_.count(type)) {
            continue;
        }
        
        // 创建回调包装
        std::string plugin_id = info.id;
        std::shared_ptr<PluginManager> pm = plugin_manager_;
        
        auto callback = [pm, plugin_id, this]
                        (const std::string& input, const std::string& method) -> AgentResult {
            PluginInterface* p = pm->getPlugin(plugin_id);
            if (!p || !p->isAgentPlugin()) {
                AgentResult err;
                err.success = false;
                err.error_message = "Agent plugin '" + plugin_id + "' not available";
                if (p) pm->releasePlugin(plugin_id);
                return err;
            }
            
            AgentPlugin* ap = static_cast<AgentPlugin*>(p);
            
            // 构建 AgentContext
            AgentContext ctx;
            ctx.userQuery = input;
            ctx.refinedQuery = input;
            ctx.previousResult = input;
            ctx.currentRule.validation_method = method;
            ctx.iterationIndex = 0;
            
            // 从 Orchestrator 注入历史和统计（如果有）
            ctx.history = execution_history_;
            ctx.statistics = statistics_;
            
            AgentResult result = ap->execute(ctx);
            
            // 提取分数元数据（Evaluator 插件会填充 score）
            if (result.metadata.count("overall_score")) {
                try {
                    EvaluationScore score;
                    auto r_it = result.metadata.find("reasonableness");
                    auto e_it = result.metadata.find("executability");
                    auto s_it = result.metadata.find("satisfaction");
                    if (r_it != result.metadata.end() &&
                        e_it != result.metadata.end() &&
                        s_it != result.metadata.end()) {
                        score.reasonableness = std::stod(r_it->second);
                        score.executability = std::stod(e_it->second);
                        score.satisfaction = std::stod(s_it->second);
                        result.score = score;
                    }
                } catch (...) {}
            }
            
            pm->releasePlugin(plugin_id);
            return result;
        };
        
        agents_[type] = callback;
        loaded++;
    }
    
    return loaded;
}

void Orchestrator::setRuleEngine(std::shared_ptr<RuleEngine> rule_engine) {
    rule_engine_ = rule_engine;
}

void Orchestrator::setTokenMonitor(std::shared_ptr<TokenMonitor> token_monitor) {
    token_monitor_ = token_monitor;
}

void Orchestrator::setLoopController(std::shared_ptr<LoopController> loop_controller) {
    loop_controller_ = loop_controller;
}

AgentResult Orchestrator::processQuery(const std::string& user_query) {
    // 不再自动清空执行历史 — 调用方通过 reset() 手动控制
    // 保留历史记录供跨查询追溯和调试

    // 缓存检查：相同查询直接返回缓存
    if (cache_enabled_) {
        size_t qhash = hashQuery(user_query);
        auto cache_it = query_cache_.find(qhash);
        if (cache_it != query_cache_.end()) {
            AgentResult cached = cache_it->second;
            cached.metadata["cached"] = "true";
            return cached;
        }
    }

    // DAG 模式分派
    if (dag_config_.enabled && planner_callback_) {
        return processQueryDag(user_query);
    }

    // 记录查询
    updateStatistics("queries_processed");
    
    // 查找匹配规则
    if (!rule_engine_) {
        initializeDefaultComponents();
    }
    
    auto rule_opt = rule_engine_->getBestRule(user_query);
    if (!rule_opt) {
        AgentResult result;
        result.success = false;
        result.error_message = "No matching rule found for query: " + user_query;
        return result;
    }
    
    Rule rule = *rule_opt;
    updateStatistics("rules_matched");
    
    // 设置循环控制器模式
    if (loop_controller_) {
        loop_controller_->reset();
    }
    
    AgentResult best_result;
    EvaluationScore best_score{0.0, 0.0, 0.0};
    double prev_score = 0.0;
    size_t stable_count = 0;
    bool early_stopped = false;
    
    // 主循环
    do {
        // 精炼问题
        std::string refined_query = refineQuery(user_query, rule);
        
        // 执行单次迭代（线性或并行模式）
        AgentResult iteration_result;
        if (candidate_config_.enabled) {
            iteration_result = executeIterationParallel(user_query, rule, refined_query);
        } else {
            iteration_result = executeIterationLinear(user_query, rule, refined_query);
        }
        
        if (!iteration_result.success) {
            best_result = iteration_result;
            break;
        }
        
        // 评估结果 — 使用验证后的输出（非覆盖后的主输出）
        AgentResult eval_input = iteration_result;
        if (eval_input.metadata.count("verified_content")) {
            eval_input.content = eval_input.metadata["verified_content"];
        }
        EvaluationScore score = evaluateResult(eval_input, rule);
        
        // 记录Token使用
        TokenUsage token_usage(
            iteration_result.metadata.count("prompt_tokens") 
                ? std::stoull(iteration_result.metadata.at("prompt_tokens")) : 0,
            iteration_result.metadata.count("completion_tokens") 
                ? std::stoull(iteration_result.metadata.at("completion_tokens")) : 0,
            "orchestrator",
            "iteration"
        );
        
        // 添加到循环控制器
        if (loop_controller_) {
            loop_controller_->addIterationResult(score, token_usage);
        }
        
        // 更新最佳结果
        if (score.overall() > best_score.overall()) {
            best_score = score;
            best_result = iteration_result;
            best_result.score = score;
            
            // 保存当前迭代的完整快照到最佳结果
            best_result.metadata["best_iteration_content"] = iteration_result.content;
            best_result.metadata["best_iteration_reasoned"] = 
                iteration_result.metadata.count("reasoned_content") ? 
                iteration_result.metadata["reasoned_content"] : "";
            best_result.metadata["best_iteration_verified"] = 
                iteration_result.metadata.count("verified_content") ? 
                iteration_result.metadata["verified_content"] : "";
        }
        
        // 记录执行历史（保留各阶段内容）
        AgentResult history_entry = iteration_result;
        history_entry.metadata["iteration_score_reasonableness"] = std::to_string(score.reasonableness);
        history_entry.metadata["iteration_score_executability"] = std::to_string(score.executability);
        history_entry.metadata["iteration_score_satisfaction"] = std::to_string(score.satisfaction);
        history_entry.metadata["iteration_score_overall"] = std::to_string(score.overall());
        history_entry.metadata["iteration_index"] = std::to_string(
            loop_controller_ ? loop_controller_->getIterationCount() : execution_history_.size()
        );
        execution_history_.emplace_back(user_query, history_entry);
        
        // 检查是否满足阈值
        if (score.meetsThreshold(rule.threshold)) {
            updateStatistics("iterations_successful");
            break;
        }

        // 提前终止检测：连续N次迭代改进小于epsilon
        double improvement = std::abs(score.overall() - prev_score);
        if (improvement < candidate_config_.early_stop_epsilon) {
            stable_count++;
        } else {
            stable_count = 0;
        }
        if (stable_count >= candidate_config_.early_stop_lookback) {
            early_stopped = true;
            best_result.metadata["early_stopped"] = "true";
            best_result.metadata["early_stop_reason"] =
                "Stable for " + std::to_string(stable_count) +
                " iterations (improvement < " + std::to_string(candidate_config_.early_stop_epsilon) + ")";
            break;
        }
        prev_score = score.overall();

        // 应用Token优化（如果接近预算）
        if (token_monitor_ && token_monitor_->needsWarning()) {
            token_monitor_->suggestTruncationLength();
            // 可以在这里触发优化策略
        }
        
    } while (loop_controller_ && loop_controller_->shouldContinue());
    
    // 设置最终结果
    if (loop_controller_) {
        best_result.metadata["total_iterations"] = std::to_string(loop_controller_->getIterationCount());
        best_result.metadata["total_token_usage"] = std::to_string(loop_controller_->getTotalTokenUsage());
        best_result.metadata["final_score_reasonableness"] = std::to_string(best_score.reasonableness);
        best_result.metadata["final_score_executability"] = std::to_string(best_score.executability);
        best_result.metadata["final_score_satisfaction"] = std::to_string(best_score.satisfaction);
        best_result.metadata["final_score_overall"] = std::to_string(best_score.overall());
        
        // 记录并行候选模式
        if (candidate_config_.enabled) {
            best_result.metadata["parallel_candidates"] = std::to_string(candidate_config_.num_candidates);
        }

        // 记录是否缓存
        
        // 记录各轮迭代的分数
        size_t idx = 0;
        for (const auto& [score, token] : loop_controller_->getIterationHistory()) {
            best_result.metadata["iter_" + std::to_string(idx) + "_score"] = 
                std::to_string(score.overall());
            best_result.metadata["iter_" + std::to_string(idx) + "_reasonableness"] = 
                std::to_string(score.reasonableness);
            best_result.metadata["iter_" + std::to_string(idx) + "_executability"] = 
                std::to_string(score.executability);
            best_result.metadata["iter_" + std::to_string(idx) + "_satisfaction"] = 
                std::to_string(score.satisfaction);
            idx++;
        }
        best_result.metadata["iteration_count"] = std::to_string(idx);
        
        if (!best_score.meetsThreshold(rule.threshold)) {
            best_result.success = false;
            best_result.error_message = "Threshold not met after " + 
                                       std::to_string(loop_controller_->getIterationCount()) + 
                                       " iterations. Best score: " + 
                                       std::to_string(best_score.overall()) + 
                                       " (threshold: " + std::to_string(rule.threshold) + ")";
        }
    }
    
    updateStatistics("processes_completed");

    // 缓存结果
    if (cache_enabled_ && best_result.success) {
        size_t qhash = hashQuery(user_query);
        // 只缓存成功结果，不覆盖已有缓存
        if (query_cache_.find(qhash) == query_cache_.end()) {
            query_cache_[qhash] = best_result;
        }
    }

    return best_result;
}

void Orchestrator::setLoopMode(LoopController::Mode mode) {
    if (loop_controller_) {
        loop_controller_->setMode(mode);
    }
}

std::map<std::string, size_t> Orchestrator::getStatistics() const {
    return statistics_;
}

const std::vector<std::pair<std::string, AgentResult>>& Orchestrator::getExecutionHistory() const {
    return execution_history_;
}

void Orchestrator::reset() {
    execution_history_.clear();
    statistics_.clear();
    if (token_monitor_) {
        token_monitor_->reset();
    }
    if (loop_controller_) {
        loop_controller_->reset();
    }
}

void Orchestrator::setMaxIterations(size_t max_iterations) {
    if (loop_controller_) {
        loop_controller_->setMaxIterations(max_iterations);
    }
}

void Orchestrator::setSatisfactionThreshold(double threshold) {
    if (loop_controller_) {
        loop_controller_->setSatisfactionThreshold(threshold);
    }
}

void Orchestrator::setTokenBudget(size_t budget) {
    if (token_monitor_) {
        token_monitor_->setBudget(budget);
    }
}

size_t Orchestrator::getTotalTokenUsage() const {
    if (token_monitor_) {
        return token_monitor_->getTotalUsed();
    }
    return 0;
}

LoopController::Mode Orchestrator::getCurrentMode() const {
    if (loop_controller_) {
        return loop_controller_->getMode();
    }
    return LoopController::Mode::CLOSED_LOOP;
}

void Orchestrator::initializeDefaultComponents() {
    if (!rule_engine_) {
        rule_engine_ = std::make_shared<RuleEngine>();
    }
    
    if (!token_monitor_) {
        token_monitor_ = std::make_shared<TokenMonitor>(10000);
    }
    
    if (!loop_controller_) {
        loop_controller_ = std::make_shared<LoopController>();
    }
}

// ==================== 并行候选生成方法 ====================

void Orchestrator::setCandidateConfig(const CandidateConfig& config) {
    candidate_config_ = config;
}

const CandidateConfig& Orchestrator::getCandidateConfig() const {
    return candidate_config_;
}

void Orchestrator::clearCache() {
    query_cache_.clear();
}

void Orchestrator::setCacheEnabled(bool enabled) {
    cache_enabled_ = enabled;
}

bool Orchestrator::isCacheEnabled() const {
    return cache_enabled_;
}

size_t Orchestrator::hashQuery(const std::string& q) const {
    // 简单的字符串哈希（djb2变体）
    size_t hash = 5381;
    for (char c : q) {
        hash = ((hash << 5) + hash) + static_cast<size_t>(c);
    }
    return hash;
}

std::vector<AgentResult> Orchestrator::generateCandidates(
    const std::string& refined_query,
    const Rule& rule,
    size_t N) {
    std::vector<AgentResult> candidates;
    candidates.reserve(N);
    
    for (size_t i = 0; i < N; ++i) {
        // 使用 reasonSolution + executeSolution 生成一个候选
        // 每个候选获得独立的推理和执行（模拟并行）
        AgentResult reasoned = reasonSolution(refined_query, rule);
        if (!reasoned.success) {
            // 跳过失败的候选
            continue;
        }
        AgentResult executed = executeSolution(reasoned, rule);
        if (!executed.success) {
            continue;
        }
        // 标记候选来源
        executed.metadata["candidate_index"] = std::to_string(i);
        executed.metadata["candidate_reasoned"] = reasoned.content;
        candidates.push_back(executed);
    }
    
    return candidates;
}

AgentResult Orchestrator::selectBestCandidate(
    const std::vector<AgentResult>& candidates,
    const AgentResult& reasoned_solution,
    const Rule& rule) {
    if (candidates.empty()) {
        AgentResult err;
        err.success = false;
        err.error_message = "No candidates generated";
        return err;
    }
    
    // 快速验证每个候选
    struct ScoredCandidate {
        size_t index;
        double score;
    };
    std::vector<ScoredCandidate> scored;
    scored.reserve(candidates.size());
    
    for (size_t i = 0; i < candidates.size(); ++i) {
        AgentResult verified = verifySolution(candidates[i], rule);
        double s = 0.5; // 默认中等分数
        if (verified.success) {
            // 简单启发式评分：内容长度适中加分
            size_t len = verified.content.length();
            s = 0.6 + std::min(0.3, len / 200.0);
        }
        scored.push_back({i, s});
    }
    
    // 按分数降序排序
    std::sort(scored.begin(), scored.end(),
              [](const ScoredCandidate& a, const ScoredCandidate& b) {
                  return a.score > b.score;
              });
    
    // 保留 top critic_keep_ratio (至少1个)
    size_t keep_count = std::max<size_t>(1, 
        static_cast<size_t>(candidates.size() * candidate_config_.critic_keep_ratio));
    
    // 选择最佳候选
    size_t best_idx = scored.front().index;
    AgentResult best = candidates[best_idx];
    best.metadata["selected_candidate_index"] = std::to_string(best_idx);
    best.metadata["candidates_generated"] = std::to_string(candidates.size());
    best.metadata["candidates_filtered_to"] = std::to_string(keep_count);
    best.metadata["best_candidate_score"] = std::to_string(scored.front().score);
    best.success = true;
    
    return best;
}

// ==================== 线性迭代 ====================

AgentResult Orchestrator::executeIterationLinear(const std::string& user_query, 
                                                  const Rule& rule,
                                                  const std::string& refined_query) {
    // 委托给旧有的 executeIteration 逻辑
    updateStatistics("iterations_executed");
    
    AgentResult final_result;
    
    // 步骤1：推理解决方案
    AgentResult reasoned_solution = reasonSolution(refined_query, rule);
    if (!reasoned_solution.success) {
        final_result.success = false;
        final_result.error_message = "Reasoning failed: " + reasoned_solution.error_message;
        return final_result;
    }
    
    // 步骤2：执行解决方案
    AgentResult executed_solution = executeSolution(reasoned_solution, rule);
    if (!executed_solution.success) {
        final_result.success = false;
        final_result.error_message = "Execution failed: " + executed_solution.error_message;
        return final_result;
    }
    
    // 步骤3：验证解决方案
    AgentResult verified_solution = verifySolution(executed_solution, rule);
    if (!verified_solution.success) {
        final_result.success = false;
        final_result.error_message = "Verification failed: " + verified_solution.error_message;
        return final_result;
    }
    
    // 组合结果：保留各阶段输出
    final_result = verified_solution;
    final_result.content = executed_solution.content;  // 主输出 = 求解器的真实答案
    final_result.metadata["refined_query"] = refined_query;
    final_result.metadata["reasoned_content"] = reasoned_solution.content;
    final_result.metadata["solved_content"] = executed_solution.content;
    final_result.metadata["verified_content"] = verified_solution.content;
    final_result.metadata["iteration_mode"] = "linear";
    final_result.success = true;
    
    return final_result;
}

// ==================== 并行迭代 ====================

AgentResult Orchestrator::executeIterationParallel(const std::string& user_query,
                                                    const Rule& rule,
                                                    const std::string& refined_query) {
    updateStatistics("iterations_executed");
    
    AgentResult final_result;
    
    // 步骤1：推理解决方案
    AgentResult reasoned_solution = reasonSolution(refined_query, rule);
    if (!reasoned_solution.success) {
        final_result.success = false;
        final_result.error_message = "Reasoning failed: " + reasoned_solution.error_message;
        return final_result;
    }
    
    // 步骤2：并行生成N个候选
    size_t N = candidate_config_.num_candidates;
    std::vector<AgentResult> candidates = generateCandidates(refined_query, rule, N);
    
    if (candidates.empty()) {
        final_result.success = false;
        final_result.error_message = "All candidates failed";
        return final_result;
    }
    
    // 步骤3：Critic 筛选最佳候选
    AgentResult selected = selectBestCandidate(candidates, reasoned_solution, rule);
    if (!selected.success) {
        final_result.success = false;
        final_result.error_message = "Candidate selection failed";
        return final_result;
    }
    
    // 步骤4：完整验证最终候选
    AgentResult verified_solution = verifySolution(selected, rule);
    if (!verified_solution.success) {
        // 验证失败但仍有候选结果可用
        final_result.success = false; 
        final_result.error_message = "Final verification failed for best candidate";
        final_result.content = selected.content;
        final_result.metadata["reasoned_content"] = reasoned_solution.content;
        final_result.metadata["verified_content"] = verified_solution.content;
        return final_result;
    }
    
    // 组合结果
    final_result = verified_solution;
    final_result.content = selected.content;
    final_result.metadata["refined_query"] = refined_query;
    final_result.metadata["reasoned_content"] = reasoned_solution.content;
    final_result.metadata["solved_content"] = selected.content;
    final_result.metadata["verified_content"] = verified_solution.content;
    final_result.metadata["iteration_mode"] = "parallel";
    final_result.metadata["candidates_count"] = std::to_string(candidates.size());
    final_result.success = true;
    
    return final_result;
}

std::string Orchestrator::refineQuery(const std::string& user_query, const Rule& rule) {
    // 检查是否有精炼智能体注册
    if (agents_.count(AgentType::REFINER)) {
        AgentResult result = agents_[AgentType::REFINER](user_query, rule.validation_method);
        if (result.success) {
            updateStatistics("queries_refined");
            return result.content;
        }
    }
    
    // 默认精炼：添加规则上下文
    std::stringstream refined;
    refined << "Query: " << user_query << "\n";
    refined << "Context: This query matches pattern '" << rule.pattern << "'\n";
    refined << "Validation method: " << rule.validation_method << "\n";
    refined << "Recommended tools: ";
    for (const auto& tool : rule.recommended_tools) {
        refined << tool << " ";
    }
    
    return refined.str();
}

AgentResult Orchestrator::reasonSolution(const std::string& refined_query, const Rule& rule) {
    // 检查是否有推理智能体注册
    if (agents_.count(AgentType::REASONER)) {
        updateStatistics("solutions_reasoned");
        return agents_[AgentType::REASONER](refined_query, rule.validation_method);
    }
    
    // 默认推理：生成简单解决方案
    AgentResult result;
    result.content = "Proposed solution for: " + refined_query + "\n";
    result.content += "Using validation method: " + rule.validation_method + "\n";
    result.content += "This is a placeholder reasoning result.";
    result.success = true;
    result.metadata["reasoning_type"] = "default";
    
    return result;
}

AgentResult Orchestrator::executeSolution(const AgentResult& reasoned_solution, const Rule& rule) {
    // 检查是否有解决智能体注册
    if (agents_.count(AgentType::SOLVER)) {
        updateStatistics("solutions_executed");
        return agents_[AgentType::SOLVER](reasoned_solution.content, rule.validation_method);
    }
    
    // 默认执行：返回输入作为输出
    AgentResult result;
    result.content = "Executed: " + reasoned_solution.content;
    result.success = true;
    result.metadata["execution_type"] = "default";
    
    return result;
}

AgentResult Orchestrator::verifySolution(const AgentResult& executed_solution, const Rule& rule) {
    // 检查是否有验证智能体注册
    if (agents_.count(AgentType::VERIFIER)) {
        updateStatistics("solutions_verified");
        return agents_[AgentType::VERIFIER](executed_solution.content, rule.validation_method);
    }
    
    // 默认验证：始终通过
    AgentResult result;
    result.content = "Verified: " + executed_solution.content;
    result.success = true;
    result.metadata["verification_type"] = "default";
    
    return result;
}

EvaluationScore Orchestrator::evaluateResult(const AgentResult& verified_solution, 
                                             const Rule& rule) {
    // 检查是否有评估智能体注册
    if (agents_.count(AgentType::EVALUATOR)) {
        AgentResult eval_result = agents_[AgentType::EVALUATOR](verified_solution.content, rule.validation_method);
        if (eval_result.success && eval_result.metadata.count("reasonableness") &&
            eval_result.metadata.count("executability") &&
            eval_result.metadata.count("satisfaction")) {
            
            EvaluationScore score;
            score.reasonableness = std::stod(eval_result.metadata.at("reasonableness"));
            score.executability = std::stod(eval_result.metadata.at("executability"));
            score.satisfaction = std::stod(eval_result.metadata.at("satisfaction"));
            
            updateStatistics("results_evaluated");
            return score;
        }
    }
    
    // 默认评估：基于规则权重的简单分数
    EvaluationScore score;
    
    // 改进的启发式评分（不再基于内容长度）
    size_t content_length = verified_solution.content.length();
    
    // reasonableness: 至少0.5基础分 + 长内容加分
    double reasonableness_base = 0.5;
    double length_bonus = std::min(0.4, content_length / 500.0);
    score.reasonableness = std::min(1.0, reasonableness_base + length_bonus);
    
    // executability: 默认0.7，含代码片段(+0.1)加分
    double exec_base = 0.7;
    if (content_length > 20) exec_base += 0.1;  // 有实际内容
    score.executability = std::min(1.0, exec_base);
    
    // satisfaction: 默认0.6，含详细内容(+0.1)加分
    double sat_base = 0.6;
    if (content_length > 50) sat_base += 0.1;
    score.satisfaction = std::min(1.0, sat_base);
    
    return score;
}

void Orchestrator::recordTokenUsage(size_t prompt_tokens, size_t completion_tokens, 
                                    const std::string& agent_type, const std::string& operation) {
    std::cout << "[Orchestrator] recordTokenUsage: prompt=" << prompt_tokens << ", completion=" << completion_tokens << std::endl;
    if (token_monitor_) {
        token_monitor_->recordUsage(prompt_tokens, completion_tokens, agent_type, operation);
    }
}

void Orchestrator::updateStatistics(const std::string& key, size_t value) {
    statistics_[key] += value;
}

// ==================== DAG 规划实现 ====================

void Orchestrator::registerPlanner(AgentCallback planner_callback) {
    planner_callback_ = std::move(planner_callback);
}

void Orchestrator::setDagMode(bool enabled) {
    dag_config_.enabled = enabled;
}

bool Orchestrator::isDagMode() const {
    return dag_config_.enabled;
}

void Orchestrator::setDAGConfig(const DAGConfig& config) {
    dag_config_ = config;
}

const DAGConfig& Orchestrator::getDAGConfig() const {
    return dag_config_;
}

bool Orchestrator::hasDagResult() const {
    return dag_has_result_;
}

std::map<std::string, std::string> Orchestrator::getDagStatus() const {
    std::map<std::string, std::string> status;
    status["mode"] = dag_config_.enabled ? "dag" : "linear";
    status["has_result"] = dag_has_result_ ? "true" : "false";
    status["total_nodes"] = std::to_string(current_dag_.getNodeCount());
    status["completed"] = std::to_string(current_dag_.getCompletedCount());
    status["failed"] = std::to_string(current_dag_.getFailedCount());
    return status;
}

std::string Orchestrator::getTaskContext(const TaskGraph& graph, const TaskNode& node) const {
    std::string context;
    // 注入已完成的前驱任务结果
    for (const auto& depId : node.dependencies) {
        auto it = dag_checkpoints_.find(depId);
        if (it != dag_checkpoints_.end()) {
            context += "=== Result from " + depId + " ===\n";
            context += it->second.content + "\n\n";
        }
    }
    return context;
}

void Orchestrator::rollbackTask(TaskNode& node) {
    node.status = "rollback";
    // 回退意味着标记为失败，依赖此节点的其他节点将检测到失败不再执行
    // dag_checkpoints_ 保留之前成功节点的 checkpoint
    std::cerr << "[DAG] Rolled back task: " << node.id << std::endl;
    
    // 标记所有依赖此节点的节点为 blocked
    for (auto& n : current_dag_.nodes) {
        for (const auto& dep : n.dependencies) {
            if (dep == node.id && n.status == "pending") {
                n.status = "failed";
                n.result.error_message = "Dependency " + node.id + " failed";
                std::cerr << "[DAG] Cascading failure: " << n.id 
                          << " (depends on " << node.id << ")" << std::endl;
            }
        }
    }
}

AgentResult Orchestrator::executeDAGChunk(const std::string& task_description,
                                           const std::string& context) {
    // 复用线性迭代逻辑
    // 创建一个临时 rule 用于 chunk 执行
    Rule chunk_rule;
    chunk_rule.pattern = ".*";
    chunk_rule.validation_method = "dag_chunk";
    chunk_rule.threshold = 0.3;
    
    // 把 task_description 和 context 合并为完整输入
    std::string full_input = task_description;
    if (!context.empty()) {
        full_input += "\n\nContext from completed subtasks:\n" + context;
    }
    
    return executeIterationLinear(full_input, chunk_rule, task_description);
}

AgentResult Orchestrator::processQueryDag(const std::string& user_query) {
    updateStatistics("dag_queries_processed");
    dag_has_result_ = false;
    current_dag_ = TaskGraph();
    dag_checkpoints_.clear();
    
    // Step 1: 使用 planner 回调分解任务
    AgentResult plan_result = planner_callback_(user_query, "dag_plan");
    if (!plan_result.success) {
        AgentResult err;
        err.success = false;
        err.error_message = "DAG planning failed: " + plan_result.error_message;
        return err;
    }
    
    // Step 2: 从 planner 输出中解析 TaskGraph
    // 支持两种格式：
    //   Format A — JSON 数组: [{"id":"task_0","desc":"...","deps":[]}, ...]
    //   Format B — 管道格式:  task_0|description|dep1,dep2  (每行一个任务)
    // 优先尝试 JSON 解析，失败后回退到管道格式解析。
    {
        // Try Format A: JSON
        bool json_parsed = false;
        std::string stripped = plan_result.content;
        {
            // 尝试提取 ```json ... ``` 块
            auto json_start = stripped.find("```json");
            if (json_start != std::string::npos) {
                auto json_end = stripped.find("```", json_start + 7);
                if (json_end != std::string::npos) {
                    stripped = stripped.substr(json_start + 7, json_end - json_start - 7);
                }
            }
            // 尝试提取 ``` ... ``` 块（无语言标记）
            if (!json_parsed) {
                json_start = stripped.find("```");
                if (json_start != std::string::npos) {
                    auto json_end = stripped.find("```", json_start + 3);
                    if (json_end != std::string::npos) {
                        stripped = stripped.substr(json_start + 3, json_end - json_start - 3);
                    }
                }
            }
            // 去除首尾空白
            auto start = stripped.find_first_not_of(" \t\n\r");
            if (start != std::string::npos) {
                auto end = stripped.find_last_not_of(" \t\n\r");
                stripped = stripped.substr(start, end - start + 1);
            } else {
                stripped.clear();
            }
            
            // 检查是否是 JSON 数组（以 [ 开头）
            if (!stripped.empty() && stripped.front() == '[') {
                try {
                    YAML::Node root = YAML::Load(stripped);
                    if (root.IsSequence()) {
                        for (size_t i = 0; i < root.size(); ++i) {
                            const auto& item = root[i];
                            if (!item.IsMap()) {
                                std::cerr << "[DAG] JSON item[" << i << "] is not a map, skipping" << std::endl;
                                continue;
                            }
                            TaskNode node;
                            node.id = item["id"].as<std::string>("");
                            if (node.id.empty()) {
                                std::cerr << "[DAG] JSON item[" << i << "]: empty id, skipping" << std::endl;
                                continue;
                            }
                            // 支持 desc 或 description 字段
                            node.description = item["desc"].as<std::string>("");
                            if (node.description.empty()) {
                                node.description = item["description"].as<std::string>("");
                            }
                            // 解析 deps 字段（可能是数组或字符串）
                            if (item["deps"].IsDefined()) {
                                if (item["deps"].IsSequence()) {
                                    for (size_t d = 0; d < item["deps"].size(); ++d) {
                                        std::string dep = item["deps"][d].as<std::string>("");
                                        if (!dep.empty() && dep != node.id) {
                                            node.dependencies.push_back(dep);
                                        }
                                    }
                                } else if (item["deps"].IsScalar()) {
                                    std::string deps_str = item["deps"].as<std::string>("");
                                    if (!deps_str.empty()) {
                                        std::istringstream deps_stream(deps_str);
                                        std::string dep;
                                        while (std::getline(deps_stream, dep, ',')) {
                                            dep.erase(0, dep.find_first_not_of(" \t"));
                                            dep.erase(dep.find_last_not_of(" \t") + 1);
                                            if (!dep.empty() && dep != node.id) {
                                                node.dependencies.push_back(dep);
                                            }
                                        }
                                    }
                                }
                            }
                            node.status = "pending";
                            current_dag_.addNode(node);
                            json_parsed = true;
                        }
                    }
                } catch (const YAML::Exception& e) {
                    std::cerr << "[DAG] JSON parse error: " << e.what() << ", falling back to pipe format" << std::endl;
                }
            }
        }
        
        // If JSON parsing succeeded, skip pipe format parsing
        if (json_parsed) {
            goto after_parsing;
        }
    }
    
    // Format B — 管道格式（兼容旧格式）
    {
        std::istringstream plan_stream(plan_result.content);
        std::string line;
        size_t line_num = 0;
        while (std::getline(plan_stream, line)) {
            line_num++;
            // 跳过空行和 ``` 标记（以防 JSON 尝试混合）
            auto trimmed = line;
            auto ts = trimmed.find_first_not_of(" \t\n\r");
            if (ts != std::string::npos) trimmed = trimmed.substr(ts);
            auto te = trimmed.find_last_not_of(" \t\n\r");
            if (te != std::string::npos) trimmed = trimmed.substr(0, te + 1);
            if (trimmed.empty() || trimmed.find("```") != std::string::npos) continue;
            
            // 跳过非管道行（不以 task_ 开头且不包含 |）
            if (trimmed.find('|') == std::string::npos) {
                if (trimmed.find("task_") == std::string::npos) continue;
            }
            
            TaskNode node;
            auto first_pipe = trimmed.find('|');
            if (first_pipe == std::string::npos) {
                std::cerr << "[DAG] Line " << line_num << ": no pipe delimiter, skipping" << std::endl;
                continue;
            }
            node.id = trimmed.substr(0, first_pipe);
            // 去除 id 首尾空白
            {
                auto is = node.id.find_first_not_of(" \t");
                auto ie = node.id.find_last_not_of(" \t");
                if (is != std::string::npos) node.id = node.id.substr(is, ie - is + 1);
            }
            if (node.id.empty()) {
                std::cerr << "[DAG] Line " << line_num << ": empty task id, skipping" << std::endl;
                continue;
            }
            
            auto second_pipe = trimmed.find('|', first_pipe + 1);
            if (second_pipe == std::string::npos) {
                node.description = trimmed.substr(first_pipe + 1);
                // 去除 description 首尾空白
                {
                    auto ds = node.description.find_first_not_of(" \t");
                    auto de = node.description.find_last_not_of(" \t");
                    if (ds != std::string::npos) node.description = node.description.substr(ds, de - ds + 1);
                }
            } else {
                node.description = trimmed.substr(first_pipe + 1, second_pipe - first_pipe - 1);
                {
                    auto ds = node.description.find_first_not_of(" \t");
                    auto de = node.description.find_last_not_of(" \t");
                    if (ds != std::string::npos) node.description = node.description.substr(ds, de - ds + 1);
                }
                std::string deps_str = trimmed.substr(second_pipe + 1);
                {
                    auto ds = deps_str.find_first_not_of(" \t");
                    auto de = deps_str.find_last_not_of(" \t");
                    if (ds != std::string::npos) deps_str = deps_str.substr(ds, de - ds + 1);
                }
                if (!deps_str.empty()) {
                    std::istringstream deps_stream(deps_str);
                    std::string dep;
                    while (std::getline(deps_stream, dep, ',')) {
                        dep.erase(0, dep.find_first_not_of(" \t"));
                        dep.erase(dep.find_last_not_of(" \t") + 1);
                        if (!dep.empty() && dep != node.id) {
                            node.dependencies.push_back(dep);
                        }
                    }
                }
            }
            if (node.description.empty()) {
                std::cerr << "[DAG] Line " << line_num << " (" << node.id << "): empty description, using id" << std::endl;
                node.description = node.id;
            }
            node.status = "pending";
            current_dag_.addNode(node);
        }
    }
    
after_parsing:
    
    // Step 3: 检查是否应降级（只有 1 个 task 或空图）
    if (current_dag_.getNodeCount() <= dag_config_.min_subtasks_to_enable 
        && dag_config_.auto_downgrade) {
        std::cerr << "[DAG] Auto-downgrade to linear mode (" 
                  << current_dag_.getNodeCount() << " tasks)\n";
        dag_config_.enabled = false;  // 暂时关闭
        AgentResult result = processQuery(user_query);  // 用线性模式重新处理
        dag_config_.enabled = true;   // 恢复
        return result;
    }
    
    // Step 4: 检查循环依赖
    if (current_dag_.hasCyclicDependency()) {
        AgentResult err;
        err.success = false;
        err.error_message = "DAG has cyclic dependency";
        return err;
    }
    
    // Step 5: DAG 调度主循环
    while (!current_dag_.allCompleted()) {
        auto ready_indices = current_dag_.getReadyNodeIndices();
        if (ready_indices.empty()) {
            // 没有就绪节点但未完成 = blocked（所有未完成节点都在等已失败的节点）
            break;
        }
        
        for (size_t idx : ready_indices) {
            TaskNode* node = &current_dag_.nodes[idx];
            node->status = "running";
            updateStatistics("dag_tasks_executed");
            
            std::string context = getTaskContext(current_dag_, *node);
            AgentResult chunk_result = executeDAGChunk(node->description, context);
            
            if (chunk_result.success) {
                node->status = "done";
                node->result = chunk_result;
                dag_checkpoints_[node->id] = chunk_result;
                updateStatistics("dag_tasks_completed");
            } else {
                node->retry_count++;
                if (node->retry_count > node->max_retries) {
                    rollbackTask(*node);
                    updateStatistics("dag_tasks_failed");
                } else {
                    node->status = "failed";  // 下次循环会重新尝试
                    std::cerr << "[DAG] Retry " << node->retry_count 
                              << "/" << node->max_retries 
                              << " for " << node->id << std::endl;
                }
            }
        }
    }
    
    // Step 6: 合并结果
    AgentResult final_result;
    std::string merged_content;
    bool all_success = true;
    size_t completed = current_dag_.getCompletedCount();
    size_t total = current_dag_.getNodeCount();
    
    // 按原始顺序拼接各 task 输出
    for (const auto& node : current_dag_.nodes) {
        if (node.status == "done") {
            merged_content += "=== " + node.id + ": " + node.description + " ===\n";
            merged_content += node.result.content + "\n\n";
        } else {
            all_success = false;
            merged_content += "=== " + node.id + ": " + node.description + " [FAILED] ===\n";
            merged_content += node.result.error_message + "\n\n";
        }
    }
    
    final_result.content = merged_content;
    final_result.success = all_success;
    if (!all_success) {
        final_result.error_message = "DAG completed with " + 
            std::to_string(current_dag_.getFailedCount()) + " failed tasks";
    }
    final_result.metadata["dag_completed"] = "true";
    final_result.metadata["dag_total_tasks"] = std::to_string(total);
    final_result.metadata["dag_completed_tasks"] = std::to_string(completed);
    final_result.metadata["dag_failed_tasks"] = std::to_string(current_dag_.getFailedCount());
    final_result.metadata["dag_auto_downgraded"] = "false";
    
    dag_has_result_ = true;
    updateStatistics("dag_processes_completed");
    return final_result;
}

} // namespace clma