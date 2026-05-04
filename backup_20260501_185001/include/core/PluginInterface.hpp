#ifndef CLM_PLUGIN_INTERFACE_HPP
#define CLM_PLUGIN_INTERFACE_HPP

#include <string>
#include <memory>
#include <vector>
#include <any>

namespace clma {

// 插件类型枚举
enum class PluginType {
    TOOL,
    STRATEGY,
    JUDGE,
    PROVIDER,
    CUSTOM
};

// 插件版本
struct PluginVersion {
    int major = 0;
    int minor = 0;
    int patch = 0;

    bool operator==(const PluginVersion& other) const {
        return major == other.major && minor == other.minor && patch == other.patch;
    }
    bool operator<(const PluginVersion& other) const {
        if (major != other.major) return major < other.major;
        if (minor != other.minor) return minor < other.minor;
        return patch < other.patch;
    }
    std::string toString() const {
        return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
    }
};

// 插件元数据（从 .so 导出信息）
struct PluginInfo {
    std::string id;           // 唯一标识符，如 "tool.code_executor"
    std::string name;         // 人类可读名称
    PluginVersion version;
    PluginType type;
    std::string author;
    std::string description;
    std::vector<std::string> dependencies;  // 依赖的插件ID列表
    std::string license;
    int apiVersion = 1;       // 插件API版本，用于兼容性检查
};

// 插件生命周期状态
enum class PluginState {
    UNLOADED,
    LOADED,
    INITIALIZED,
    RUNNING,
    ERROR,
    UNLOADING
};

// 插件事件
enum class PluginEvent {
    BEFORE_LOAD,
    AFTER_LOAD,
    BEFORE_INIT,
    AFTER_INIT,
    BEFORE_EXEC,
    AFTER_EXEC,
    BEFORE_UNLOAD,
    AFTER_UNLOAD,
    ERROR_OCCURRED
};

// 插件事件监听器
class PluginEventListener {
public:
    virtual ~PluginEventListener() = default;
    virtual void onPluginEvent(PluginEvent event, const std::string& pluginId) = 0;
};

// 插件基类 — 所有插件必须继承此类
class PluginInterface {
public:
    virtual ~PluginInterface() = default;

    // === 元数据 ===
    virtual PluginInfo getInfo() const = 0;

    // === 生命周期 ===
    virtual bool initialize(std::any config = {}) = 0;
    virtual bool start() = 0;
    virtual void stop() = 0;
    virtual void shutdown() = 0;

    // === 运行时状态 ===
    virtual PluginState getState() const = 0;
    virtual bool isHealthy() const = 0;

    // === 配置 ===
    virtual bool configure(const std::string& key, std::any value) = 0;
    virtual std::any getConfig(const std::string& key) const = 0;

    // === 错误处理 ===
    virtual std::string getLastError() const = 0;
    virtual void clearError() = 0;

    // === 事件 ===
    virtual void setEventListener(PluginEventListener* listener) = 0;

    // === RTTI 替代 ===
    // 子类可覆盖以标识具体类型（避免跨 .so dynamic_cast）
    virtual bool isAgentPlugin() const { return false; }
};

// 插件工厂函数类型（每个 .so 导出此函数）
using CreatePluginFunc = PluginInterface* (*)();
using DestroyPluginFunc = void (*)(PluginInterface*);

// 插件描述函数（每个 .so 导出此函数，用于快速查询）
using GetPluginInfoFunc = PluginInfo (*)();

} // namespace clma

#endif // CLM_PLUGIN_INTERFACE_HPP
