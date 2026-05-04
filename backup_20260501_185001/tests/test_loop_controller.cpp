#include <gtest/gtest.h>
#include "core/LoopController.hpp"
#include <thread>
#include <chrono>

TEST(LoopControllerTest, DefaultConstructor) {
    clma::LoopController controller;
    
    EXPECT_EQ(controller.getMode(), clma::LoopController::Mode::CLOSED_LOOP);
    EXPECT_EQ(controller.getIterationCount(), 0);
    EXPECT_FALSE(controller.shouldStop());
    EXPECT_TRUE(controller.shouldContinue()); // 闭环模式且未达到阈值，应该继续
    
    auto best_score = controller.getBestScore();
    EXPECT_DOUBLE_EQ(best_score.reasonableness, 0.0);
    EXPECT_DOUBLE_EQ(best_score.executability, 0.0);
    EXPECT_DOUBLE_EQ(best_score.satisfaction, 0.0);
    
    EXPECT_EQ(controller.getTotalTokenUsage(), 0);
}

TEST(LoopControllerTest, ModeSwitch) {
    clma::LoopController controller;
    
    // 默认是闭环模式
    EXPECT_EQ(controller.getMode(), clma::LoopController::Mode::CLOSED_LOOP);
    EXPECT_TRUE(controller.shouldContinue()); // 闭环模式应该继续
    
    // 切换到开环模式
    controller.setMode(clma::LoopController::Mode::OPEN_LOOP);
    EXPECT_EQ(controller.getMode(), clma::LoopController::Mode::OPEN_LOOP);
    EXPECT_FALSE(controller.shouldContinue()); // 开环模式不应该继续
    
    // 切换回闭环模式
    controller.setMode(clma::LoopController::Mode::CLOSED_LOOP);
    EXPECT_EQ(controller.getMode(), clma::LoopController::Mode::CLOSED_LOOP);
}

TEST(LoopControllerTest, AddIterationResult) {
    clma::LoopController controller;
    
    clma::EvaluationScore score1{0.7, 0.8, 0.6};
    clma::TokenUsage token_usage1(100, 50, "orchestrator", "iteration1");
    
    controller.addIterationResult(score1, token_usage1);
    EXPECT_EQ(controller.getIterationCount(), 1);
    EXPECT_EQ(controller.getTotalTokenUsage(), 150);
    
    clma::EvaluationScore score2{0.8, 0.9, 0.7};
    clma::TokenUsage token_usage2(120, 60, "orchestrator", "iteration2");
    
    controller.addIterationResult(score2, token_usage2);
    EXPECT_EQ(controller.getIterationCount(), 2);
    EXPECT_EQ(controller.getTotalTokenUsage(), 330); // 150 + 180
}

TEST(LoopControllerTest, BestScoreTracking) {
    clma::LoopController controller;
    
    clma::EvaluationScore score1{0.6, 0.7, 0.5}; // overall: 0.6
    clma::TokenUsage token_usage1(100, 50, "test", "test");
    controller.addIterationResult(score1, token_usage1);
    
    auto best1 = controller.getBestScore();
    EXPECT_DOUBLE_EQ(best1.overall(), 0.6);
    
    clma::EvaluationScore score2{0.8, 0.9, 0.7}; // overall: 0.8
    clma::TokenUsage token_usage2(100, 50, "test", "test");
    controller.addIterationResult(score2, token_usage2);
    
    auto best2 = controller.getBestScore();
    EXPECT_DOUBLE_EQ(best2.overall(), 0.8); // 应该更新为更好的分数
    
    clma::EvaluationScore score3{0.7, 0.8, 0.6}; // overall: 0.7
    clma::TokenUsage token_usage3(100, 50, "test", "test");
    controller.addIterationResult(score3, token_usage3);
    
    auto best3 = controller.getBestScore();
    EXPECT_DOUBLE_EQ(best3.overall(), 0.8); // 应该保持最佳分数
}

TEST(LoopControllerTest, AverageScore) {
    clma::LoopController controller;
    
    clma::EvaluationScore score1{0.6, 0.7, 0.5};
    clma::TokenUsage token_usage1(100, 50, "test", "test");
    controller.addIterationResult(score1, token_usage1);
    
    auto avg1 = controller.getAverageScore();
    EXPECT_DOUBLE_EQ(avg1.reasonableness, 0.6);
    EXPECT_DOUBLE_EQ(avg1.executability, 0.7);
    EXPECT_DOUBLE_EQ(avg1.satisfaction, 0.5);
    
    clma::EvaluationScore score2{0.8, 0.9, 0.7};
    clma::TokenUsage token_usage2(100, 50, "test", "test");
    controller.addIterationResult(score2, token_usage2);
    
    auto avg2 = controller.getAverageScore();
    EXPECT_DOUBLE_EQ(avg2.reasonableness, 0.7); // (0.6+0.8)/2
    EXPECT_DOUBLE_EQ(avg2.executability, 0.8); // (0.7+0.9)/2
    EXPECT_DOUBLE_EQ(avg2.satisfaction, 0.6); // (0.5+0.7)/2
}

TEST(LoopControllerTest, ShouldStopMaxIterations) {
    clma::LoopController controller;
    controller.setMaxIterations(3);
    
    EXPECT_FALSE(controller.shouldStop());
    
    // 添加3次迭代
    for (int i = 0; i < 3; ++i) {
        clma::EvaluationScore score{0.7, 0.8, 0.6};
        clma::TokenUsage token_usage(100, 50, "test", "test");
        controller.addIterationResult(score, token_usage);
    }
    
    EXPECT_EQ(controller.getIterationCount(), 3);
    EXPECT_TRUE(controller.shouldStop()); // 达到最大迭代次数
    
    // 即使添加更多迭代，计数也不会超过最大值
    clma::EvaluationScore extra_score{0.8, 0.9, 0.7};
    clma::TokenUsage extra_token_usage(100, 50, "test", "test");
    controller.addIterationResult(extra_score, extra_token_usage);
    EXPECT_EQ(controller.getIterationCount(), 4); // 但仍然会增加
    EXPECT_TRUE(controller.shouldStop()); // 仍然应该停止
}

TEST(LoopControllerTest, ShouldContinueThreshold) {
    clma::LoopController controller;
    controller.setSatisfactionThreshold(0.8);
    
    // 添加一个低于阈值的分数
    clma::EvaluationScore low_score{0.7, 0.7, 0.7}; // overall: 0.7
    clma::TokenUsage token_usage(100, 50, "test", "test");
    controller.addIterationResult(low_score, token_usage);
    
    EXPECT_TRUE(controller.shouldContinue()); // 低于阈值，应该继续
    
    // 添加一个达到阈值的分数
    clma::EvaluationScore good_score{0.85, 0.9, 0.85}; // overall: 0.866...
    clma::TokenUsage token_usage2(100, 50, "test", "test");
    controller.addIterationResult(good_score, token_usage2);
    
    EXPECT_FALSE(controller.shouldContinue()); // 达到阈值，不应该继续
}

TEST(LoopControllerTest, OpenLoopModeShouldNotContinue) {
    clma::LoopController controller;
    controller.setMode(clma::LoopController::Mode::OPEN_LOOP);
    
    // 即使分数低于阈值，开环模式也不应该继续
    clma::EvaluationScore low_score{0.5, 0.5, 0.5};
    clma::TokenUsage token_usage(100, 50, "test", "test");
    controller.addIterationResult(low_score, token_usage);
    
    EXPECT_FALSE(controller.shouldContinue()); // 开环模式只执行一次
}

TEST(LoopControllerTest, Reset) {
    clma::LoopController controller;
    
    // 添加一些数据
    for (int i = 0; i < 5; ++i) {
        clma::EvaluationScore score{0.7, 0.8, 0.6};
        clma::TokenUsage token_usage(100, 50, "test", "test");
        controller.addIterationResult(score, token_usage);
    }
    
    EXPECT_EQ(controller.getIterationCount(), 5);
    EXPECT_GT(controller.getTotalTokenUsage(), 0);
    
    controller.reset();
    
    EXPECT_EQ(controller.getIterationCount(), 0);
    EXPECT_EQ(controller.getTotalTokenUsage(), 0);
    EXPECT_FALSE(controller.shouldStop());
}

TEST(LoopControllerTest, SetTokenBudget) {
    clma::LoopController controller;
    
    controller.setTokenBudget(500);
    EXPECT_EQ(controller.getRemainingTokenBudget(), 500);
    
    // 使用一些token
    clma::EvaluationScore score{0.7, 0.8, 0.6};
    clma::TokenUsage token_usage(200, 100, "test", "test");
    controller.addIterationResult(score, token_usage);
    
    EXPECT_EQ(controller.getRemainingTokenBudget(), 200); // 500 - 300
    EXPECT_FALSE(controller.shouldStop()); // 未超预算
    
    // 使用更多token，超过预算
    clma::TokenUsage token_usage2(200, 100, "test", "test");
    controller.addIterationResult(score, token_usage2);
    
    EXPECT_TRUE(controller.shouldStop()); // 应该停止（超预算）
}

TEST(LoopControllerTest, HasConverged) {
    clma::LoopController controller;
    
    // 添加逐步改进的分数
    std::vector<double> scores = {0.5, 0.55, 0.57, 0.58, 0.585, 0.586};
    for (double overall : scores) {
        clma::EvaluationScore score{overall, overall, overall};
        clma::TokenUsage token_usage(100, 50, "test", "test");
        controller.addIterationResult(score, token_usage);
    }
    
    // 最后几次迭代改进很小，应该收敛
    EXPECT_TRUE(controller.hasConverged(3, 0.02)); // 最后3次迭代改进小于2%
    
    // 添加一个大的改进
    clma::EvaluationScore big_improvement{0.8, 0.8, 0.8};
    clma::TokenUsage token_usage(100, 50, "test", "test");
    controller.addIterationResult(big_improvement, token_usage);
    
    EXPECT_FALSE(controller.hasConverged(3, 0.02)); // 不再收敛
}

TEST(LoopControllerTest, Timeout) {
    clma::LoopController controller;
    controller.setTimeoutSeconds(1); // 设置1秒超时
    
    EXPECT_FALSE(controller.isTimeout());
    
    // 等待超过1秒
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    
    EXPECT_TRUE(controller.isTimeout());
    EXPECT_TRUE(controller.shouldStop()); // 超时应该导致停止
}

TEST(LoopControllerTest, IterationHistory) {
    clma::LoopController controller;
    
    std::vector<clma::EvaluationScore> scores = {
        {0.6, 0.7, 0.5},
        {0.7, 0.8, 0.6},
        {0.8, 0.9, 0.7}
    };
    
    for (const auto& score : scores) {
        clma::TokenUsage token_usage(100, 50, "test", "test");
        controller.addIterationResult(score, token_usage);
    }
    
    const auto& history = controller.getIterationHistory();
    EXPECT_EQ(history.size(), 3);
    
    for (size_t i = 0; i < history.size(); ++i) {
        EXPECT_DOUBLE_EQ(history[i].first.reasonableness, scores[i].reasonableness);
        EXPECT_DOUBLE_EQ(history[i].first.executability, scores[i].executability);
        EXPECT_DOUBLE_EQ(history[i].first.satisfaction, scores[i].satisfaction);
        EXPECT_EQ(history[i].second.total_tokens, 150);
    }
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}