#ifndef CLOSED_LOOP_TOKEN_MONITOR_HPP
#define CLOSED_LOOP_TOKEN_MONITOR_HPP

#include "core/Types.hpp"
#include <string>
#include <map>
#include <vector>
#include <chrono>
#include <deque>

namespace clma {

class TokenMonitor {
public:

    
    TokenMonitor(size_t total_budget = 10000);
    
    // 记录Token使用
    void recordUsage(const TokenUsage& usage);
    void recordUsage(size_t prompt_tokens, size_t completion_tokens, 
                     const std::string& agent_type, const std::string& operation);
    
    // 检查是否超出预算
    bool isOverBudget() const;
    
    // 获取剩余预算
    size_t getRemainingBudget() const;
    
    // 获取总使用量
    size_t getTotalUsed() const;
    
    // 获取按智能体类型统计的使用量
    std::map<std::string, size_t> getUsageByAgent() const;
    
    // 获取按操作统计的使用量
    std::map<std::string, size_t> getUsageByOperation() const;
    
    // 获取使用历史（最近N条）
    std::vector<TokenUsage> getRecentUsage(size_t limit = 10) const;
    
    // 应用压缩策略（返回建议的截断长度）
    size_t suggestTruncationLength() const;
    
    // 检查是否需要警告（使用量超过80%）
    bool needsWarning() const;
    
    // 重置监控器
    void reset();
    
    // 设置预算
    void setBudget(size_t budget);
    
    // 获取预算
    size_t getBudget() const { return total_budget_; }
    
    // 获取使用率（0-1）
    double getUsageRatio() const;
    
private:
    size_t total_budget_;
    size_t total_used_;
    std::deque<TokenUsage> usage_history_;
    
    // 预算告警阈值（80%）
    static constexpr double WARNING_THRESHOLD = 0.8;
    
    // 最大历史记录数
    static constexpr size_t MAX_HISTORY = 1000;
    
    // 清理旧记录
    void cleanupHistory();
};

} // namespace clma

#endif // CLOSED_LOOP_TOKEN_MONITOR_HPP