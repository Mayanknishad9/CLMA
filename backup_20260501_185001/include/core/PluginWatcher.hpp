#ifndef CLM_PLUGIN_WATCHER_HPP
#define CLM_PLUGIN_WATCHER_HPP

#include <string>
#include <memory>
#include <atomic>
#include <thread>
#include <functional>
#include <vector>

namespace clma {

class PluginManager;

/**
 * PluginWatcher — 文件系统监控器
 *
 * 使用 Linux inotify 监控插件目录的文件变更，
 * 自动触发 PluginManager::hotReload()。
 *
 * 如果 inotify 不可用，自动降级到轮询模式。
 */
class PluginWatcher {
public:
    explicit PluginWatcher(PluginManager* manager);
    ~PluginWatcher();

    PluginWatcher(const PluginWatcher&) = delete;
    PluginWatcher& operator=(const PluginWatcher&) = delete;

    // 启动/停止监控
    bool start(int intervalMs = 2000);
    void stop();

    // 添加监控目录
    void watchDirectory(const std::string& path);

    // 状态
    bool isRunning() const { return running_.load(std::memory_order_relaxed); }

    // 设置回调 — 文件变更时的通知
    using ChangeCallback = std::function<void(const std::string& pluginId)>;
    void setChangeCallback(ChangeCallback cb);

private:
    // 后台线程主循环
    void watchLoop(int intervalMs);

    // 检查单个目录的变更
    void checkDirectory(const std::string& dir);

    // 成员
    PluginManager* manager_;
    std::atomic<bool> running_{false};
    std::thread watcherThread_;
    std::vector<std::string> watchDirs_;
    ChangeCallback changeCallback_;

    // 轮询模式的文件状态缓存: filePath -> lastModTime
    // （用于 inotify 不可用时的后备）
    struct FileState {
        std::string path;
        std::chrono::system_clock::time_point lastModTime;
    };
    std::vector<FileState> fileStates_;
};

} // namespace clma

#endif // CLM_PLUGIN_WATCHER_HPP
