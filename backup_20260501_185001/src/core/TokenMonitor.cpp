#include "core/TokenMonitor.hpp"
#include <algorithm>
#include <map>
#include <limits>
#include <iostream>

namespace clma {

TokenMonitor::TokenMonitor(size_t total_budget)
    : total_budget_(total_budget), total_used_(0) {
    // usage_history_.reserve(MAX_HISTORY); // deque does not have reserve
    std::cout << "[TokenMonitor] Constructor: budget=" << total_budget_ << ", used=" << total_used_ << std::endl;
}

void TokenMonitor::recordUsage(const TokenUsage& usage) {
    total_used_ += usage.total_tokens;
    usage_history_.push_back(usage);
    
    // 限制历史记录大小
    if (usage_history_.size() > MAX_HISTORY) {
        usage_history_.pop_front();
    }
}

void TokenMonitor::recordUsage(size_t prompt_tokens, size_t completion_tokens, 
                               const std::string& agent_type, const std::string& operation) {
    TokenUsage usage(prompt_tokens, completion_tokens, agent_type, operation);
    recordUsage(usage);
}

bool TokenMonitor::isOverBudget() const {
    std::cout << "[TokenMonitor] isOverBudget: used=" << total_used_ << ", budget=" << total_budget_ << ", result=" << (total_used_ > total_budget_) << std::endl;
    return total_used_ > total_budget_;
}

size_t TokenMonitor::getRemainingBudget() const {
    if (total_used_ > total_budget_) {
        return 0;
    }
    return total_budget_ - total_used_;
}

size_t TokenMonitor::getTotalUsed() const {
    return total_used_;
}

std::map<std::string, size_t> TokenMonitor::getUsageByAgent() const {
    std::map<std::string, size_t> result;
    for (const auto& usage : usage_history_) {
        result[usage.agent_type] += usage.total_tokens;
    }
    return result;
}

std::map<std::string, size_t> TokenMonitor::getUsageByOperation() const {
    std::map<std::string, size_t> result;
    for (const auto& usage : usage_history_) {
        result[usage.operation] += usage.total_tokens;
    }
    return result;
}

std::vector<TokenUsage> TokenMonitor::getRecentUsage(size_t limit) const {
    if (limit >= usage_history_.size()) {
        return {usage_history_.begin(), usage_history_.end()};
    }
    
    // 返回最新的limit条记录
    auto start = usage_history_.end() - limit;
    return {start, usage_history_.end()};
}

size_t TokenMonitor::suggestTruncationLength() const {
    if (usage_history_.empty()) {
        return 1000;  // 默认长度
    }
    
    // 计算平均每次操作的Token使用量
    double avg_tokens_per_op = static_cast<double>(total_used_) / usage_history_.size();
    
    // 根据剩余预算调整建议长度
    double budget_ratio = getUsageRatio();
    double adjustment_factor = 1.0 - budget_ratio;
    
    // 基础建议长度
    size_t base_length = static_cast<size_t>(avg_tokens_per_op * 2.0);
    
    // 应用调整因子
    size_t suggested = static_cast<size_t>(base_length * adjustment_factor);
    
    // 确保在合理范围内
    const size_t MIN_LENGTH = 100;
    const size_t MAX_LENGTH = 5000;
    
    if (suggested < MIN_LENGTH) {
        return MIN_LENGTH;
    }
    if (suggested > MAX_LENGTH) {
        return MAX_LENGTH;
    }
    return suggested;
}

bool TokenMonitor::needsWarning() const {
    return getUsageRatio() >= WARNING_THRESHOLD;
}

void TokenMonitor::reset() {
    total_used_ = 0;
    usage_history_.clear();
}

void TokenMonitor::setBudget(size_t budget) {
    total_budget_ = budget;
}

double TokenMonitor::getUsageRatio() const {
    if (total_budget_ == 0) {
        return 1.0;
    }
    return static_cast<double>(total_used_) / total_budget_;
}

void TokenMonitor::cleanupHistory() {
    if (usage_history_.size() > MAX_HISTORY) {
        usage_history_.erase(usage_history_.begin(), 
                           usage_history_.end() - MAX_HISTORY);
    }
}

} // namespace clma