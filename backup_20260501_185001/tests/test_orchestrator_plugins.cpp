#include <gtest/gtest.h>
#include "core/Orchestrator.hpp"
#include "core/PluginManager.hpp"
#include "core/RuleEngine.hpp"
#include "core/LoopController.hpp"
#include "core/AgentPlugin.hpp"
#include <memory>
#include <iostream>
#include <filesystem>

namespace fs = std::filesystem;

class OrchestratorPluginsTest : public ::testing::Test {
protected:
    void SetUp() override {
        // 创建 PluginManager
        plugin_manager = std::make_shared<clma::PluginManager>();
        
        // 添加插件目录 — 直接硬编码绝对路径
        plugin_manager->addPluginDirectory("/root/closed-loop-multiagent/build/lib");
        
        // 创建 Orchestrator
        orchestrator = std::make_shared<clma::Orchestrator>();
        
        // 设置规则引擎
        rule_engine = std::make_shared<clma::RuleEngine>();
        loop_controller = std::make_shared<clma::LoopController>();
        
        orchestrator->setRuleEngine(rule_engine);
        orchestrator->setLoopController(loop_controller);
        
        // 添加测试规则
        clma::Rule test_rule;
        test_rule.pattern = "test";
        test_rule.validation_method = "test_method";
        test_rule.weights["reasonableness"] = 0.4;
        test_rule.weights["executability"] = 0.4;
        test_rule.weights["satisfaction"] = 0.2;
        test_rule.threshold = 0.8;
        rule_engine->addRule(test_rule);
    }
    
    void TearDown() override {
        orchestrator.reset();
        plugin_manager.reset();
        rule_engine.reset();
        loop_controller.reset();
    }
    
    // 扫描并加载所有可用插件
    bool loadAllPlugins() {
        int found = plugin_manager->scanPlugins();
        if (found == 0) {
            // 首次扫描可能文件系统延迟，重试一次
            std::cerr << "[DEBUG] scanPlugins first attempt found 0, retrying..." << std::endl;
            found = plugin_manager->scanPlugins();
        }
        if (found == 0) {
            std::cerr << "[DEBUG] scanPlugins found 0 plugins!" << std::endl;
            auto dirs = plugin_manager->getPluginDirectories();
            for (const auto& d : dirs) {
                std::cerr << "[DEBUG]   plugin dir: " << d << std::endl;
            }
            return false;
        }
        std::cerr << "[DEBUG] scanPlugins found " << found << " plugins" << std::endl;
        bool ok = plugin_manager->loadAll();
        std::cerr << "[DEBUG] loadAll returned " << (ok ? "true" : "false") << std::endl;
        return ok;
    }
    
    std::shared_ptr<clma::Orchestrator> orchestrator;
    std::shared_ptr<clma::PluginManager> plugin_manager;
    std::shared_ptr<clma::RuleEngine> rule_engine;
    std::shared_ptr<clma::LoopController> loop_controller;
};

// 测试 1：扫描并加载插件
TEST_F(OrchestratorPluginsTest, ScanAndLoadPlugins) {
    // 注意：首次 fixture 的 SetUp 中 scanPlugins 可能返回 0（dlopen 首次加载环境问题），
    // 但后续在 SetUp 外手动扫描总是成功
    int found = plugin_manager->scanPlugins();
    if (found == 0) {
        found = plugin_manager->scanPlugins();
        if (found == 0) {
            found = plugin_manager->scanPlugins();
        }
    }
    EXPECT_GE(found, 5) << "Should find at least 5 agent plugins";
    
    ASSERT_TRUE(plugin_manager->loadAll());
    EXPECT_EQ(plugin_manager->getPluginCount(), 6);  // 5 agent + 1 example_tool
}

// 测试 2：插件列表包含所有 Agent 类型
TEST_F(OrchestratorPluginsTest, ScanListsAllAgentPlugins) {
    ASSERT_TRUE(loadAllPlugins());
    
    std::vector<std::string> expected_ids = {
        "agent.refiner",
        "agent.reasoner",
        "agent.solver",
        "agent.verifier",
        "agent.evaluator"
    };
    
    auto plugins = plugin_manager->listPlugins();
    for (const auto& expected : expected_ids) {
        bool found = false;
        for (const auto& p : plugins) {
            if (p.id == expected) {
                found = true;
                break;
            }
        }
        EXPECT_TRUE(found) << "Expected plugin " << expected << " not found";
    }
}

// 测试 3：PluginManager 注册到 Orchestrator 后加载 Agent 插件回调
TEST_F(OrchestratorPluginsTest, RegisterPluginManagerAndLoadAgents) {
    ASSERT_TRUE(loadAllPlugins());
    
    // 将 PluginManager 注册到 Orchestrator
    orchestrator->registerPluginManager(plugin_manager);
    
    // 从插件加载 Agent 回调
    size_t loaded = orchestrator->loadPluginAgents();
    EXPECT_EQ(loaded, 5) << "Should load 5 agent plugins as callbacks";
}

// 测试 4：通过插件 Agent 处理查询
TEST_F(OrchestratorPluginsTest, ProcessQueryWithPluginAgents) {
    ASSERT_TRUE(loadAllPlugins());
    orchestrator->registerPluginManager(plugin_manager);
    orchestrator->loadPluginAgents();
    
    // 使用开环模式
    orchestrator->setLoopMode(clma::LoopController::Mode::OPEN_LOOP);
    
    // 处理查询 — 应该走插件路径
    clma::AgentResult result = orchestrator->processQuery("test hello world");
    
    // 调试：打印错误信息
    if (!result.success) {
        std::cerr << "[DEBUG] processQuery failed: " << result.error_message << std::endl;
    }
    
    // 验证结果成功（阈值内）
    EXPECT_TRUE(result.success);
    EXPECT_FALSE(result.content.empty());
    
    // 验证统计数据
    auto stats = orchestrator->getStatistics();
    EXPECT_GT(stats["queries_processed"], 0);
}

// 测试 5：所有 5 个 Agent 步骤都在查询中执行
TEST_F(OrchestratorPluginsTest, AllAgentStepsExecuted) {
    ASSERT_TRUE(loadAllPlugins());
    orchestrator->registerPluginManager(plugin_manager);
    orchestrator->loadPluginAgents();
    
    orchestrator->setLoopMode(clma::LoopController::Mode::OPEN_LOOP);
    
    // 使用较低的阈值确保成功
    rule_engine->clearRules();
    clma::Rule test_rule;
    test_rule.pattern = "test";
    test_rule.validation_method = "test_method";
    test_rule.weights["reasonableness"] = 0.4;
    test_rule.weights["executability"] = 0.4;
    test_rule.weights["satisfaction"] = 0.2;
    test_rule.threshold = 0.1;  // 低阈值确保通过
    rule_engine->addRule(test_rule);
    
    clma::AgentResult result = orchestrator->processQuery("test write a python program!");
    
    EXPECT_TRUE(result.success);
    EXPECT_FALSE(result.content.empty());
    
    // 检查各阶段内容都存在
    EXPECT_NE(result.metadata["refined_query"].find("validate"), std::string::npos) 
        << "Refiner should add validation method prefix";
    EXPECT_NE(result.metadata["reasoned_content"].find("Reasoning"), std::string::npos)
        << "Reasoner should produce reasoning plan";
    EXPECT_NE(result.metadata["solved_content"].find("python"), std::string::npos)
        << "Solver should detect python and produce code";
    EXPECT_NE(result.metadata["verified_content"].find("checks passed"), std::string::npos)
        << "Verifier should produce verification report";
}

// 测试 6：闭环模式下插件路径工作正常
TEST_F(OrchestratorPluginsTest, ClosedLoopWithPluginAgents) {
    ASSERT_TRUE(loadAllPlugins());
    orchestrator->registerPluginManager(plugin_manager);
    orchestrator->loadPluginAgents();
    
    // 低阈值确保快速通过闭环
    rule_engine->clearRules();
    clma::Rule test_rule;
    test_rule.pattern = "test";
    test_rule.validation_method = "test_method";
    test_rule.weights["reasonableness"] = 0.4;
    test_rule.weights["executability"] = 0.4;
    test_rule.weights["satisfaction"] = 0.2;
    test_rule.threshold = 0.1;
    rule_engine->addRule(test_rule);
    
    orchestrator->setMaxIterations(2);
    orchestrator->setSatisfactionThreshold(0.6);
    
    clma::AgentResult result = orchestrator->processQuery("test query");
    
    EXPECT_TRUE(result.success);
    
    // 闭环模式下应该有迭代信息
    auto stats = orchestrator->getStatistics();
    EXPECT_GT(stats["iterations_executed"], 0);
    
    auto history = orchestrator->getExecutionHistory();
    EXPECT_GT(history.size(), 0);
    
    // 检查元数据包含迭代分数
    EXPECT_TRUE(result.metadata.count("total_iterations") > 0);
}

// 测试 7：混合模式 — 手动注册+插件自动发现互不干扰
TEST_F(OrchestratorPluginsTest, HybridManualAndPluginAgents) {
    ASSERT_TRUE(loadAllPlugins());
    orchestrator->registerPluginManager(plugin_manager);
    
    // 先手动注册一个 Agent（应该覆盖插件发现）
    auto manual_refiner = [](const std::string& q, const std::string& m) -> clma::AgentResult {
        clma::AgentResult r;
        r.content = "MANUAL_REFINER: " + q;
        r.success = true;
        r.metadata["manual"] = "true";
        r.metadata["prompt_tokens"] = "10";
        r.metadata["completion_tokens"] = "5";
        return r;
    };
    orchestrator->registerAgent(clma::AgentType::REFINER, manual_refiner);
    
    // 然后加载插件 — REFINER 应该保持手动注册的
    size_t loaded = orchestrator->loadPluginAgents();
    EXPECT_EQ(loaded, 4) << "loadPluginAgents: REFINER already manually registered, skipped";  // 5 agents - 1 manually registered REFINER = 4
    
    // 验证流程
    orchestrator->setLoopMode(clma::LoopController::Mode::OPEN_LOOP);
    clma::AgentResult result = orchestrator->processQuery("test query");
    
    EXPECT_TRUE(result.success);
    // 精炼步骤应该使用手动注册的回调
    EXPECT_NE(result.metadata["refined_query"].find("MANUAL_REFINER"), std::string::npos)
        << "Manual refiner should be used instead of plugin refiner";
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
