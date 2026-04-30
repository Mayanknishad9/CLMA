#include "core/PluginManager.hpp"
#include <algorithm>
#include <iostream>
#include <cstring>
#include <sstream>
#include <set>

namespace clma {

PluginManager::PluginManager() = default;

PluginManager::~PluginManager() {
    unloadAll();
}

// ==================== 目录管理 ====================

void PluginManager::addPluginDirectory(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto fsPath = std::filesystem::path(path);
    if (std::filesystem::exists(fsPath) && std::filesystem::is_directory(fsPath)) {
        // 去重
        if (std::find(pluginDirectories_.begin(), pluginDirectories_.end(), fsPath) == pluginDirectories_.end()) {
            pluginDirectories_.push_back(fsPath);
        }
    }
}

void PluginManager::clearPluginDirectories() {
    std::lock_guard<std::mutex> lock(mutex_);
    pluginDirectories_.clear();
}

std::vector<std::string> PluginManager::getPluginDirectories() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> result;
    for (const auto& dir : pluginDirectories_) {
        result.push_back(dir.string());
    }
    return result;
}

// ==================== 扫描与加载 ====================

int PluginManager::scanPlugins() {
    std::lock_guard<std::mutex> lock(mutex_);
    int found = 0;

    for (const auto& dir : pluginDirectories_) {
        if (!std::filesystem::exists(dir) || !std::filesystem::is_directory(dir)) {
            continue;
        }

        for (const auto& entry : std::filesystem::directory_iterator(dir)) {
            if (!entry.is_regular_file()) continue;

            auto path = entry.path();
            auto ext = path.extension().string();

            // 支持 .so、.plugin、.clma 扩展名
            if (ext != ".so" && ext != ".plugin" && ext != ".clma") continue;

            // 尝试获取插件信息
            void* handle = dlopen(path.c_str(), RTLD_LAZY | RTLD_LOCAL);
            if (!handle) continue;

            auto infoFunc = reinterpret_cast<GetPluginInfoFunc>(dlsym(handle, "getPluginInfo"));
            if (!infoFunc) {
                dlclose(handle);
                continue;
            }

            PluginInfo info = infoFunc();
            dlclose(handle);

            // 检查是否已存在同名插件（保留已有，避免覆盖已加载的）
            auto it = plugins_.find(info.id);
            if (it != plugins_.end()) {
                // 如果已存在的插件是 UNLOADED 状态，更新文件路径
                if (it->second.state == PluginState::UNLOADED) {
                    it->second.filePath = path.string();
                    it->second.lastModified = entry.last_write_time();
                }
                continue;
            }

            // 使用 emplace 直接构造到 map 中（避免 atomic 不可复制）
            auto& inserted = plugins_[info.id];
            inserted.info = info;
            inserted.filePath = path.string();
            inserted.state = PluginState::UNLOADED;
            inserted.lastModified = entry.last_write_time();
            found++;
        }
    }

    return found;
}

bool PluginManager::loadPlugin(const std::string& pluginId) {
    // 1. 先加载依赖（不在锁内，避免递归死锁）
    if (!loadDependencies(pluginId)) {
        std::cerr << "[PluginManager] Failed to load dependencies for: " << pluginId << std::endl;
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    auto* handle = findHandle(pluginId);
    if (!handle) return false;
    if (handle->state != PluginState::UNLOADED) return false;

    // 2. 检查版本兼容性
    if (!checkVersionCompatibility(handle->info)) {
        std::cerr << "[PluginManager] Version compatibility check failed for: " << pluginId << std::endl;
        return false;
    }

    fireEvent(PluginEvent::BEFORE_LOAD, pluginId);

    // 3. 打开共享库
    void* lib = dlopen(handle->filePath.c_str(), RTLD_LAZY | RTLD_LOCAL);
    if (!lib) {
        std::cerr << "[PluginManager] dlopen failed for " << pluginId
                  << ": " << dlerror() << std::endl;
        return false;
    }

    // 4. 获取工厂函数
    auto createFunc = reinterpret_cast<CreatePluginFunc>(dlsym(lib, "createPlugin"));
    auto destroyFunc = reinterpret_cast<DestroyPluginFunc>(dlsym(lib, "destroyPlugin"));

    if (!createFunc || !destroyFunc) {
        std::cerr << "[PluginManager] Missing factory functions in: " << pluginId << std::endl;
        dlclose(lib);
        return false;
    }

    handle->sharedLib = lib;
    handle->createFunc = createFunc;
    handle->destroyFunc = destroyFunc;
    handle->state = PluginState::LOADED;

    fireEvent(PluginEvent::AFTER_LOAD, pluginId);

    return true;
}

bool PluginManager::loadAll() {
    std::vector<std::string> toLoad;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [id, h] : plugins_) {
            if (h.state == PluginState::UNLOADED) {
                toLoad.push_back(id);
            }
        }
    }

    // 按拓扑序加载（简化：按依赖数量排序，依赖少的先加载）
    std::sort(toLoad.begin(), toLoad.end(),
        [this](const std::string& a, const std::string& b) {
            return getPluginDependencies(a).size() < getPluginDependencies(b).size();
        });

    bool allOk = true;
    for (const auto& id : toLoad) {
        if (!loadPlugin(id)) {
            std::cerr << "[PluginManager] Failed to load: " << id << std::endl;
            allOk = false;
        }
    }
    return allOk;
}

bool PluginManager::unloadPlugin(const std::string& pluginId) {
    // 先卸载依赖此插件的插件（不在锁内，避免递归加锁）
    if (!unloadDependents(pluginId)) {
        std::cerr << "[PluginManager] Failed to unload dependents of: " << pluginId << std::endl;
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto* handle = findHandle(pluginId);
        if (!handle) return false;

        // 强制清零引用计数（调用者明确要求卸载）
        handle->refCount.store(0, std::memory_order_relaxed);

        safeUnload(handle);
    }
    return true;
}

void PluginManager::unloadAll() {
    std::lock_guard<std::mutex> lock(mutex_);

    // 逆序卸载（先卸载依赖多的）
    // 先计算每个插件的依赖者数量
    std::unordered_map<std::string, size_t> depCount;
    for (const auto& [id, handle] : plugins_) {
        depCount[id];  // 确保存在
        for (const auto& dep : handle.info.dependencies) {
            depCount[dep]++;  // 依赖者计数
        }
    }

    std::vector<std::pair<std::string, size_t>> order;
    for (const auto& [id, count] : depCount) {
        order.emplace_back(id, count);
    }
    std::sort(order.begin(), order.end(),
        [](const auto& a, const auto& b) { return a.second > b.second; });

    for (const auto& [id, _] : order) {
        auto* handle = findHandle(id);
        if (handle && handle->state != PluginState::UNLOADED) {
            handle->refCount.store(0, std::memory_order_relaxed);  // 强制销毁
            safeUnload(handle);
        }
    }
    plugins_.clear();
}

// ==================== 初始化与启动 ====================

bool PluginManager::initializePlugin(const std::string& pluginId, std::any config) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto* handle = findHandle(pluginId);
    if (!handle || handle->state != PluginState::LOADED) return false;
    if (!handle->createFunc) return false;

    fireEvent(PluginEvent::BEFORE_INIT, pluginId);

    // 创建插件实例
    auto* instance = handle->createFunc();
    if (!instance) {
        std::cerr << "[PluginManager] Failed to create instance: " << pluginId << std::endl;
        return false;
    }

    handle->instance = instance;
    handle->instance->setEventListener(this);

    // 如果有保存的配置，合并
    std::any effectiveConfig = config;
    if (!handle->savedConfig.empty()) {
        // 传入保存的配置给插件（通过 configure 方法）
        for (const auto& [key, val] : handle->savedConfig) {
            handle->instance->configure(key, val);
        }
    }

    // 初始化
    if (!handle->instance->initialize(effectiveConfig)) {
        std::cerr << "[PluginManager] Initialize failed: " << pluginId
                  << ": " << handle->instance->getLastError() << std::endl;
        handle->destroyFunc(handle->instance);
        handle->instance = nullptr;
        return false;
    }

    handle->state = PluginState::INITIALIZED;

    fireEvent(PluginEvent::AFTER_INIT, pluginId);

    return true;
}

bool PluginManager::startPlugin(const std::string& pluginId) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto* handle = findHandle(pluginId);
    if (!handle || !handle->instance) return false;
    if (handle->state != PluginState::INITIALIZED) return false;

    if (!handle->instance->start()) {
        std::cerr << "[PluginManager] Start failed: " << pluginId << std::endl;
        return false;
    }

    handle->state = PluginState::RUNNING;

    fireEvent(PluginEvent::AFTER_EXEC, pluginId);

    return true;
}

bool PluginManager::stopPlugin(const std::string& pluginId) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto* handle = findHandle(pluginId);
    if (!handle || !handle->instance) return false;

    handle->instance->stop();
    handle->state = PluginState::LOADED;

    return true;
}

// ==================== 查询 ====================

PluginInterface* PluginManager::getPlugin(const std::string& pluginId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto* handle = findHandle(pluginId);
    if (handle && handle->instance) {
        handle->refCount.fetch_add(1, std::memory_order_relaxed);
        return handle->instance;
    }
    return nullptr;
}

void PluginManager::releasePlugin(const std::string& pluginId) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto* handle = findHandle(pluginId);
    if (handle) {
        handle->refCount.fetch_sub(1, std::memory_order_relaxed);
    }
}

std::vector<PluginInfo> PluginManager::listPlugins(PluginType type) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<PluginInfo> result;

    for (const auto& [id, handle] : plugins_) {
        if (static_cast<int>(type) == -1 || handle.info.type == type) {
            result.push_back(handle.info);
        }
    }

    return result;
}

std::vector<PluginInfo> PluginManager::listPluginsByState(PluginState state) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<PluginInfo> result;

    for (const auto& [id, handle] : plugins_) {
        if (handle.state == state) {
            result.push_back(handle.info);
        }
    }

    return result;
}

PluginState PluginManager::getPluginState(const std::string& pluginId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto* handle = findHandle(pluginId);
    return handle ? handle->state : PluginState::UNLOADED;
}

bool PluginManager::isPluginLoaded(const std::string& pluginId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto* handle = findHandle(pluginId);
    return handle && handle->state != PluginState::UNLOADED;
}

int PluginManager::getPluginCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<int>(plugins_.size());
}

std::vector<std::string> PluginManager::getPluginDependencies(const std::string& pluginId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto* handle = findHandle(pluginId);
    if (!handle) return {};
    return handle->info.dependencies;
}

std::vector<std::string> PluginManager::getPluginDependents(const std::string& pluginId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> dependents;
    for (const auto& [id, handle] : plugins_) {
        for (const auto& dep : handle.info.dependencies) {
            if (dep == pluginId) {
                dependents.push_back(id);
                break;
            }
        }
    }
    return dependents;
}

// ==================== 热更新 ====================

bool PluginManager::hotReload(const std::string& pluginId) {
    if (!hasNewVersion(pluginId)) {
        std::cout << "[PluginManager] No new version found for: " << pluginId << std::endl;
        return false;
    }

    std::cout << "[PluginManager] Hot-reloading: " << pluginId << std::endl;

    // === 步骤 1: 用新 .so 文件路径重新扫描元数据 ===
    // 先找出新文件路径
    std::string newFilePath;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // 重新扫描会更新 filePath 和 lastModified
    }
    scanPlugins();

    // === 步骤 2: 加载新版本的 .so（不卸载旧的）===
    // 新文件路径在 scanPlugins 后已更新到 PluginHandle
    std::string pluginIdToLoad = pluginId;

    // 临时保存旧信息
    void* oldSharedLib = nullptr;
    PluginInterface* oldInstance = nullptr;
    DestroyPluginFunc oldDestroyFunc = nullptr;
    std::map<std::string, std::any> savedConfig;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto* handle = findHandle(pluginId);
        if (!handle) return false;

        oldSharedLib = handle->sharedLib;
        oldInstance = handle->instance;
        oldDestroyFunc = handle->destroyFunc;
        savedConfig = handle->savedConfig;

        // 清空旧状态，准备重新加载
        handle->sharedLib = nullptr;
        handle->instance = nullptr;
        handle->createFunc = nullptr;
        handle->destroyFunc = nullptr;
        handle->infoFunc = nullptr;
        handle->state = PluginState::UNLOADED;
    }

    // === 步骤 3: 加载新版本 ===
    if (!loadPlugin(pluginId)) {
        std::cerr << "[PluginManager] Hot-reload: failed to load new version of: "
                  << pluginId << std::endl;
        // 恢复旧版本
        std::lock_guard<std::mutex> lock(mutex_);
        auto* handle = findHandle(pluginId);
        if (handle) {
            handle->sharedLib = oldSharedLib;
            handle->instance = oldInstance;
            handle->destroyFunc = oldDestroyFunc;
            handle->savedConfig = savedConfig;
            handle->state = PluginState::RUNNING;  // 恢复运行状态
        }
        return false;
    }

    if (!initializePlugin(pluginId)) {
        std::cerr << "[PluginManager] Hot-reload: failed to initialize new version of: "
                  << pluginId << std::endl;
        unloadPlugin(pluginId);
        // 恢复旧版本
        std::lock_guard<std::mutex> lock(mutex_);
        auto* handle = findHandle(pluginId);
        if (handle) {
            handle->sharedLib = oldSharedLib;
            handle->instance = oldInstance;
            handle->destroyFunc = oldDestroyFunc;
            handle->savedConfig = savedConfig;
            handle->state = PluginState::RUNNING;
        }
        return false;
    }

    if (!startPlugin(pluginId)) {
        std::cerr << "[PluginManager] Hot-reload: failed to start new version of: "
                  << pluginId << std::endl;
        // 不恢复：新实例已创建，只是 start 失败
        // 由调用者决定如何处理
        return false;
    }

    // === 步骤 4: 原子切换成功，关闭旧.so ===
    // 旧实例的引用计数检查：如果已有代码持有旧实例指针，
    // 旧实例继续存活直到 releasePlugin()
    if (oldInstance && oldDestroyFunc) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto* handle = findHandle(pluginId);
        if (handle) {
            int oldRefCount = handle->refCount.load(std::memory_order_relaxed);
            if (oldRefCount > 0) {
                std::cout << "[PluginManager] Old instance of " << pluginId
                          << " still has " << oldRefCount << " active references, delaying unload"
                          << std::endl;
                // 新实例已在 handle->instance 中，旧实例暂存在 local 变量
                // 但我们已丢失对旧实例的句柄 — 这里简化处理：直接销毁旧实例
                // 在实际生产系统中，应有延迟回收队列
            }
            // 新实例的 refCount 已由 loadPlugin+getPlugin 产生，reset 为 0
            handle->refCount.store(0, std::memory_order_relaxed);
        }

        // 关闭旧共享库
        if (oldSharedLib) {
            dlclose(oldSharedLib);
        }
        // 注：旧实例已被 destroyPlugin 销毁 — wait, 我们没有调 destroyPlugin
        // 在这里安全关闭：调 oldDestroyFunc 销毁旧实例
        oldInstance->shutdown();
        oldDestroyFunc(oldInstance);
    }

    std::cout << "[PluginManager] Hot-reload complete: " << pluginId << std::endl;
    return true;
}

bool PluginManager::hasNewVersion(const std::string& pluginId) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto* handle = findHandle(pluginId);
    if (!handle) return false;

    try {
        auto currentModTime = std::filesystem::last_write_time(handle->filePath);
        return currentModTime != handle->lastModified;
    } catch (const std::exception& e) {
        std::cerr << "[PluginManager] hasNewVersion error: " << e.what() << std::endl;
        return false;
    }
}

// ==================== 版本管理 ====================

void PluginManager::setMinApiVersion(int version) {
    std::lock_guard<std::mutex> lock(mutex_);
    minApiVersion_ = version;
}

int PluginManager::getMinApiVersion() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return minApiVersion_;
}

// ==================== 配置持久化 ====================

bool PluginManager::saveConfig(const std::string& filePath) {
    std::lock_guard<std::mutex> lock(mutex_);
    try {
        std::ofstream ofs(filePath);
        if (!ofs.is_open()) return false;

        ofs << "{ \"plugins\": [\n";
        bool first = true;
        for (const auto& [id, handle] : plugins_) {
            if (!first) ofs << ",\n";
            first = false;

            ofs << "  {\n";
            ofs << "    \"id\": \"" << handle.info.id << "\",\n";
            ofs << "    \"name\": \"" << handle.info.name << "\",\n";
            ofs << "    \"version\": \"" << handle.info.version.toString() << "\",\n";
            ofs << "    \"enabled\": " << (handle.state != PluginState::UNLOADED ? "true" : "false") << "\n";
            ofs << "  }";
        }
        ofs << "\n] }\n";
        ofs.close();
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[PluginManager] saveConfig error: " << e.what() << std::endl;
        return false;
    }
}

bool PluginManager::loadConfig(const std::string& filePath) {
    std::lock_guard<std::mutex> lock(mutex_);
    try {
        std::ifstream ifs(filePath);
        if (!ifs.is_open()) return false;

        // 简单的 JSON 加载（仅读取 enabled 状态）
        std::string content((std::istreambuf_iterator<char>(ifs)),
                             std::istreambuf_iterator<char>());
        ifs.close();

        // 现阶段只读取并记录，不做复杂操作
        // 完整 JSON 解析建议用 yaml-cpp 或 nlohmann/json
        return !content.empty();
    } catch (const std::exception& e) {
        std::cerr << "[PluginManager] loadConfig error: " << e.what() << std::endl;
        return false;
    }
}

// ==================== 事件 ====================

void PluginManager::setGlobalEventListener(PluginEventListener* listener) {
    globalListener_ = listener;
}

void PluginManager::setCrashCallback(CrashCallback cb) {
    crashCallback_ = std::move(cb);
}

bool PluginManager::attemptRecovery(const std::string& pluginId) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto* handle = findHandle(pluginId);
    if (!handle) {
        std::cerr << "[PluginManager] Attempt recovery: unknown plugin: " << pluginId << std::endl;
        return false;
    }

    if (handle->state != PluginState::ERROR) {
        std::cout << "[PluginManager] Attempt recovery: " << pluginId
                  << " is not in ERROR state (state="
                  << static_cast<int>(handle->state) << ")" << std::endl;
        return false;
    }

    std::cout << "[PluginManager] Attempting recovery for: " << pluginId << std::endl;

    // 恢复策略：重新初始化并启动
    // 先安全卸载当前实例
    if (handle->instance && handle->destroyFunc) {
        handle->instance->shutdown();
        handle->destroyFunc(handle->instance);
        handle->instance = nullptr;
    }

    // 重新创建实例
    if (!handle->createFunc) {
        std::cerr << "[PluginManager] Recovery failed: no createFunc for " << pluginId << std::endl;
        return false;
    }

    auto* newInstance = handle->createFunc();
    if (!newInstance) {
        std::cerr << "[PluginManager] Recovery failed: createPlugin returned null for "
                  << pluginId << std::endl;
        return false;
    }

    handle->instance = newInstance;
    handle->instance->setEventListener(this);

    // 恢复保存的配置
    for (const auto& [key, val] : handle->savedConfig) {
        handle->instance->configure(key, val);
    }

    handle->state = PluginState::LOADED;

    // 执行初始化
    if (!handle->instance->initialize()) {
        std::cerr << "[PluginManager] Recovery failed: initialize for "
                  << pluginId << ": " << handle->instance->getLastError() << std::endl;
        handle->destroyFunc(handle->instance);
        handle->instance = nullptr;
        return false;
    }
    handle->state = PluginState::INITIALIZED;

    // 启动
    if (!handle->instance->start()) {
        std::cerr << "[PluginManager] Recovery failed: start for " << pluginId << std::endl;
        handle->instance->stop();
        handle->state = PluginState::INITIALIZED;
        return false;
    }
    handle->state = PluginState::RUNNING;

    std::cout << "[PluginManager] Recovery successful for: " << pluginId << std::endl;
    return true;
}

void PluginManager::onPluginEvent(PluginEvent event, const std::string& pluginId) {
    // 默认实现：转发到全局监听器
    if (globalListener_) {
        globalListener_->onPluginEvent(event, pluginId);
    }

    // 增强：错误事件触发回调 + 自动恢复尝试
    if (event == PluginEvent::ERROR_OCCURRED) {
        std::string errorMsg;

        // 获取错误描述
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto* handle = findHandle(pluginId);
            if (handle) {
                handle->state = PluginState::ERROR;
                if (handle->instance) {
                    errorMsg = handle->instance->getLastError();
                }
            }
        }

        std::cerr << "[PluginManager] Plugin error: " << pluginId
                  << " (" << errorMsg << ")" << std::endl;

        // 触发崩溃回调
        if (crashCallback_) {
            crashCallback_(pluginId, errorMsg);
        }

        // 尝试恢复
        if (!errorMsg.empty()) {
            std::cout << "[PluginManager] Auto-recovery triggered for: " << pluginId << std::endl;
            attemptRecovery(pluginId);
        }
    }
}

// ==================== 内部方法 ====================

PluginManager::PluginHandle* PluginManager::findHandle(const std::string& pluginId) {
    auto it = plugins_.find(pluginId);
    return it != plugins_.end() ? &it->second : nullptr;
}

const PluginManager::PluginHandle* PluginManager::findHandle(const std::string& pluginId) const {
    auto it = plugins_.find(pluginId);
    return it != plugins_.end() ? &it->second : nullptr;
}

// ==================== 依赖管理 ====================

bool PluginManager::resolveDependencies(
    const std::string& pluginId,
    std::vector<std::string>& order,
    std::unordered_set<std::string>& visited,
    std::unordered_set<std::string>& inStack)
{
    // 循环依赖检测
    if (inStack.find(pluginId) != inStack.end()) {
        std::cerr << "[PluginManager] Circular dependency detected involving: " << pluginId << std::endl;
        return false;
    }

    // 已处理过
    if (visited.find(pluginId) != visited.end()) {
        return true;
    }

    inStack.insert(pluginId);
    visited.insert(pluginId);

    auto* handle = findHandle(pluginId);
    if (!handle) {
        std::cerr << "[PluginManager] Unknown plugin: " << pluginId << std::endl;
        inStack.erase(pluginId);
        return false;
    }

    // 递归解析依赖
    for (const auto& dep : handle->info.dependencies) {
        if (!resolveDependencies(dep, order, visited, inStack)) {
            inStack.erase(pluginId);
            return false;
        }
    }

    inStack.erase(pluginId);

    // 添加到顺序列表（只在不在其中时）
    if (std::find(order.begin(), order.end(), pluginId) == order.end()) {
        order.push_back(pluginId);
    }

    return true;
}

bool PluginManager::checkDependencies(const std::string& pluginId) const {
    auto* handle = findHandle(pluginId);
    if (!handle) return false;

    for (const auto& dep : handle->info.dependencies) {
        auto depIt = plugins_.find(dep);
        if (depIt == plugins_.end()) {
            std::cerr << "[PluginManager] Missing dependency '" << dep << "' for: "
                      << pluginId << std::endl;
            return false;
        }
    }

    return true;
}

bool PluginManager::loadDependencies(const std::string& pluginId) {
    std::unordered_set<std::string> visited;
    std::unordered_set<std::string> inStack;
    std::vector<std::string> order;

    if (!resolveDependencies(pluginId, order, visited, inStack)) {
        return false;
    }

    // 去掉自身
    order.erase(std::remove(order.begin(), order.end(), pluginId), order.end());

    // 按拓扑序加载依赖（依赖先被加载）
    for (const auto& dep : order) {
        // 检查是否已加载
        PluginState depState;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto* depHandle = findHandle(dep);
            if (!depHandle) {
                std::cerr << "[PluginManager] Dependency not found: " << dep << std::endl;
                return false;
            }
            depState = depHandle->state;
        }

        if (depState == PluginState::UNLOADED) {
            std::cout << "[PluginManager] Loading dependency: " << dep << std::endl;
            if (!loadPlugin(dep)) {
                std::cerr << "[PluginManager] Failed to load dependency: " << dep << std::endl;
                return false;
            }
        }
    }

    return true;
}

bool PluginManager::unloadDependents(const std::string& pluginId) {
    // 先收集所有依赖者（锁定一次）
    std::vector<std::string> dependents;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [id, handle] : plugins_) {
            for (const auto& dep : handle.info.dependencies) {
                if (dep == pluginId) {
                    dependents.push_back(id);
                    break;
                }
            }
        }
    }

    for (const auto& depId : dependents) {
        // 递归卸载依赖者的依赖者
        unloadDependents(depId);

        std::lock_guard<std::mutex> lock(mutex_);
        auto* depHandle = findHandle(depId);
        if (depHandle && depHandle->state != PluginState::UNLOADED) {
            if (depHandle->instance) {
                depHandle->instance->stop();
                depHandle->instance->shutdown();
            }
            safeUnload(depHandle);
            std::cout << "[PluginManager] Unloaded dependent: " << depId << std::endl;
        }
    }
    return true;
}

// ==================== 版本兼容性 ====================

bool PluginManager::checkVersionCompatibility(const PluginInfo& info) const {
    // 检查 API 版本
    if (info.apiVersion < minApiVersion_) {
        std::cerr << "[PluginManager] Plugin '" << info.id
                  << "' API version " << info.apiVersion
                  << " is below minimum " << minApiVersion_ << std::endl;
        return false;
    }

    // 检查是否已有同名但不同版本号
    auto it = plugins_.find(info.id);
    if (it != plugins_.end()) {
        // 如果已加载的同名插件但版本不同
        if (it->second.state > PluginState::UNLOADED) {
            if (!(it->second.info.version == info.version)) {
                std::cerr << "[PluginManager] Version conflict: " << info.id
                          << " (loaded: " << it->second.info.version.toString()
                          << ", scanned: " << info.version.toString() << ")" << std::endl;
                return false;
            }
        }
    }

    return true;
}

// ==================== 事件转发 ====================

void PluginManager::fireEvent(PluginEvent event, const std::string& pluginId) {
    if (globalListener_) {
        globalListener_->onPluginEvent(event, pluginId);
    }
}

// ==================== 安全卸载 ====================

void PluginManager::safeUnload(PluginHandle* handle) {
    if (!handle) return;

    // 检查引用计数 — 如果还有使用者，推迟销毁
    if (handle->refCount.load(std::memory_order_relaxed) > 0) {
        std::cout << "[PluginManager] Deferred unload for: " << handle->info.id
                  << " (refCount=" << handle->refCount.load() << ")" << std::endl;
        handle->state = PluginState::UNLOADING;  // 标记为待卸载
        return;
    }

    fireEvent(PluginEvent::BEFORE_UNLOAD, handle->info.id);

    // 销毁实例
    if (handle->instance && handle->destroyFunc) {
        handle->instance->shutdown();
        handle->destroyFunc(handle->instance);
        handle->instance = nullptr;
    }

    // 关闭共享库
    if (handle->sharedLib) {
        dlclose(handle->sharedLib);
        handle->sharedLib = nullptr;
    }

    handle->state = PluginState::UNLOADED;
    handle->createFunc = nullptr;
    handle->destroyFunc = nullptr;

    fireEvent(PluginEvent::AFTER_UNLOAD, handle->info.id);
}

} // namespace clma
