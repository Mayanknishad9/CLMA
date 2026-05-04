#include <gtest/gtest.h>
#include "core/Types.hpp"

TEST(EvaluationScoreTest, DefaultConstructor) {
    clma::EvaluationScore score;
    EXPECT_DOUBLE_EQ(score.reasonableness, 0.0);
    EXPECT_DOUBLE_EQ(score.executability, 0.0);
    EXPECT_DOUBLE_EQ(score.satisfaction, 0.0);
    EXPECT_DOUBLE_EQ(score.overall(), 0.0);
}

TEST(EvaluationScoreTest, OverallCalculation) {
    clma::EvaluationScore score{0.8, 0.7, 0.9};
    EXPECT_DOUBLE_EQ(score.overall(), (0.8 + 0.7 + 0.9) / 3.0);
}

TEST(EvaluationScoreTest, MeetsThreshold) {
    clma::EvaluationScore score{0.85, 0.9, 0.8};
    EXPECT_TRUE(score.meetsThreshold(0.8));
    EXPECT_FALSE(score.meetsThreshold(0.9));
}

TEST(EvaluationScoreTest, PartialThreshold) {
    clma::EvaluationScore score{0.9, 0.7, 0.8};
    EXPECT_TRUE(score.meetsThreshold(0.8));  // overall = (0.9+0.7+0.8)/3 = 0.8 >= 0.8
}

TEST(RuleTest, DefaultConstructor) {
    clma::Rule rule;
    EXPECT_TRUE(rule.pattern.empty());
    EXPECT_TRUE(rule.validation_method.empty());
    EXPECT_TRUE(rule.recommended_tools.empty());
    EXPECT_TRUE(rule.weights.empty());
    EXPECT_DOUBLE_EQ(rule.threshold, 0.0);
}

TEST(RuleTest, PatternMatching) {
    clma::Rule rule;
    rule.pattern = "write";
    
    EXPECT_TRUE(rule.matches("write a program"));
    EXPECT_TRUE(rule.matches("I need to write code"));
    EXPECT_TRUE(rule.matches("WRITE something"));  // case-insensitive
    EXPECT_FALSE(rule.matches("read a book"));
    EXPECT_FALSE(rule.matches(""));
}

TEST(RuleTest, PatternMatchingComplex) {
    clma::Rule rule;
    rule.pattern = "analyze data";
    
    EXPECT_TRUE(rule.matches("analyze data set"));
    EXPECT_TRUE(rule.matches("we need to analyze data"));
    EXPECT_FALSE(rule.matches("analyze code"));
}

TEST(TokenUsageTest, DefaultConstructor) {
    clma::TokenUsage usage;
    EXPECT_EQ(usage.prompt_tokens, 0);
    EXPECT_EQ(usage.completion_tokens, 0);
    EXPECT_EQ(usage.total_tokens, 0);
    EXPECT_TRUE(usage.agent_type.empty());
    EXPECT_TRUE(usage.operation.empty());
}

TEST(TokenUsageTest, ParameterizedConstructor) {
    clma::TokenUsage usage(100, 200, "refiner", "refine_query");
    EXPECT_EQ(usage.prompt_tokens, 100);
    EXPECT_EQ(usage.completion_tokens, 200);
    EXPECT_EQ(usage.total_tokens, 300);
    EXPECT_EQ(usage.agent_type, "refiner");
    EXPECT_EQ(usage.operation, "refine_query");
}

TEST(AgentResultTest, DefaultConstructor) {
    clma::AgentResult result;
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.content.empty());
    EXPECT_TRUE(result.metadata.empty());
    EXPECT_DOUBLE_EQ(result.score.reasonableness, 0.0);
    EXPECT_DOUBLE_EQ(result.score.executability, 0.0);
    EXPECT_DOUBLE_EQ(result.score.satisfaction, 0.0);
    EXPECT_TRUE(result.error_message.empty());
}

TEST(EnumTest, AgentTypeValues) {
    EXPECT_EQ(static_cast<int>(clma::AgentType::REFINER), 0);
    EXPECT_EQ(static_cast<int>(clma::AgentType::REASONER), 1);
    EXPECT_EQ(static_cast<int>(clma::AgentType::SOLVER), 2);
    EXPECT_EQ(static_cast<int>(clma::AgentType::VERIFIER), 3);
    EXPECT_EQ(static_cast<int>(clma::AgentType::EVALUATOR), 4);
}

TEST(EnumTest, AgentStateValues) {
    EXPECT_EQ(static_cast<int>(clma::AgentState::IDLE), 0);
    EXPECT_EQ(static_cast<int>(clma::AgentState::PROCESSING), 1);
    EXPECT_EQ(static_cast<int>(clma::AgentState::SUCCESS), 2);
    EXPECT_EQ(static_cast<int>(clma::AgentState::FAILED), 3);
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}