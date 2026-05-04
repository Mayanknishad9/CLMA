#include <gtest/gtest.h>
#include "core/TokenMonitor.hpp"
#include <string>
#include <vector>
#include <climits>
#include <random>
#include <algorithm>

// ==================== TokenMonitor 大内容边界测试 ====================

// 测试 1: 极大 token 值（接近 size_t max）
TEST(TokenMonitorEdgeTest, MaxSizeTokenValues) {
    clma::TokenMonitor monitor(1000000);
    
    // 使用接近 size_t 最大值
    size_t huge_prompt = 999999999;   // ~1B
    size_t huge_completion = 888888888;
    
    monitor.recordUsage(huge_prompt, huge_completion, "agent", "huge_op");
    
    // 总使用量应累加
    size_t expected = huge_prompt + huge_completion;
    EXPECT_EQ(monitor.getTotalUsed(), expected);
    
    // 超预算
    EXPECT_TRUE(monitor.isOverBudget());
    EXPECT_TRUE(monitor.needsWarning());
    
    // 按智能体统计
    auto byAgent = monitor.getUsageByAgent();
    EXPECT_EQ(byAgent["agent"], expected);
}

// 测试 2: 零值记录（0 prompt, 0 completion）
TEST(TokenMonitorEdgeTest, ZeroTokenValues) {
    clma::TokenMonitor monitor(100);
    
    // 0+0
    monitor.recordUsage(0, 0, "agent", "noop");
    EXPECT_EQ(monitor.getTotalUsed(), 0);
    EXPECT_FALSE(monitor.isOverBudget());
    EXPECT_DOUBLE_EQ(monitor.getUsageRatio(), 0.0);
    
    // 一个为0
    monitor.recordUsage(50, 0, "agent", "prompt_only");
    EXPECT_EQ(monitor.getTotalUsed(), 50);
    EXPECT_DOUBLE_EQ(monitor.getUsageRatio(), 0.5);
    
    // 另一个为0
    monitor.recordUsage(0, 25, "agent", "completion_only");
    EXPECT_EQ(monitor.getTotalUsed(), 75);
    
    auto byAgent = monitor.getUsageByAgent();
    EXPECT_EQ(byAgent["agent"], 75);
}

// 测试 3: 空 agent_type / 空 operation
TEST(TokenMonitorEdgeTest, EmptyStrings) {
    clma::TokenMonitor monitor;
    
    monitor.recordUsage(100, 50, "", "");
    EXPECT_EQ(monitor.getTotalUsed(), 150);
    
    auto byAgent = monitor.getUsageByAgent();
    EXPECT_EQ(byAgent[""], 150);
    
    auto byOp = monitor.getUsageByOperation();
    EXPECT_EQ(byOp[""], 150);
    
    auto recent = monitor.getRecentUsage(10);
    EXPECT_EQ(recent.size(), 1);
    EXPECT_EQ(recent[0].agent_type, "");
    EXPECT_EQ(recent[0].operation, "");
}

// 测试 4: 单个极大的 prompt 或 completion（远大于当前预算）
TEST(TokenMonitorEdgeTest, SingleRecordExceedsBudgetByFactor) {
    clma::TokenMonitor monitor(1000);
    
    // 单次记录 10000x 预算
    monitor.recordUsage(5000000, 5000000, "big", "op");
    EXPECT_TRUE(monitor.isOverBudget());
    
    // 预算剩 0
    EXPECT_EQ(monitor.getRemainingBudget(), 0);
    
    // 再次记录
    monitor.recordUsage(100, 50, "big", "op2");
    EXPECT_EQ(monitor.getTotalUsed(), 10000150);  // 10000000 + 150
}

// 测试 5: 极多智能体类型（大量不同 agent_type）
TEST(TokenMonitorEdgeTest, ManyDistinctAgentTypes) {
    clma::TokenMonitor monitor;
    
    for (int i = 0; i < 100; ++i) {
        monitor.recordUsage(10, 5, "agent_" + std::to_string(i), "op");
    }
    
    auto byAgent = monitor.getUsageByAgent();
    EXPECT_EQ(byAgent.size(), 100) << "Should have 100 distinct agent entries";
    
    // 每个 agent 15 tokens
    for (int i = 0; i < 100; ++i) {
        EXPECT_EQ(byAgent["agent_" + std::to_string(i)], 15);
    }
    
    // 总使用量
    EXPECT_EQ(monitor.getTotalUsed(), 1500);
}

// 测试 6: getUsageByAgent / getUsageByOperation 在空历史中
TEST(TokenMonitorEdgeTest, EmptyHistoryQueries) {
    const clma::TokenMonitor monitor;  // const 对象，无任何记录
    
    auto byAgent = monitor.getUsageByAgent();
    EXPECT_TRUE(byAgent.empty()) << "Empty monitor should have empty agent map";
    
    auto byOp = monitor.getUsageByOperation();
    EXPECT_TRUE(byOp.empty()) << "Empty monitor should have empty operation map";
    
    auto recent = monitor.getRecentUsage(10);
    EXPECT_TRUE(recent.empty()) << "Empty monitor should return empty recent list";
}

// 测试 7: getRecentUsage 的 limit 参数边界
TEST(TokenMonitorEdgeTest, RecentUsageBounds) {
    clma::TokenMonitor monitor;
    
    // 添加 3 条记录
    monitor.recordUsage(1, 1, "a", "op1");
    monitor.recordUsage(1, 1, "a", "op2");
    monitor.recordUsage(1, 1, "a", "op3");
    
    // limit=0
    auto zero = monitor.getRecentUsage(0);
    EXPECT_TRUE(zero.empty()) << "limit=0 should return empty";
    
    // limit=1
    auto one = monitor.getRecentUsage(1);
    EXPECT_EQ(one.size(), 1);
    EXPECT_EQ(one[0].operation, "op3");  // 最新的
    
    // limit=5（超出实际数量）
    auto five = monitor.getRecentUsage(5);
    EXPECT_EQ(five.size(), 3);  // 只有 3 条
}

// 测试 8: 精确预算边界（刚好为0 / 刚好用完 / 刚好超一点）
TEST(TokenMonitorEdgeTest, ExactBudgetBoundaries) {
    // 预算刚好 100
    clma::TokenMonitor monitor(100);
    
    // 刚好花完
    monitor.recordUsage(50, 50, "a", "op");
    EXPECT_FALSE(monitor.isOverBudget()) << "Exactly at budget (100/100) should not be over";
    EXPECT_EQ(monitor.getRemainingBudget(), 0);
    
    // 超 0.0000001（但 token 是整数，最小的超就是 1）
    monitor.recordUsage(1, 0, "a", "op2");
    EXPECT_TRUE(monitor.isOverBudget()) << "101/100 should be over budget";
}

// 测试 9: setBudget 后边界行为
TEST(TokenMonitorEdgeTest, SetBudgetBehaviorAtEdges) {
    clma::TokenMonitor monitor(100);
    
    monitor.recordUsage(50, 50, "a", "op");  // 100/100
    EXPECT_FALSE(monitor.isOverBudget());
    
    // 提升预算
    monitor.setBudget(200);
    EXPECT_FALSE(monitor.isOverBudget()) << "After raising budget, should no longer be over";
    EXPECT_EQ(monitor.getRemainingBudget(), 100);
    
    // 降低预算到低于当前使用量
    monitor.setBudget(30);
    EXPECT_TRUE(monitor.isOverBudget()) << "After lowering budget below usage, should be over";
    EXPECT_EQ(monitor.getRemainingBudget(), 0);
}

// 测试 10: 1000+ 历史记录精简到 MAX_HISTORY(=1000)
TEST(TokenMonitorEdgeTest, HistoryTruncationAtMax) {
    clma::TokenMonitor monitor(1000000);
    
    // 2000 条记录
    for (int i = 0; i < 2000; ++i) {
        monitor.recordUsage(1, 1, "mass", "mass_op");
    }
    
    auto recent = monitor.getRecentUsage(5000);
    // 历史应不超过 1000 条
    EXPECT_LE(recent.size(), 1000u);
    
    // 最近的记录应该是最后几条
    if (recent.size() > 0) {
        // 最后一条的总 token=2
        EXPECT_EQ(recent.back().prompt_tokens + recent.back().completion_tokens, 2);
    }
}

// 测试 11: 多次 reset 后边界
TEST(TokenMonitorEdgeTest, MultipleResetEdges) {
    clma::TokenMonitor monitor(100);
    
    monitor.recordUsage(60, 40, "a", "op");  // 100% 使用
    EXPECT_TRUE(monitor.needsWarning());
    EXPECT_EQ(monitor.getUsageRatio(), 1.0);
    
    monitor.reset();
    EXPECT_EQ(monitor.getUsageRatio(), 0.0);
    EXPECT_FALSE(monitor.needsWarning());
    EXPECT_EQ(monitor.getTotalUsed(), 0);
    
    // 再记录一次
    monitor.recordUsage(30, 20, "b", "op2");
    EXPECT_EQ(monitor.getTotalUsed(), 50);
    EXPECT_DOUBLE_EQ(monitor.getUsageRatio(), 0.5);
    
    // 再 reset
    monitor.reset();
    auto byAgent = monitor.getUsageByAgent();
    EXPECT_TRUE(byAgent.empty()) << "After reset, agent map should be empty";
}

// 测试 12: WARNING_THRESHOLD 刚好 80%
TEST(TokenMonitorEdgeTest, WarningThresholdEdge) {
    clma::TokenMonitor monitor(100);
    
    // 79% — 不警告
    monitor.recordUsage(40, 39, "a", "op");  // 79/100
    EXPECT_FALSE(monitor.needsWarning()) << "79% should NOT trigger warning";
    
    monitor.recordUsage(1, 0, "a", "op2");  // 80/100
    EXPECT_TRUE(monitor.needsWarning()) << "80% should trigger warning";
}

// 测试 13: 多种 agent_type 混合下统计的正确性
TEST(TokenMonitorEdgeTest, MixedAgentStats) {
    clma::TokenMonitor monitor;
    
    std::vector<std::pair<std::string, size_t>> records = {
        {"refiner", 100},  // prompt_tokens
        {"reasoner", 200},
        {"solver", 150},
        {"refiner", 50},   // 第二个 refiner
        {"verifier", 80},
        {"evaluator", 60},
        {"refiner", 30},   // 第三个 refiner
    };
    
    for (const auto& [agent, pt] : records) {
        monitor.recordUsage(pt, pt / 2, agent, "op");
    }
    
    auto byAgent = monitor.getUsageByAgent();
    // refiner: 100+50 + half of that = (100+50+30) + (50+25+15) = 180+90 = 270
    EXPECT_EQ(byAgent["refiner"], 270);
    EXPECT_EQ(byAgent["reasoner"], 300);  // 200 + 100
    EXPECT_EQ(byAgent["solver"], 225);    // 150 + 75
    EXPECT_EQ(byAgent["verifier"], 120);  // 80 + 40
    EXPECT_EQ(byAgent["evaluator"], 90);  // 60 + 30
    
    // 总使用量
    size_t total = 0;
    for (const auto& [_, val] : byAgent) {
        total += val;
    }
    EXPECT_EQ(monitor.getTotalUsed(), total);
}

// 测试 14: TokenUsage 结构体构造
TEST(TokenMonitorEdgeTest, TokenUsageStruct) {
    // 测试默认构造
    clma::TokenUsage defaultUsage;
    EXPECT_EQ(defaultUsage.prompt_tokens, 0);
    EXPECT_EQ(defaultUsage.completion_tokens, 0);
    EXPECT_EQ(defaultUsage.agent_type, "");
    EXPECT_EQ(defaultUsage.operation, "");
    
    // 测试参数构造
    clma::TokenUsage paramUsage(100, 50, "agent", "test_op");
    EXPECT_EQ(paramUsage.prompt_tokens, 100);
    EXPECT_EQ(paramUsage.completion_tokens, 50);
    EXPECT_EQ(paramUsage.agent_type, "agent");
    EXPECT_EQ(paramUsage.operation, "test_op");
    
    // 使用 TokenUsage 结构记录
    clma::TokenMonitor monitor;
    monitor.recordUsage(paramUsage);
    EXPECT_EQ(monitor.getTotalUsed(), 150);
    
    // 所有字段
    auto recent = monitor.getRecentUsage(1);
    ASSERT_EQ(recent.size(), 1);
    EXPECT_EQ(recent[0].prompt_tokens, 100);
    EXPECT_EQ(recent[0].completion_tokens, 50);
    EXPECT_EQ(recent[0].agent_type, "agent");
    EXPECT_EQ(recent[0].operation, "test_op");
}

// 测试 15: suggestTruncationLength 在极低/极高使用率下的行为
TEST(TokenMonitorEdgeTest, SuggestTruncationEdges) {
    // 空历史 — 默认值
    {
        clma::TokenMonitor monitor(100);
        size_t suggestion = monitor.suggestTruncationLength();
        EXPECT_GE(suggestion, 100);
        EXPECT_LE(suggestion, 5000);
    }
    
    // 接近用完 — 极小截断建议
    {
        clma::TokenMonitor monitor(100);
        monitor.recordUsage(99, 0, "a", "op");  // 99/100 = 99%
        size_t suggestion = monitor.suggestTruncationLength();
        EXPECT_LE(suggestion, 500);  // 使用率高的应建议短截断
    }
    
    // 特大预算 + 少量使用 — 最大截断建议
    {
        clma::TokenMonitor monitor(1000000);
        monitor.recordUsage(10, 5, "a", "op");  // 15/1000000 ≈ 0%
        size_t suggestion = monitor.suggestTruncationLength();
        EXPECT_GE(suggestion, 100);  // 最低下限
        EXPECT_LE(suggestion, 5000); // 最高上限
    }
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
