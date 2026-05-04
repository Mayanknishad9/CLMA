#ifndef CLM_PLUGIN_MANAGER_HPP
#define CLM_PLUGIN_MANAGER_HPP

#include "core/PluginInterface.hpp"
#include <string>
#include <memory>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <mutex>
#include <atomic>
#include <filesystem>
#include <dlfcn.h>
#include <fstream>
#include <chrono>
#include <map>

namespace clma {

/**
 * PluginManager — 插件管理器
 *
 * 职责：
 * 1. 插件扫描、加载、卸载、热更新
 * 2. 依赖管理（拓扑排序、自动加载、循环检测）
 * 3. 生命周期事件通知
 * 4. 版本兼容性检查
 * 5. 配置持久化
 */
class PluginManager : public PluginEventListener {
public:
    PluginManager();
    ~PluginManager() override;

    // 禁用拷贝
    PluginManager(const PluginManager&) = delete;
    PluginManager& operator=(const PluginManager&) = delete;

    // ==================== 目录管理 ====================
    void addPluginDirectory(const std::string& path);
    void clearPluginDirectories();
    std::vector<std::string> getPluginDirectories() const;

    // ==================== 扫描与加载 ====================
    int scanPlugins();                              // 扫描目录，发现可用插件
    bool loadPlugin(const std::string& pluginId);   // 加载单个插件（自动解决依赖）
    bool loadAll();                                 // 加载所有已发现的插件
    bool unloadPlugin(const std::string& pluginId); // 卸载单个插件（含依赖它的插件）
    void unloadAll();                               // 卸载所有插件

    // ==================== 初始化与启动 ====================
    bool initializePlugin(const std::string& pluginId, std::any config = {});
    bool startPlugin(const std::string& pluginId);
    bool stopPlugin(const std::string& pluginId);

    // ==================== 查询 ====================
    PluginInterface* getPlugin(const std::string& pluginId) const;
    void releasePlugin(const std::string& pluginId);  // 释放引用计数
    std::vector<PluginInfo> listPlugins(PluginType type = static_cast<PluginType>(-1)) const;
    std::vector<PluginInfo> listPluginsByState(PluginState state) const;
    PluginState getPluginState(const std::string& pluginId) const;
    bool isPluginLoaded(const std::string& pluginId) const;
    int getPluginCount() const;

    // 获取插件的依赖树
    std::vector<std::string> getPluginDependencies(const std::string& pluginId) const;
    // 获取依赖此插件的插件列表
    std::vector<std::string> getPluginDependents(const std::string& pluginId) const;

    // ==================== 热更新 ====================
    bool hotReload(const std::string& pluginId);
    bool hasNewVersion(const std::string& pluginId) const;

    // ==================== 版本管理 ====================
    // 设置最低兼容 API 版本
    void setMinApiVersion(int version);
    int getMinApiVersion() const;

    // ==================== 配置持久化 ====================
    bool saveConfig(const std::string& filePath = "plugins_config.json");
    bool loadConfig(const std::string& filePath = "plugins_config.json");

    // ==================== 事件 ====================
    void setGlobalEventListener(PluginEventListener* listener);
    void onPluginEvent(PluginEvent event, const std::string& pluginId) override;

    // ==================== 错误恢复 ====================
    // 注册崩溃回调（当插件进入 ERROR 状态时触发）
    using CrashCallback = std::function<void(const std::string& pluginId, const std::string& error)>;
    void setCrashCallback(CrashCallback cb);
    // 尝试恢复一个处于 ERROR 状态的插件
    bool attemptRecovery(const std::string& pluginId);

private:
    // 内部句柄
    struct PluginHandle {
        void* sharedLib = nullptr;
        PluginInterface* instance = nullptr;
        PluginInfo info;
        PluginState state = PluginState::UNLOADED;
        CreatePluginFunc createFunc = nullptr;
        DestroyPluginFunc destroyFunc = nullptr;
        GetPluginInfoFunc infoFunc = nullptr;
        std::string filePath;
        std::filesystem::file_time_type lastModified;  // 用于热更新的 mtime
        std::map<std::string, std::any> savedConfig;   // 持久化配置

        // 引用计数 — 每次 getPlugin() 增加，releasePlugin() 减少
        // 达到 0 时 safeUnload 可真正销毁
        mutable std::atomic<int> refCount{0};
    };

    // 内部方法
    PluginHandle* findHandle(const std::string& pluginId);
    const PluginHandle* findHandle(const std::string& pluginId) const;

    // 依赖管理
    bool resolveDependencies(const std::string& pluginId,
                             std::vector<std::string>& order,
                             std::unordered_set<std::string>& visited,
                             std::unordered_set<std::string>& inStack);
    bool checkDependencies(const std::string& pluginId) const;
    bool loadDependencies(const std::string& pluginId);
    bool unloadDependents(const std::string& pluginId);

    // 版本兼容性
    bool checkVersionCompatibility(const PluginInfo& info) const;

    // 事件转发
    void fireEvent(PluginEvent event, const std::string& pluginId);

    // 安全卸载
    void safeUnload(PluginHandle* handle);

    // 成员
    std::vector<std::filesystem::path> pluginDirectories_;
    std::unordered_map<std::string, PluginHandle> plugins_;
    mutable std::mutex mutex_;
    PluginEventListener* globalListener_ = nullptr;
    int minApiVersion_ = 1;  // 最低兼容 API 版本
    CrashCallback crashCallback_;
};

} // namespace clma

#endif // CLM_PLUGIN_MANAGER_HPP
