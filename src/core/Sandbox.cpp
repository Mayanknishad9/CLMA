#include "core/Sandbox.hpp"
#include <iostream>
#include <cstring>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <sys/prctl.h>
#include <sys/signal.h>
#include <seccomp.h>
#include <fcntl.h>
#include <chrono>

namespace clma {

Sandbox::Sandbox(const SandboxConfig& config)
    : config_(config)
{
}

Sandbox::~Sandbox() = default;

void Sandbox::setConfig(const SandboxConfig& config) {
    config_ = config;
}

void Sandbox::addAllowedReadPath(const std::string& path) {
    config_.allowedReadPaths.push_back(path);
}

void Sandbox::addAllowedWritePath(const std::string& path) {
    config_.allowedWritePaths.push_back(path);
}

bool Sandbox::isSeccompAvailable() {
    // 尝试通过 prctl 检测 seccomp
    // 如果内核不支持，prctl(PR_GET_SECCOMP) 返回 -1
    // 这里简单检查是否能在子进程中使用
    // 实际更好的方法：尝试编译加载 seccomp 过滤器
    // 但编译时已经链接 libseccomp，运行时看是否能正常执行

    // 方法：打开 /proc/sys/kernel/seccomp 检查
    // 或者直接尝试 prctl
    if (prctl(PR_GET_SECCOMP, 0, 0, 0, 0) < 0) {
        if (errno == EINVAL) {
            // 内核不支持 seccomp
            return false;
        }
        // PR_GET_SECCOMP 在没有过滤器时返回 -1/ENOSYS 或 -1/EACCES
        // 但能在调用前设置说明功能存在
        if (errno == ENOSYS) {
            return false;
        }
        // EACCES 或其他错误 — seccomp 可能存在但当前无法查询
        return true;
    }
    return true;
}

bool Sandbox::isSupported() {
    // 检查 fork 可用
    pid_t pid = fork();
    if (pid < 0) {
        return false;
    }
    if (pid == 0) {
        _exit(0);
    }
    int status;
    waitpid(pid, &status, 0);

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return false;
    }

    // 检查 seccomp 是否可能可用
    // 不强制要求 seccomp — 没有 seccomp 时降级为纯资源限制沙箱
    return true;
}

void Sandbox::applyResourceLimits() {
    // CPU 时间限制（超时保障）
    if (config_.timeoutMs > 0) {
        struct rlimit cpuLimit;
        cpuLimit.rlim_cur = (config_.timeoutMs / 1000) + 1;  // 秒级，多给1秒余量
        cpuLimit.rlim_max = cpuLimit.rlim_cur + 1;
        setrlimit(RLIMIT_CPU, &cpuLimit);
    }

    // 内存限制
    if (config_.maxMemoryMB > 0) {
        struct rlimit memLimit;
        memLimit.rlim_cur = config_.maxMemoryMB * 1024 * 1024;
        memLimit.rlim_max = memLimit.rlim_cur;
        setrlimit(RLIMIT_AS, &memLimit);
        setrlimit(RLIMIT_DATA, &memLimit);
    }

    // 输出大小限制
    if (config_.maxOutputBytes > 0) {
        struct rlimit fileLimit;
        fileLimit.rlim_cur = config_.maxOutputBytes;
        fileLimit.rlim_max = config_.maxOutputBytes;
        setrlimit(RLIMIT_FSIZE, &fileLimit);
    }

    // 禁止核心转储
    struct rlimit coreLimit;
    coreLimit.rlim_cur = 0;
    coreLimit.rlim_max = 0;
    setrlimit(RLIMIT_CORE, &coreLimit);

    // 禁止创建新进程（如果配置不允许）
    if (!config_.allowProcesses) {
        // RLIMIT_NPROC = 0 会阻止 fork
        // 但我们的沙箱本身就是 fork 出来的子进程
        // 设置子进程的子进程无法创建
        struct rlimit nprocLimit;
        nprocLimit.rlim_cur = 0;
        nprocLimit.rlim_max = 0;
        setrlimit(RLIMIT_NPROC, &nprocLimit);
    }

    // 打开文件数量限制
    struct rlimit nofileLimit;
    nofileLimit.rlim_cur = 64;   // 最多64个文件描述符
    nofileLimit.rlim_max = 128;
    setrlimit(RLIMIT_NOFILE, &nofileLimit);
}

void Sandbox::applySeccompFilter() {
    // 初始化 seccomp 过滤器
    scmp_filter_ctx ctx = seccomp_init(SCMP_ACT_KILL);
    if (!ctx) {
        std::cerr << "[Sandbox] seccomp_init failed" << std::endl;
        return;
    }

    // 允许的基本系统调用 — 所有程序都需要
    std::vector<int> allowedSyscalls = {
        SCMP_SYS(read),
        SCMP_SYS(write),
        SCMP_SYS(close),
        SCMP_SYS(fstat),
        SCMP_SYS(lseek),
        SCMP_SYS(mmap),
        SCMP_SYS(munmap),
        SCMP_SYS(mprotect),
        SCMP_SYS(brk),
        SCMP_SYS(exit_group),
        SCMP_SYS(exit),
        SCMP_SYS(gettid),
        SCMP_SYS(tgkill),
        SCMP_SYS(getpid),
        SCMP_SYS(getppid),
        SCMP_SYS(getuid),
        SCMP_SYS(geteuid),
        SCMP_SYS(getgid),
        SCMP_SYS(getegid),
        SCMP_SYS(clock_gettime),
        SCMP_SYS(nanosleep),
        SCMP_SYS(sched_yield),
        SCMP_SYS(futex),
        SCMP_SYS(rt_sigaction),
        SCMP_SYS(rt_sigprocmask),
        SCMP_SYS(sigaltstack),
        SCMP_SYS(set_robust_list),
        SCMP_SYS(getrandom),
        SCMP_SYS(getcwd),
        SCMP_SYS(arch_prctl),    // x86_64 线程相关
        SCMP_SYS(set_tid_address),
        SCMP_SYS(uname),
        SCMP_SYS(readlink),
        SCMP_SYS(access),
        SCMP_SYS(stat),
        SCMP_SYS(lstat),
        SCMP_SYS(newfstatat),
        SCMP_SYS(openat),
        SCMP_SYS(open),
        SCMP_SYS(dup),
        SCMP_SYS(dup2),
        SCMP_SYS(dup3),
        SCMP_SYS(pipe),
        SCMP_SYS(pipe2),
        SCMP_SYS(writev),
        SCMP_SYS(readv),
        SCMP_SYS(pread64),
        SCMP_SYS(pwrite64),
        SCMP_SYS(ftruncate),
        SCMP_SYS(truncate),
        SCMP_SYS(fsync),
        SCMP_SYS(fdatasync),
        SCMP_SYS(pselect6),
        SCMP_SYS(select),
        SCMP_SYS(madvise),
        SCMP_SYS(mlock),
        SCMP_SYS(munlock),
        SCMP_SYS(prctl),
        SCMP_SYS(getrlimit),
        SCMP_SYS(setrlimit),
        SCMP_SYS(getrusage),
        SCMP_SYS(times),
        SCMP_SYS(sysinfo),
        SCMP_SYS(time),
        SCMP_SYS(gettimeofday),
        SCMP_SYS(sched_getparam),
        SCMP_SYS(sched_getscheduler),
        SCMP_SYS(sched_setparam),
        SCMP_SYS(sched_setscheduler),
    };

    // 如果允许网络，添加网络系统调用
    if (config_.allowNetwork) {
        allowedSyscalls.insert(allowedSyscalls.end(), {
            SCMP_SYS(socket),
            SCMP_SYS(connect),
            SCMP_SYS(sendto),
            SCMP_SYS(recvfrom),
            SCMP_SYS(sendmsg),
            SCMP_SYS(recvmsg),
            SCMP_SYS(bind),
            SCMP_SYS(listen),
            SCMP_SYS(accept),
            SCMP_SYS(getsockname),
            SCMP_SYS(getpeername),
            SCMP_SYS(setsockopt),
            SCMP_SYS(getsockopt),
            SCMP_SYS(shutdown),
            SCMP_SYS(poll),
            SCMP_SYS(epoll_create),
            SCMP_SYS(epoll_ctl),
            SCMP_SYS(epoll_wait),
            SCMP_SYS(eventfd),
            SCMP_SYS(ioctl),
            SCMP_SYS(fcntl),
        });
    }

    // 如果允许创建进程，添加 fork/clone 相关调用
    if (config_.allowProcesses) {
        allowedSyscalls.insert(allowedSyscalls.end(), {
            SCMP_SYS(clone),
            SCMP_SYS(fork),
            SCMP_SYS(vfork),
            SCMP_SYS(execve),
            SCMP_SYS(execveat),
            SCMP_SYS(wait4),
            SCMP_SYS(waitid),
        });
    }

    // 批量添加允许的系统调用
    for (int syscallNum : allowedSyscalls) {
        int rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, syscallNum, 0);
        if (rc < 0) {
            // 忽略不存在的系统调用（不同架构差异）
            if (rc != -2) {  // -2 = ENOENT（系统调用不存在于该架构）
                std::cerr << "[Sandbox] seccomp_rule_add warning for syscall "
                          << syscallNum << ": " << strerror(-rc) << std::endl;
            }
        }
    }

    // 应用过滤器
    int rc = seccomp_load(ctx);
    if (rc < 0) {
        std::cerr << "[Sandbox] seccomp_load failed: " << strerror(-rc) << std::endl;
    }

    seccomp_release(ctx);
}

void Sandbox::applySandboxLimits() {
    // 1. 设置资源限制
    applyResourceLimits();

    // 2. 设置 seccomp 过滤器（如果可用）
    if (isSeccompAvailable()) {
        applySeccompFilter();
    } else {
        std::cout << "[Sandbox] seccomp not available, using rlimit only" << std::endl;
    }

    // 3. 设置进程标题（调试用）
    prctl(PR_SET_NAME, "sandboxed", 0, 0, 0);

    // 4. 设置 PF 死亡信号 — 如果父进程挂了，子进程自动退出
    prctl(PR_SET_PDEATHSIG, SIGKILL, 0, 0, 0);
}

bool Sandbox::isPathAllowed(const std::string& path, bool write) const {
    if (!write) {
        // 读路径检查
        for (const auto& allowed : config_.allowedReadPaths) {
            if (path.compare(0, allowed.length(), allowed) == 0) {
                return true;
            }
        }
    } else {
        // 写路径检查
        for (const auto& allowed : config_.allowedWritePaths) {
            if (path.compare(0, allowed.length(), allowed) == 0) {
                return true;
            }
        }
    }
    return false;
}

SandboxResult Sandbox::execute(const SandboxedFunc& func) {
    SandboxResult result;

    auto startTime = std::chrono::high_resolution_clock::now();

    // 创建 pipe 用于从子进程获取 stdout
    int stdoutPipe[2];
    int stderrPipe[2];

    if (pipe(stdoutPipe) < 0 || pipe(stderrPipe) < 0) {
        result.errorMessage = "Failed to create pipes";
        return result;
    }

    // fork
    pid_t pid = fork();

    if (pid < 0) {
        // fork 失败
        close(stdoutPipe[0]);
        close(stdoutPipe[1]);
        close(stderrPipe[0]);
        close(stderrPipe[1]);
        result.errorMessage = "fork() failed: " + std::string(strerror(errno));
        return result;
    }

    if (pid == 0) {
        // === 子进程 ===
        // 重定向 stdout/stderr 到 pipe
        close(stdoutPipe[0]);  // 关闭读端
        close(stderrPipe[0]);
        dup2(stdoutPipe[1], STDOUT_FILENO);
        dup2(stderrPipe[1], STDERR_FILENO);
        close(stdoutPipe[1]);
        close(stderrPipe[1]);

        // 应用沙箱限制
        applySandboxLimits();

        // 执行用户函数
        int exitCode = func();
        _exit(exitCode);
    }

    // === 父进程 ===
    close(stdoutPipe[1]);  // 关闭写端
    close(stderrPipe[1]);

    // 读取子进程输出（非阻塞）
    char buffer[4096];
    std::string stdoutBuf, stderrBuf;

    // 设置非阻塞
    int flags = fcntl(stdoutPipe[0], F_GETFL, 0);
    fcntl(stdoutPipe[0], F_SETFL, flags | O_NONBLOCK);
    flags = fcntl(stderrPipe[0], F_GETFL, 0);
    fcntl(stderrPipe[0], F_SETFL, flags | O_NONBLOCK);

    // 等待子进程完成或超时
    auto deadline = std::chrono::high_resolution_clock::now()
                    + std::chrono::milliseconds(config_.timeoutMs);

    bool timedOut = false;
    int status = 0;

    while (true) {
        auto now = std::chrono::high_resolution_clock::now();
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - now).count();

        if (remaining <= 0) {
            // 超时 — 杀死子进程
            kill(pid, SIGKILL);
            timedOut = true;
            break;
        }

        // 非阻塞读取输出
        ssize_t n;
        while ((n = read(stdoutPipe[0], buffer, sizeof(buffer) - 1)) > 0) {
            buffer[n] = '\0';
            stdoutBuf += buffer;
        }
        while ((n = read(stderrPipe[0], buffer, sizeof(buffer) - 1)) > 0) {
            buffer[n] = '\0';
            stderrBuf += buffer;
        }

        // 检查子进程是否退出
        pid_t wpid = waitpid(pid, &status, WNOHANG);
        if (wpid == pid) {
            // 子进程已退出
            break;
        }

        // 短暂休眠
        usleep(10000);  // 10ms
    }

    // 如果超时后还没退出，强制杀死
    if (timedOut) {
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
    }

    // 最后一次读取剩余输出
    ssize_t n;
    while ((n = read(stdoutPipe[0], buffer, sizeof(buffer) - 1)) > 0) {
        buffer[n] = '\0';
        stdoutBuf += buffer;
    }
    while ((n = read(stderrPipe[0], buffer, sizeof(buffer) - 1)) > 0) {
        buffer[n] = '\0';
        stderrBuf += buffer;
    }

    close(stdoutPipe[0]);
    close(stderrPipe[0]);

    auto endTime = std::chrono::high_resolution_clock::now();

    // 填充结果
    result.stdout = stdoutBuf;
    result.stderr = stderrBuf;
    result.timedOut = timedOut;
    result.executionTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        endTime - startTime).count();

    if (timedOut) {
        result.success = false;
        result.errorMessage = "Execution timed out after " + std::to_string(config_.timeoutMs) + "ms";
    } else if (WIFEXITED(status)) {
        result.exitCode = WEXITSTATUS(status);
        result.success = (result.exitCode == 0);
        if (!result.success) {
            result.errorMessage = "Process exited with code " + std::to_string(result.exitCode);
        }
    } else if (WIFSIGNALED(status)) {
        result.signalNumber = WTERMSIG(status);
        result.success = false;
        result.errorMessage = "Process killed by signal " + std::to_string(result.signalNumber);
        if (result.signalNumber == SIGKILL) {
            result.errorMessage = "Process was killed (possibly resource limit exceeded)";
        } else if (result.signalNumber == SIGSEGV) {
            result.errorMessage = "Segmentation fault in sandboxed process";
        } else if (result.signalNumber == SIGXCPU) {
            result.errorMessage = "CPU time limit exceeded";
        } else if (result.signalNumber == SIGXFSZ) {
            result.errorMessage = "Output size limit exceeded";
        }
    }

    return result;
}

} // namespace clma
