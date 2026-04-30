#ifndef CLM_SANDBOX_HPP
#define CLM_SANDBOX_HPP

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <chrono>

namespace clma {

/**
 * Sandbox — 子进程沙箱隔离
 *
 * 使用 fork() + seccomp() 为插件执行提供隔离环境。
 * 支持：系统调用过滤（白名单）、超时限制、内存限制、文件路径白名单。
 *
 * 工作模式：
 * 1. fork() 子进程
 * 2. 子进程加载 seccomp BPF 过滤器
 * 3. 执行用户提供的函数
 * 4. 父进程监控：超时杀死 + 收集退出码
 * 5. 结果通过 pipe 传递回父进程
 */

// 沙箱配置
struct SandboxConfig {
    // 超时限制（毫秒）
    int timeoutMs = 5000;

    // 内存限制（MB，0=不限制）
    size_t maxMemoryMB = 256;

    // 文件系统白名单
    std::vector<std::string> allowedReadPaths;
    std::vector<std::string> allowedWritePaths;

    // 允许的网络操作
    bool allowNetwork = false;

    // 允许的进程创建
    bool allowProcesses = false;

    // 最大输出大小 (bytes)
    size_t maxOutputBytes = 1024 * 1024;  // 1MB

    // 默认配置 — 最严格
    static SandboxConfig strict() {
        SandboxConfig cfg;
        cfg.timeoutMs = 5000;
        cfg.maxMemoryMB = 64;
        cfg.allowNetwork = false;
        cfg.allowProcesses = false;
        return cfg;
    }

    // 宽松配置 — 允许网络和标准路径
    static SandboxConfig permissive() {
        SandboxConfig cfg;
        cfg.timeoutMs = 30000;
        cfg.maxMemoryMB = 512;
        cfg.allowedReadPaths = {"/tmp", "/usr/share", "/etc"};
        cfg.allowNetwork = true;
        cfg.allowProcesses = false;
        return cfg;
    }
};

// 沙箱执行结果
struct SandboxResult {
    bool success = false;
    int exitCode = -1;
    std::string stdout;
    std::string stderr;
    std::string errorMessage;      // 错误描述
    bool timedOut = false;
    int signalNumber = 0;           // 导致终止的信号
    double executionTimeMs = 0.0;
};

/**
 * Sandbox 沙箱执行器
 */
class Sandbox {
public:
    explicit Sandbox(const SandboxConfig& config = SandboxConfig::strict());
    ~Sandbox();

    Sandbox(const Sandbox&) = delete;
    Sandbox& operator=(const Sandbox&) = delete;

    // 设置配置
    void setConfig(const SandboxConfig& config);
    const SandboxConfig& getConfig() const { return config_; }

    // 添加白名单路径
    void addAllowedReadPath(const std::string& path);
    void addAllowedWritePath(const std::string& path);

    // 在沙箱中执行一个函数（子进程运行）
    // 函数返回 0 表示成功，非 0 表示失败
    using SandboxedFunc = std::function<int()>;
    SandboxResult execute(const SandboxedFunc& func);

    // 检查 seccomp 是否可用
    static bool isSeccompAvailable();

    // 检查当前运行环境
    static bool isSupported();

private:
    // 在子进程中设置沙箱限制
    void applySandboxLimits();

    // 设置 seccomp BPF 过滤器
    void applySeccompFilter();

    // 设置资源限制（RLIMIT）
    void applyResourceLimits();

    // 检查路径是否在白名单内
    bool isPathAllowed(const std::string& path, bool write) const;

    // 成员
    SandboxConfig config_;
};

} // namespace clma

#endif // CLM_SANDBOX_HPP
