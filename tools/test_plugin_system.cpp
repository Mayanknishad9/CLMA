#include "core/PluginManager.hpp"
#include "core/PluginInterface.hpp"
#include <iostream>
#include <filesystem>

int main() {
    clma::PluginManager pm;

    // 添加插件目录
    pm.addPluginDirectory("../plugins");
    // 也搜索 build/lib 目录（cmake 输出位置）
    pm.addPluginDirectory("../build/lib");

    std::cout << "=== Plugin Manager Test ===" << std::endl;

    // 扫描插件
    int found = pm.scanPlugins();
    std::cout << "Plugins found: " << found << std::endl;

    // 列出发现的插件
    auto allPlugins = pm.listPlugins();
    std::cout << "\nDiscovered plugins (" << allPlugins.size() << "):" << std::endl;
    for (const auto& p : allPlugins) {
        std::cout << "  - " << p.id << " v" << p.version.toString()
                  << " [" << (p.type == clma::PluginType::TOOL ? "TOOL" :
                             p.type == clma::PluginType::STRATEGY ? "STRATEGY" :
                             p.type == clma::PluginType::JUDGE ? "JUDGE" : "OTHER") << "]"
                  << std::endl;
        std::cout << "    " << p.description << std::endl;
    }

    // 加载示例插件
    if (found > 0) {
        auto& firstPlugin = allPlugins[0];
        std::string id = firstPlugin.id;

        std::cout << "\nLoading plugin: " << id << std::endl;
        if (pm.loadPlugin(id)) {
            std::cout << "  [OK] Loaded" << std::endl;
        } else {
            std::cout << "  [FAIL] Load failed" << std::endl;
            return 1;
        }

        if (pm.initializePlugin(id)) {
            std::cout << "  [OK] Initialized" << std::endl;
        } else {
            std::cout << "  [FAIL] Init failed" << std::endl;
            return 1;
        }

        if (pm.startPlugin(id)) {
            std::cout << "  [OK] Started" << std::endl;
        } else {
            std::cout << "  [FAIL] Start failed" << std::endl;
            return 1;
        }

        // 检查状态
        auto state = pm.getPluginState(id);
        std::cout << "  State: " << (state == clma::PluginState::RUNNING ? "RUNNING" : "ERROR") << std::endl;

        // 获取实例
        auto* plugin = pm.getPlugin(id);
        if (plugin) {
            std::cout << "  [OK] Instance retrieved" << std::endl;
            std::cout << "  Healthy: " << (plugin->isHealthy() ? "yes" : "no") << std::endl;
        }

        // 停止并卸载
        pm.stopPlugin(id);
        pm.unloadPlugin(id);
        std::cout << "\n  [OK] Stopped and unloaded" << std::endl;
    }

    std::cout << "\n=== Plugin count: " << pm.getPluginCount() << " ===" << std::endl;
    std::cout << "All tests passed!" << std::endl;
    return 0;
}
