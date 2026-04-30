#ifndef CLOSED_LOOP_TYPES_HPP
#define CLOSED_LOOP_TYPES_HPP

#include <string>
#include <vector>
#include <map>
#include <variant>
#include <memory>
#include <optional>
#include <chrono>
#include <cmath>
#include <limits>

namespace clma {

// 评估分数结构
struct EvaluationScore {
    double reasonableness;  // 合理度 (0-1)
    double executability;   // 可执行度 (0-1)
    double satisfaction;    // 满意度 (0-1)
    
    EvaluationScore() : reasonableness(0.0), executability(0.0), satisfaction(0.0) {}
    
    EvaluationScore(double r, double e, double s) 
        : reasonableness(r), executability(e), satisfaction(s) {}
    
    double overall() const {
        return (reasonableness + executability + satisfaction) / 3.0;
    }
    
    bool meetsThreshold(double threshold) const {
        return overall() >= threshold - std::numeric_limits<double>::epsilon();
    }
};

// 智能体结果
struct AgentResult {
    std::string content;
    std::map<std::string, std::string> metadata;
    EvaluationScore score;
    bool success;
    std::string error_message;
    
    AgentResult() : success(false) {}
};

// 规则定义
struct Rule {
    std::string pattern;
    std::string validation_method;
    std::vector<std::string> recommended_tools;
    std::map<std::string, double> weights;  // 评分权重
    double threshold;
    
    Rule() : threshold(0.0) {}
    
    bool matches(const std::string& query) const;
};

// 智能体类型枚举
enum class AgentType {
    REFINER,
    REASONER,
    SOLVER,
    VERIFIER,
    EVALUATOR
};

// 智能体状态
enum class AgentState {
    IDLE,
    PROCESSING,
    SUCCESS,
    FAILED
};

// Token使用记录
struct TokenUsage {
    size_t prompt_tokens;
    size_t completion_tokens;
    size_t total_tokens;
    std::chrono::system_clock::time_point timestamp;
    std::string agent_type;
    std::string operation;
    
    TokenUsage() : prompt_tokens(0), completion_tokens(0), total_tokens(0) {}
    
    TokenUsage(size_t prompt, size_t completion, const std::string& agent, const std::string& op)
        : prompt_tokens(prompt), completion_tokens(completion),
          total_tokens(prompt + completion),
          timestamp(std::chrono::system_clock::now()),
          agent_type(agent), operation(op) {}
};

// 并行候选配置
struct CandidateConfig {
    size_t num_candidates = 3;       // 并行生成候选数
    bool enabled = false;            // 默认关闭
    double critic_keep_ratio = 0.5;  // critic 保留比例
    double early_stop_epsilon = 0.02;// 收敛判定阈值
    size_t early_stop_lookback = 3;  // 连续N次改进小于epsilon则早停
};

// DAG 规划配置
struct DAGConfig {
    bool enabled = false;            // 默认关闭
    size_t max_subtasks = 8;         // 最大子任务数
    size_t min_subtasks_to_enable = 2; // ≥2 才启用 DAG 模式
    bool auto_downgrade = true;      // 简单查询自动降级线性
};

// DAG 任务节点
struct TaskNode {
    std::string id;                    // "task_0", "task_1"
    std::string description;           // "实现加法函数"
    std::vector<std::string> dependencies; // ["task_0"]
    std::string status;                // "pending"|"running"|"done"|"failed"|"rollback"
    AgentResult result;                // 执行结果
    EvaluationScore score;             // 最终评分
    size_t retry_count = 0;
    size_t max_retries = 2;
};

// DAG 任务图
struct TaskGraph {
    std::vector<TaskNode> nodes;
    
    void addNode(const TaskNode& node);
    
    // 获取所有依赖已满足的就绪节点索引
    std::vector<size_t> getReadyNodeIndices() const;
    
    // 是否全部完成
    bool allCompleted() const;
    
    // 节点数量
    size_t getNodeCount() const { return nodes.size(); }
    
    // 按 ID 查找节点
    TaskNode* findNode(const std::string& id);
    const TaskNode* findNode(const std::string& id) const;
    
    // 检查是否有循环依赖（DFS）
    bool hasCyclicDependency() const;
    
    // 获取已完成和失败的数量
    size_t getCompletedCount() const;
    size_t getFailedCount() const;
};

} // namespace clma

#endif // CLOSED_LOOP_TYPES_HPP