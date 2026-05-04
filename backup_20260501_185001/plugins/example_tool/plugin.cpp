#include "core/PluginInterface.hpp"
#include <iostream>
#include <unordered_map>
#include <any>

namespace clma {

class ExampleToolPlugin : public PluginInterface {
public:
    ExampleToolPlugin() = default;
    ~ExampleToolPlugin() override = default;

    PluginInfo getInfo() const override {
        return PluginInfo{
            "tool.example_tool",
            "Example Tool Plugin",
            {1, 0, 0},
            PluginType::TOOL,
            "CLMA Team",
            "An example plugin demonstrating the plugin system",
            {},
            "MIT",
            1
        };
    }

    bool initialize(std::any config) override {
        state_ = PluginState::INITIALIZED;
        std::cout << "[ExampleTool] Initialized" << std::endl;
        return true;
    }

    bool start() override {
        state_ = PluginState::RUNNING;
        std::cout << "[ExampleTool] Started" << std::endl;
        return true;
    }

    void stop() override {
        state_ = PluginState::LOADED;
        std::cout << "[ExampleTool] Stopped" << std::endl;
    }

    void shutdown() override {
        state_ = PluginState::UNLOADED;
        std::cout << "[ExampleTool] Shutdown" << std::endl;
    }

    PluginState getState() const override {
        return state_;
    }

    bool isHealthy() const override {
        return state_ == PluginState::RUNNING || state_ == PluginState::INITIALIZED;
    }

    bool configure(const std::string& key, std::any value) override {
        config_[key] = value;
        return true;
    }

    std::any getConfig(const std::string& key) const override {
        auto it = config_.find(key);
        if (it != config_.end()) return it->second;
        return {};
    }

    std::string getLastError() const override {
        return lastError_;
    }

    void clearError() override {
        lastError_.clear();
    }

    void setEventListener(PluginEventListener* listener) override {
        listener_ = listener;
    }

private:
    PluginState state_ = PluginState::UNLOADED;
    PluginEventListener* listener_ = nullptr;
    std::unordered_map<std::string, std::any> config_;
    std::string lastError_;
};

} // namespace clma

// === 导出符号 ===

extern "C" {

clma::PluginInterface* createPlugin() {
    return new clma::ExampleToolPlugin();
}

void destroyPlugin(clma::PluginInterface* plugin) {
    delete plugin;
}

clma::PluginInfo getPluginInfo() {
    return clma::ExampleToolPlugin().getInfo();
}

} // extern "C"
