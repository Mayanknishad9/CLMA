#include "core/AgentPlugin.hpp"

// AgentPlugin 的默认虚函数实现
// 放在 .cpp 中确保所有编译单元使用同一个 vtable layout

namespace clma {

AgentPlugin::~AgentPlugin() = default;

bool AgentPlugin::initialize(std::any config) {
    state_ = PluginState::INITIALIZED;
    return true;
}

bool AgentPlugin::start() {
    state_ = PluginState::RUNNING;
    return true;
}

void AgentPlugin::stop() {
    state_ = PluginState::LOADED;
}

void AgentPlugin::shutdown() {
    state_ = PluginState::UNLOADED;
}

PluginState AgentPlugin::getState() const {
    return state_;
}

bool AgentPlugin::isHealthy() const {
    return state_ == PluginState::RUNNING;
}

bool AgentPlugin::configure(const std::string& key, std::any value) {
    return true;
}

std::any AgentPlugin::getConfig(const std::string& key) const {
    return {};
}

std::string AgentPlugin::getLastError() const {
    return lastError_;
}

void AgentPlugin::clearError() {
    lastError_.clear();
}

void AgentPlugin::setEventListener(PluginEventListener* listener) {
    listener_ = listener;
}

bool AgentPlugin::isAgentPlugin() const {
    return true;
}

} // namespace clma
