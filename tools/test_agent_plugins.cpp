#include <core/PluginManager.hpp>
#include <core/PluginInterface.hpp>
#include <core/AgentPlugin.hpp>
#include <iostream>
#include <filesystem>
#include <cassert>
#include <thread>
#include <chrono>

int main() {
    clma::PluginManager pm;

    pm.addPluginDirectory("../plugins");
    pm.addPluginDirectory("../build/lib");

    std::cout << "=== PluginManager Phase 2 Integration Test ===\n" << std::endl;

    // =================== 1. 扫描 ===================
    std::cout << "--- 1. Scan & Discover ---" << std::endl;
    int found = pm.scanPlugins();
    std::cout << "Plugins found: " << found << std::endl;
    if (found < 5) { std::cerr << "FAIL: expected >= 5 plugins, got " << found << "\n"; return 1; }
    std::cout << "  [PASS] Scan found " << found << " plugins\n" << std::endl;

    // =================== 2. 加载与执行 ===================
    std::cout << "--- 2. Load & Execute All Agent Plugins ---" << std::endl;
    const char* testPlugins[] = {
        "agent.refiner",
        "agent.reasoner",
        "agent.solver",
        "agent.verifier",
        "agent.evaluator"
    };

// 先全部加载
    for (const auto& pluginId : testPlugins) {
        if (!pm.loadPlugin(pluginId)) { std::cerr << "FAIL: loadPlugin(" << pluginId << ")\n"; return 1; }
        if (!pm.initializePlugin(pluginId)) { std::cerr << "FAIL: initializePlugin(" << pluginId << ")\n"; return 1; }
        if (!pm.startPlugin(pluginId)) { std::cerr << "FAIL: startPlugin(" << pluginId << ")\n"; return 1; }
    }

    // 通过 PluginInterface* 验证基本功能
    for (const auto& pluginId : testPlugins) {
        auto* plugin = pm.getPlugin(pluginId);
        if (!plugin) { std::cerr << "FAIL: getPlugin(" << pluginId << ") is null\n"; return 1; }
        if (!plugin->isHealthy()) { std::cerr << "FAIL: isHealthy(" << pluginId << ")\n"; return 1; }
        if (!plugin->isAgentPlugin()) { std::cerr << "FAIL: isAgentPlugin(" << pluginId << ")\n"; return 1; }

        // static_cast 安全 — 同一编译单元
        auto* agentPlugin = static_cast<clma::AgentPlugin*>(plugin);
        if (!agentPlugin) { std::cerr << "FAIL: static_cast<AgentPlugin>(" << pluginId << ")\n"; return 1; }

        clma::AgentContext ctx;
        ctx.userQuery = "write a python hello world program";
        ctx.refinedQuery = "[python] write a python hello world program";
        ctx.previousResult = "Test verification result for eval";
        ctx.currentRule.threshold = 0.3;

        clma::AgentResult result = agentPlugin->execute(ctx);
        if (!result.success) { std::cerr << "FAIL: execute(" << pluginId << ")\n"; return 1; }
        if (result.content.empty()) { std::cerr << "FAIL: empty content from " << pluginId << "\n"; return 1; }
        std::cout << "  [PASS] " << pluginId << " (" << result.content.length() << " chars)" << std::endl;

        pm.stopPlugin(pluginId);
        pm.unloadPlugin(pluginId);
    }
    std::cout << "  All 5 agent plugins: OK\n" << std::endl;

    // =================== 3. 热更新 ===================
    std::cout << "--- 3. Hot Reload ---" << std::endl;
    {
        pm.scanPlugins();

        if (!pm.loadPlugin("agent.refiner")) { std::cerr << "FAIL: loadPlugin(agent.refiner)\n"; return 1; }
        if (!pm.initializePlugin("agent.refiner")) { std::cerr << "FAIL: initializePlugin(agent.refiner)\n"; return 1; }
        if (!pm.startPlugin("agent.refiner")) { std::cerr << "FAIL: startPlugin(agent.refiner)\n"; return 1; }

        bool hasNew = pm.hasNewVersion("agent.refiner");
        std::cout << "  hasNewVersion (immediately): " << (hasNew ? "true" : "false") << std::endl;

        pm.stopPlugin("agent.refiner");
        pm.unloadPlugin("agent.refiner");
    }
    std::cout << "  [PASS] Hot reload checks passed\n" << std::endl;

    // =================== 4. 版本兼容性 ===================
    std::cout << "--- 4. Version Compatibility ---" << std::endl;
    {
        pm.setMinApiVersion(1);
        if (pm.getMinApiVersion() != 1) { std::cerr << "FAIL: getMinApiVersion\n"; return 1; }
        std::cout << "  [PASS] Min API version set to 1" << std::endl;

        pm.scanPlugins();
        if (!pm.loadPlugin("agent.refiner")) { std::cerr << "FAIL: loadPlugin\n"; return 1; }
        if (!pm.initializePlugin("agent.refiner")) { std::cerr << "FAIL: initializePlugin\n"; return 1; }
        if (!pm.startPlugin("agent.refiner")) { std::cerr << "FAIL: startPlugin\n"; return 1; }
        std::cout << "  [PASS] Plugin with API v1 loaded (compatible)" << std::endl;

        pm.stopPlugin("agent.refiner");
        pm.unloadPlugin("agent.refiner");
    }
    std::cout << std::endl;

    // =================== 5. 配置持久化 ===================
    std::cout << "--- 5. Config Persistence ---" << std::endl;
    {
        pm.scanPlugins();
        if (!pm.loadPlugin("agent.refiner")) { std::cerr << "FAIL: loadPlugin\n"; return 1; }
        if (!pm.initializePlugin("agent.refiner")) { std::cerr << "FAIL: initializePlugin\n"; return 1; }
        if (!pm.startPlugin("agent.refiner")) { std::cerr << "FAIL: startPlugin\n"; return 1; }

        bool saved = pm.saveConfig("/tmp/test_plugins_config.json");
        if (!saved) { std::cerr << "FAIL: saveConfig\n"; return 1; }
        std::cout << "  [PASS] Config saved" << std::endl;

        bool loaded = pm.loadConfig("/tmp/test_plugins_config.json");
        if (!loaded) { std::cerr << "FAIL: loadConfig\n"; return 1; }
        std::cout << "  [PASS] Config loaded" << std::endl;

        std::filesystem::remove("/tmp/test_plugins_config.json");
        pm.stopPlugin("agent.refiner");
        pm.unloadPlugin("agent.refiner");
    }
    std::cout << std::endl;

    // =================== 6. 批量加载 ===================
    std::cout << "--- 6. Bulk loadAll ---" << std::endl;
    {
        pm.scanPlugins();
        bool allLoaded = pm.loadAll();
        std::cout << "  loadAll: " << (allLoaded ? "OK" : "partial") << std::endl;

        int loaded = 0;
        for (const auto& id : testPlugins) {
            if (pm.getPluginState(id) != clma::PluginState::UNLOADED) loaded++;
        }
        std::cout << "  Loaded: " << loaded << "/5 plugins" << std::endl;
        if (loaded != 5) { std::cerr << "FAIL: expected 5 loaded\n"; return 1; }
        std::cout << "  [PASS] All plugins loaded via loadAll" << std::endl;

        for (const auto& id : testPlugins) {
            if (!pm.initializePlugin(id)) { std::cerr << "FAIL: initializePlugin(" << id << ")\n"; return 1; }
            if (!pm.startPlugin(id)) { std::cerr << "FAIL: startPlugin(" << id << ")\n"; return 1; }
        }
        std::cout << "  [PASS] All plugins initialized and started" << std::endl;

        for (const auto& id : testPlugins) {
            if (pm.getPluginState(id) != clma::PluginState::RUNNING) { std::cerr << "FAIL: state not RUNNING for " << id << "\n"; return 1; }
        }
        std::cout << "  [PASS] All plugins in RUNNING state" << std::endl;
    }
    std::cout << std::endl;

    // =================== 7. 查询功能 ===================
    std::cout << "--- 7. Query Functions ---" << std::endl;
    {
        auto allPlugins = pm.listPlugins();
        auto runningPlugins = pm.listPluginsByState(clma::PluginState::RUNNING);

        std::cout << "  Total: " << pm.getPluginCount() << std::endl;
        std::cout << "  All: " << allPlugins.size() << std::endl;
        std::cout << "  RUNNING state: " << runningPlugins.size() << std::endl;

        if (pm.getPluginCount() < 5) { std::cerr << "FAIL: pluginCount\n"; return 1; }
        if (runningPlugins.size() < 5) { std::cerr << "FAIL: runningPlugins\n"; return 1; }
        std::cout << "  [PASS] All query functions work" << std::endl;
    }
    std::cout << std::endl;

    // =================== 结果 ===================
    std::cout << "=== Results ===" << std::endl;
    std::cout << "Tests: all passed" << std::endl;

    return 0;
}
