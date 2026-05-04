#include "core/PluginWatcher.hpp"
#include "core/PluginManager.hpp"
#include <iostream>
#include <algorithm>
#include <sys/inotify.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <filesystem>

namespace clma {

PluginWatcher::PluginWatcher(PluginManager* manager)
    : manager_(manager)
{
}

PluginWatcher::~PluginWatcher() {
    stop();
}

void PluginWatcher::watchDirectory(const std::string& path) {
    if (std::find(watchDirs_.begin(), watchDirs_.end(), path) == watchDirs_.end()) {
        watchDirs_.push_back(path);
        std::cout << "[PluginWatcher] Watching directory: " << path << std::endl;
    }
}

void PluginWatcher::setChangeCallback(ChangeCallback cb) {
    changeCallback_ = std::move(cb);
}

bool PluginWatcher::start(int intervalMs) {
    if (running_.load(std::memory_order_relaxed)) {
        std::cerr << "[PluginWatcher] Already running" << std::endl;
        return false;
    }

    if (watchDirs_.empty()) {
        std::cerr << "[PluginWatcher] No directories to watch" << std::endl;
        return false;
    }

    // 收集初始文件状态
    fileStates_.clear();
    for (const auto& dir : watchDirs_) {
        if (!std::filesystem::exists(dir)) continue;
        for (const auto& entry : std::filesystem::directory_iterator(dir)) {
            if (!entry.is_regular_file()) continue;
            auto ext = entry.path().extension().string();
            if (ext != ".so" && ext != ".plugin" && ext != ".clma") continue;
            FileState fs;
            fs.path = entry.path().string();
            fs.lastModTime = std::chrono::system_clock::now();
            fileStates_.push_back(fs);
        }
    }

    running_.store(true, std::memory_order_release);

    // 先尝试 inotify
    int inotifyFd = inotify_init1(IN_NONBLOCK);
    if (inotifyFd >= 0) {
        bool watchOk = true;
        std::vector<int> watchDescriptors;
        for (const auto& dir : watchDirs_) {
            int wd = inotify_add_watch(inotifyFd, dir.c_str(),
                                        IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE | IN_DELETE);
            if (wd < 0) {
                std::cerr << "[PluginWatcher] Failed to watch directory via inotify: "
                          << dir << std::endl;
                watchOk = false;
                break;
            }
            watchDescriptors.push_back(wd);
        }

        if (watchOk) {
            std::cout << "[PluginWatcher] Using inotify (fd=" << inotifyFd << ")" << std::endl;
            watcherThread_ = std::thread([this, inotifyFd, watchDescriptors, intervalMs]() {
                // 稍后实现 inotify 事件循环
                // 当前使用简单的轮询 + inotify 检测
                // 让线程处理超时轮询和 inotify 事件
                char buffer[4096];
                struct pollfd pfd = {inotifyFd, POLLIN, 0};

                while (running_.load(std::memory_order_relaxed)) {
                    int ret = poll(&pfd, 1, intervalMs);
                    if (ret < 0) {
                        if (errno == EINTR) continue;
                        break;
                    }

                    if (ret > 0 && (pfd.revents & POLLIN)) {
                        // 读取事件并触发重扫+热重载
                        ssize_t len = read(inotifyFd, buffer, sizeof(buffer));
                        if (len > 0) {
                            std::cout << "[PluginWatcher] File change detected, rescanning..." << std::endl;
                            // 触发重新扫描
                            int found = manager_->scanPlugins();
                            std::cout << "[PluginWatcher] Rescan found " << found
                                      << " new/changed plugins" << std::endl;

                            // 对每个已知插件检查是否有新版本
                            auto allPlugins = manager_->listPlugins();
                            for (const auto& info : allPlugins) {
                                if (manager_->hasNewVersion(info.id)) {
                                    std::cout << "[PluginWatcher] Hot-reloading: "
                                              << info.id << std::endl;
                                    if (changeCallback_) {
                                        changeCallback_(info.id);
                                    }
                                    manager_->hotReload(info.id);
                                }
                            }
                        }
                    }
                }

                // 清理
                for (int wd : watchDescriptors) {
                    inotify_rm_watch(inotifyFd, wd);
                }
                close(inotifyFd);
            });
            return true;
        }
        close(inotifyFd);
    }

    // inotify 不可用 — 降级到轮询模式
    std::cout << "[PluginWatcher] inotify unavailable, falling back to polling mode"
              << std::endl;

    watcherThread_ = std::thread([this, intervalMs]() {
        while (running_.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(intervalMs));

            // 检查每个目录的变更
            for (const auto& dir : watchDirs_) {
                checkDirectory(dir);
            }
        }
    });

    return true;
}

void PluginWatcher::stop() {
    running_.store(false, std::memory_order_release);
    if (watcherThread_.joinable()) {
        watcherThread_.join();
    }
}

void PluginWatcher::checkDirectory(const std::string& dir) {
    if (!std::filesystem::exists(dir)) return;

    auto now = std::chrono::system_clock::now();
    bool changed = false;

    // 检查新文件和已修改的文件
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;
        auto ext = entry.path().extension().string();
        if (ext != ".so" && ext != ".plugin" && ext != ".clma") continue;

        auto path = entry.path().string();
        auto lastWrite = entry.last_write_time();

        // 查找缓存状态
        auto it = std::find_if(fileStates_.begin(), fileStates_.end(),
            [&](const FileState& fs) { return fs.path == path; });

        if (it == fileStates_.end()) {
            // 新文件
            FileState fs;
            fs.path = path;
            fs.lastModTime = now;
            fileStates_.push_back(fs);
            changed = true;
        } else {
            // 检查文件修改时间
            // 使用文件系统 clock
            auto wt = lastWrite;
            auto ct = std::filesystem::file_time_type::clock::now();
            // 简化：直接用 mtime 比较差值
            // 如果无法比较不同类型的 clock，用 chrono::duration
            auto diff = std::chrono::duration_cast<std::chrono::seconds>(
                ct - wt).count();
            // 如果文件 mtime 接近现在（最近60秒内修改过），认为是新版本
            auto cacheAge = std::chrono::duration_cast<std::chrono::seconds>(
                now - it->lastModTime).count();
            if (diff < 60 && cacheAge > 0) {
                it->lastModTime = now;
                changed = true;
            }
        }
    }

    // 检查已删除的文件
    fileStates_.erase(
        std::remove_if(fileStates_.begin(), fileStates_.end(),
            [&](const FileState& fs) {
                if (!std::filesystem::exists(fs.path)) {
                    // 文件被删除 — 检查是否在监控目录下
                    auto parent = std::filesystem::path(fs.path).parent_path().string();
                    return parent == dir;
                }
                return false;
            }),
        fileStates_.end());

    if (changed) {
        std::cout << "[PluginWatcher] Changes detected in: " << dir << std::endl;
        int found = manager_->scanPlugins();
        std::cout << "[PluginWatcher] Rescan found " << found
                  << " new/changed plugins" << std::endl;

        auto allPlugins = manager_->listPlugins();
        for (const auto& info : allPlugins) {
            if (manager_->hasNewVersion(info.id)) {
                std::cout << "[PluginWatcher] Hot-reloading: " << info.id << std::endl;
                if (changeCallback_) {
                    changeCallback_(info.id);
                }
                manager_->hotReload(info.id);
            }
        }
    }
}

} // namespace clma
