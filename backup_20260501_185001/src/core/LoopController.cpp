#include "core/LoopController.hpp"
#include <algorithm>
#include <numeric>
#include <chrono>
#include <cmath>

namespace clma {

LoopController::LoopController()
    : mode_(Mode::CLOSED_LOOP),
      max_iterations_(10),
      current_iteration_(0),
      satisfaction_threshold_(0.3),
      token_budget_(10000),
      timeout_seconds_(300) {
    token_monitor_ = std::make_unique<TokenMonitor>(token_budget_);
    start_time_ = std::chrono::system_clock::now();
}

void LoopController::setMode(Mode mode) {
    mode_ = mode;
}

LoopController::Mode LoopController::getMode() const {
    return mode_;
}

void LoopController::addIterationResult(const EvaluationScore& score, const TokenUsage& token_usage) {
    iteration_history_.emplace_back(score, token_usage);
    token_monitor_->recordUsage(token_usage);
    current_iteration_++;
}

bool LoopController::shouldContinue() const {
    if (mode_ == Mode::OPEN_LOOP) {
        return false;  // 开环模式只执行一次
    }
    
    if (shouldStop()) {
        return false;
    }
    
    // 检查是否满足阈值
    if (current_iteration_ > 0) {
        const auto& last_score = iteration_history_.back().first;
        if (last_score.meetsThreshold(satisfaction_threshold_)) {
            return false;
        }
    }
    
    return true;
}

bool LoopController::shouldStop() const {
    // 检查最大迭代次数
    if (current_iteration_ >= max_iterations_) {
        return true;
    }
    
    // 检查Token预算
    if (checkTokenBudget()) {
        return true;
    }
    
    // 检查超时
    if (checkTimeout()) {
        return true;
    }
    
    return false;
}

size_t LoopController::getIterationCount() const {
    return current_iteration_;
}

void LoopController::reset() {
    current_iteration_ = 0;
    iteration_history_.clear();
    token_monitor_->reset();
    start_time_ = std::chrono::system_clock::now();
}

void LoopController::setMaxIterations(size_t max_iterations) {
    max_iterations_ = max_iterations;
}

void LoopController::setSatisfactionThreshold(double threshold) {
    satisfaction_threshold_ = threshold;
}

EvaluationScore LoopController::getBestScore() const {
    if (iteration_history_.empty()) {
        return EvaluationScore{0.0, 0.0, 0.0};
    }
    
    auto best_it = std::max_element(iteration_history_.begin(), iteration_history_.end(),
        [](const auto& a, const auto& b) {
            return a.first.overall() < b.first.overall();
        });
    
    return best_it->first;
}

size_t LoopController::getTotalTokenUsage() const {
    return token_monitor_->getTotalUsed();
}

EvaluationScore LoopController::getAverageScore() const {
    if (iteration_history_.empty()) {
        return EvaluationScore{0.0, 0.0, 0.0};
    }
    
    EvaluationScore sum{0.0, 0.0, 0.0};
    for (const auto& [score, _] : iteration_history_) {
        sum.reasonableness += score.reasonableness;
        sum.executability += score.executability;
        sum.satisfaction += score.satisfaction;
    }
    
    double count = static_cast<double>(iteration_history_.size());
    return EvaluationScore{
        sum.reasonableness / count,
        sum.executability / count,
        sum.satisfaction / count
    };
}

bool LoopController::hasConverged(size_t lookback, double epsilon) const {
    if (iteration_history_.size() < lookback + 1) {
        return false;
    }
    
    // 检查最后lookback次迭代的改进
    size_t start_idx = iteration_history_.size() - lookback - 1;
    for (size_t i = start_idx; i < iteration_history_.size() - 1; ++i) {
        double improvement = calculateImprovement(
            iteration_history_[i].first,
            iteration_history_[i + 1].first
        );
        
        if (improvement > epsilon) {
            return false;
        }
    }
    
    return true;
}

const std::vector<std::pair<EvaluationScore, TokenUsage>>& LoopController::getIterationHistory() const {
    return iteration_history_;
}

void LoopController::applyTokenOptimization() {
    if (token_monitor_->needsWarning()) {
        // 应用压缩策略
        size_t suggested_length = token_monitor_->suggestTruncationLength();
        // 这里可以记录日志或触发优化操作
    }
}

void LoopController::setTokenBudget(size_t budget) {
    token_budget_ = budget;
    token_monitor_->setBudget(budget);
}

size_t LoopController::getRemainingTokenBudget() const {
    return token_monitor_->getRemainingBudget();
}

void LoopController::setTimeoutSeconds(size_t seconds) {
    timeout_seconds_ = seconds;
}

bool LoopController::isTimeout() const {
    return checkTimeout();
}

double LoopController::calculateImprovement(const EvaluationScore& prev, const EvaluationScore& curr) const {
    double prev_overall = prev.overall();
    double curr_overall = curr.overall();
    
    if (prev_overall == 0.0) {
        return curr_overall > 0.0 ? 1.0 : 0.0;
    }
    
    return (curr_overall - prev_overall) / prev_overall;
}

bool LoopController::checkTimeout() const {
    auto now = std::chrono::system_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - start_time_).count();
    return elapsed >= timeout_seconds_;
}

bool LoopController::checkTokenBudget() const {
    return token_monitor_->isOverBudget();
}

} // namespace clma