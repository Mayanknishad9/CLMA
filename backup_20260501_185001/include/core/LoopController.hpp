#ifndef CLOSED_LOOP_LOOP_CONTROLLER_HPP
#define CLOSED_LOOP_LOOP_CONTROLLER_HPP

#include "core/Types.hpp"
#include "core/TokenMonitor.hpp"
#include <vector>
#include <memory>
#include <queue>
#include <functional>

namespace clma {

class LoopController {
public:
    enum class Mode {
        CLOSED_LOOP,  // 闭环模式
        OPEN_LOOP     // 开环模式
    };
    
    LoopController();
    
    // 设置模式
    void setMode(Mode mode);
    Mode getMode() const;
    
    // 添加迭代结果
    void addIterationResult(const EvaluationScore& score, const TokenUsage& token_usage);
    
    // 检查是否应该继续迭代
    bool shouldContinue() const;
    
    // 检查是否应该停止（超过最大迭代次数或达到阈值）
    bool shouldStop() const;
    
    // 获取当前迭代次数
    size_t getIterationCount() const;
    
    // 重置控制器
    void reset();
    
    // 设置最大迭代次数
    void setMaxIterations(size_t max_iterations);
    
    // 设置满意度阈值
    void setSatisfactionThreshold(double threshold);
    
    // 获取当前最佳分数
    EvaluationScore getBestScore() const;
    
    // 获取总Token使用量
    size_t getTotalTokenUsage() const;
    
    // 计算平均分数
    EvaluationScore getAverageScore() const;
    
    // 检查是否满足收敛条件（连续N次迭代改进小于epsilon）
    bool hasConverged(size_t lookback = 3, double epsilon = 0.01) const;
    
    // 获取迭代历史
    const std::vector<std::pair<EvaluationScore, TokenUsage>>& getIterationHistory() const;
    
    // 应用Token预算优化
    void applyTokenOptimization();
    
    // 设置Token预算
    void setTokenBudget(size_t budget);
    
    // 获取剩余Token预算
    size_t getRemainingTokenBudget() const;
    
    // 设置超时时间（秒）
    void setTimeoutSeconds(size_t seconds);
    
    // 检查是否超时
    bool isTimeout() const;
    
private:
    Mode mode_;
    size_t max_iterations_;
    size_t current_iteration_;
    double satisfaction_threshold_;
    size_t token_budget_;
    std::chrono::system_clock::time_point start_time_;
    size_t timeout_seconds_;
    
    std::vector<std::pair<EvaluationScore, TokenUsage>> iteration_history_;
    std::unique_ptr<TokenMonitor> token_monitor_;
    
    // 计算分数改进
    double calculateImprovement(const EvaluationScore& prev, const EvaluationScore& curr) const;
    
    // 检查超时
    bool checkTimeout() const;
    
    // 检查Token预算
    bool checkTokenBudget() const;
};

} // namespace clma

#endif // CLOSED_LOOP_LOOP_CONTROLLER_HPP