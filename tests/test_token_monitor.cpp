#include <gtest/gtest.h>
#include "core/TokenMonitor.hpp"
#include <thread>
#include <chrono>

TEST(TokenMonitorTest, DefaultConstructor) {
    clma::TokenMonitor monitor;
    EXPECT_EQ(monitor.getBudget(), 10000);
    EXPECT_EQ(monitor.getTotalUsed(), 0);
    EXPECT_EQ(monitor.getRemainingBudget(), 10000);
    EXPECT_FALSE(monitor.isOverBudget());
    EXPECT_DOUBLE_EQ(monitor.getUsageRatio(), 0.0);
}

TEST(TokenMonitorTest, CustomBudgetConstructor) {
    clma::TokenMonitor monitor(5000);
    EXPECT_EQ(monitor.getBudget(), 5000);
    EXPECT_EQ(monitor.getTotalUsed(), 0);
    EXPECT_EQ(monitor.getRemainingBudget(), 5000);
}

TEST(TokenMonitorTest, RecordUsage) {
    clma::TokenMonitor monitor(1000);
    
    monitor.recordUsage(100, 200, "refiner", "refine_query");
    EXPECT_EQ(monitor.getTotalUsed(), 300);
    EXPECT_EQ(monitor.getRemainingBudget(), 700);
    EXPECT_DOUBLE_EQ(monitor.getUsageRatio(), 0.3);
    
    monitor.recordUsage(50, 50, "reasoner", "reason_solution");
    EXPECT_EQ(monitor.getTotalUsed(), 400);
    EXPECT_EQ(monitor.getRemainingBudget(), 600);
    EXPECT_DOUBLE_EQ(monitor.getUsageRatio(), 0.4);
}

TEST(TokenMonitorTest, RecordUsageWithTokenUsageStruct) {
    clma::TokenMonitor monitor;
    
    clma::TokenUsage usage(300, 200, "solver", "execute_solution");
    monitor.recordUsage(usage);
    
    EXPECT_EQ(monitor.getTotalUsed(), 500);
    EXPECT_FALSE(monitor.isOverBudget());
}

TEST(TokenMonitorTest, IsOverBudget) {
    clma::TokenMonitor monitor(100);
    
    // 调试输出
    // std::cout << "Budget: " << monitor.getTotalBudget() << ", Used: " << monitor.getTotalUsed() << std::endl;
    
    // 初始状态：未超预算
    EXPECT_FALSE(monitor.isOverBudget());
    
    // 使用60+50=110 tokens，超过100预算
    monitor.recordUsage(60, 50, "test", "test");
    EXPECT_TRUE(monitor.isOverBudget()); // 110 > 100，应该超预算
    EXPECT_EQ(monitor.getRemainingBudget(), 0);
}

TEST(TokenMonitorTest, BudgetExceeded) {
    clma::TokenMonitor monitor(100);
    
    monitor.recordUsage(60, 50, "test", "test"); // 110 tokens
    EXPECT_TRUE(monitor.isOverBudget());
    EXPECT_EQ(monitor.getRemainingBudget(), 0); // 超预算后剩余预算为0
}

TEST(TokenMonitorTest, SetBudget) {
    clma::TokenMonitor monitor(1000);
    EXPECT_EQ(monitor.getBudget(), 1000);
    
    monitor.setBudget(2000);
    EXPECT_EQ(monitor.getBudget(), 2000);
    EXPECT_EQ(monitor.getRemainingBudget(), 2000); // 使用量仍为0
}

TEST(TokenMonitorTest, Reset) {
    clma::TokenMonitor monitor(1000);
    
    monitor.recordUsage(100, 200, "test", "test");
    EXPECT_EQ(monitor.getTotalUsed(), 300);
    
    monitor.reset();
    EXPECT_EQ(monitor.getTotalUsed(), 0);
    EXPECT_EQ(monitor.getRemainingBudget(), 1000);
    EXPECT_DOUBLE_EQ(monitor.getUsageRatio(), 0.0);
}

TEST(TokenMonitorTest, GetUsageByAgent) {
    clma::TokenMonitor monitor;
    
    monitor.recordUsage(100, 50, "refiner", "refine");
    monitor.recordUsage(200, 100, "refiner", "refine2");
    monitor.recordUsage(50, 25, "reasoner", "reason");
    monitor.recordUsage(75, 40, "solver", "solve");
    
    auto usage_by_agent = monitor.getUsageByAgent();
    EXPECT_EQ(usage_by_agent.size(), 3);
    EXPECT_EQ(usage_by_agent["refiner"], 150 + 300); // 100+50 + 200+100 = 450
    EXPECT_EQ(usage_by_agent["reasoner"], 75); // 50+25
    EXPECT_EQ(usage_by_agent["solver"], 115); // 75+40
}

TEST(TokenMonitorTest, GetUsageByOperation) {
    clma::TokenMonitor monitor;
    
    monitor.recordUsage(100, 50, "refiner", "refine");
    monitor.recordUsage(200, 100, "refiner", "process");
    monitor.recordUsage(50, 25, "reasoner", "refine");
    monitor.recordUsage(75, 40, "solver", "process");
    
    auto usage_by_op = monitor.getUsageByOperation();
    EXPECT_EQ(usage_by_op.size(), 2);
    EXPECT_EQ(usage_by_op["refine"], 150 + 75); // 100+50 + 50+25 = 225
    EXPECT_EQ(usage_by_op["process"], 300 + 115); // 200+100 + 75+40 = 415
}

TEST(TokenMonitorTest, GetRecentUsage) {
    clma::TokenMonitor monitor;
    
    // 添加一些使用记录
    for (int i = 1; i <= 5; ++i) {
        monitor.recordUsage(i * 10, i * 5, "agent", "op" + std::to_string(i));
    }
    
    auto recent = monitor.getRecentUsage(3);
    EXPECT_EQ(recent.size(), 3);
    
    // 应该返回最后3条记录
    if (recent.size() >= 3) {
        // 检查最后一条记录（最新的）
        EXPECT_EQ(recent[2].prompt_tokens, 50); // i=5 * 10
        EXPECT_EQ(recent[2].completion_tokens, 25); // i=5 * 5
    }
    
    // 请求超过实际数量
    auto all = monitor.getRecentUsage(10);
    EXPECT_EQ(all.size(), 5);
}

TEST(TokenMonitorTest, SuggestTruncationLength) {
    clma::TokenMonitor monitor(1000);
    
    // 空历史记录
    size_t suggestion1 = monitor.suggestTruncationLength();
    EXPECT_GE(suggestion1, 100);
    EXPECT_LE(suggestion1, 5000);
    
    // 添加一些使用记录
    monitor.recordUsage(100, 50, "agent", "op");
    monitor.recordUsage(200, 100, "agent", "op");
    monitor.recordUsage(150, 75, "agent", "op");
    
    size_t suggestion2 = monitor.suggestTruncationLength();
    EXPECT_GE(suggestion2, 100);
    EXPECT_LE(suggestion2, 5000);
    
    // 使用率较高时，建议长度应该较小
    clma::TokenMonitor low_budget_monitor(100);
    low_budget_monitor.recordUsage(60, 40, "agent", "op"); // 使用率100%
    size_t suggestion3 = low_budget_monitor.suggestTruncationLength();
    EXPECT_EQ(suggestion3, 100); // 最小值
}

TEST(TokenMonitorTest, NeedsWarning) {
    clma::TokenMonitor monitor(100);
    
    // 使用率低于80%，不需要警告
    monitor.recordUsage(40, 30, "agent", "op"); // 70/100 = 70%
    EXPECT_FALSE(monitor.needsWarning());
    
    // 使用率达到80%，需要警告
    monitor.recordUsage(10, 0, "agent", "op"); // 80/100 = 80%
    EXPECT_TRUE(monitor.needsWarning());
    
    // 使用率超过80%
    monitor.recordUsage(10, 0, "agent", "op"); // 90/100 = 90%
    EXPECT_TRUE(monitor.needsWarning());
}

TEST(TokenMonitorTest, HeavyUsage) {
    clma::TokenMonitor monitor(10000);
    
    // 大量使用
    for (int i = 0; i < 100; ++i) {
        monitor.recordUsage(100, 50, "agent" + std::to_string(i % 3), "operation");
    }
    
    EXPECT_GT(monitor.getTotalUsed(), 0);
    auto usage_by_agent = monitor.getUsageByAgent();
    EXPECT_GE(usage_by_agent.size(), 1);
    
    // 检查清理机制（最大历史记录1000条）
    for (int i = 0; i < 2000; ++i) {
        monitor.recordUsage(1, 1, "mass", "mass_op");
    }
    
    // 历史记录应该被限制
    auto recent = monitor.getRecentUsage(1500);
    EXPECT_LE(recent.size(), 1000); // 实际上应该正好1000或更少
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}