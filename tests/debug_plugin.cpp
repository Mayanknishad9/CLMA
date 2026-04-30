#include <gtest/gtest.h>
#include "core/PluginManager.hpp"
#include <iostream>
#include <filesystem>

namespace fs = std::filesystem;

TEST(DebugPluginTest, ScanAndLoad) {
    auto pm = std::make_shared<clma::PluginManager>();
    
    const char* build_dir = std::getenv("CLMA_BUILD_DIR");
    std::string lib_dir;
    if (build_dir) {
        lib_dir = std::string(build_dir) + "/lib";
    } else {
        lib_dir = (fs::current_path() / "lib").string();
    }
    
    std::cerr << "Adding plugin directory: " << lib_dir << std::endl;
    pm->addPluginDirectory(lib_dir);
    
    int found = pm->scanPlugins();
    std::cerr << "scanPlugins found: " << found << std::endl;
    
    auto plugins = pm->listPlugins();
    std::cerr << "listPlugins count: " << plugins.size() << std::endl;
    for (const auto& p : plugins) {
        std::cerr << "  - " << p.id << " (" << p.name << ")" << std::endl;
        std::cerr << "    loaded: " << pm->isPluginLoaded(p.id) << std::endl;
    }
    
    bool loaded = pm->loadAll();
    std::cerr << "loadAll returned: " << (loaded ? "true" : "false") << std::endl;
    
    plugins = pm->listPlugins();
    for (const auto& p : plugins) {
        std::cerr << "  after load: " << p.id << " loaded=" << pm->isPluginLoaded(p.id) << std::endl;
    }
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
