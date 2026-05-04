#include "core/Sandbox.hpp"
#include <iostream>
#include <cstring>
#include <unistd.h>

int test_basic_execution() {
    clma::Sandbox sandbox(clma::SandboxConfig::strict());
    sandbox.setConfig(clma::SandboxConfig::strict());

    // 测试 1: 基本执行 — 简单函数
    std::cout << "--- Test 1: Basic execution ---" << std::endl;
    {
        auto result = sandbox.execute([]() -> int {
            // 简单的写 stdout
            write(STDOUT_FILENO, "Hello from sandbox!\n", 20);
            return 0;
        });

        if (!result.success) {
            std::cerr << "FAIL: Basic execution failed: " << result.errorMessage << std::endl;
            return 1;
        }
        if (result.stdout.find("Hello from sandbox") == std::string::npos) {
            std::cerr << "FAIL: stdout mismatch. Got: " << result.stdout << std::endl;
            return 1;
        }
        if (result.executionTimeMs <= 0) {
            std::cerr << "FAIL: execution time not recorded" << std::endl;
            return 1;
        }
        std::cout << "  [PASS] stdout='" << result.stdout.substr(0, 40) << "..." << "'"
                  << " time=" << result.executionTimeMs << "ms" << std::endl;
    }

    // 测试 2: 非零退出码
    std::cout << "--- Test 2: Non-zero exit ---" << std::endl;
    {
        auto result = sandbox.execute([]() -> int {
            return 42;
        });

        if (result.success) {
            std::cerr << "FAIL: Expected failure for exit code 42" << std::endl;
            return 1;
        }
        if (result.exitCode != 42) {
            std::cerr << "FAIL: Expected exit code 42, got " << result.exitCode << std::endl;
            return 1;
        }
        std::cout << "  [PASS] exitCode=" << result.exitCode << std::endl;
    }

    // 测试 3: 超时
    std::cout << "--- Test 3: Timeout ---" << std::endl;
    {
        clma::SandboxConfig cfg;
        cfg.timeoutMs = 100;  // 100ms 超时
        clma::Sandbox fastSandbox(cfg);

        auto result = fastSandbox.execute([]() -> int {
            // 死循环
            volatile int x = 0;
            while (true) {
                x++;
            }
            return 0;
        });

        if (!result.timedOut) {
            std::cerr << "FAIL: Expected timeout, but didn't happen" << std::endl;
            std::cerr << "  result: success=" << result.success
                      << " timedOut=" << result.timedOut
                      << " time=" << result.executionTimeMs << "ms" << std::endl;
            return 1;
        }
        std::cout << "  [PASS] Timed out after " << result.executionTimeMs << "ms" << std::endl;
    }

    // 测试 4: stderr 捕获
    std::cout << "--- Test 4: stderr capture ---" << std::endl;
    {
        auto result = sandbox.execute([]() -> int {
            write(STDERR_FILENO, "error message\n", 14);
            return 1;
        });

        if (result.success) {
            std::cerr << "FAIL: Expected failure" << std::endl;
            return 1;
        }
        if (result.stderr.find("error message") == std::string::npos) {
            std::cerr << "FAIL: stderr mismatch. Got: " << result.stderr << std::endl;
            return 1;
        }
        std::cout << "  [PASS] stderr='" << result.stderr.substr(0, 40) << "'" << std::endl;
    }

    // 测试 5: seccomp 可用性检测
    std::cout << "--- Test 5: seccomp availability ---" << std::endl;
    {
        bool available = clma::Sandbox::isSeccompAvailable();
        std::cout << "  seccomp available: " << (available ? "yes" : "no") << std::endl;
    }

    // 测试 6: 系统支持检测
    std::cout << "--- Test 6: system support ---" << std::endl;
    {
        bool supported = clma::Sandbox::isSupported();
        if (!supported) {
            std::cerr << "FAIL: Sandbox not supported on this system" << std::endl;
            return 1;
        }
        std::cout << "  [PASS] Sandbox supported" << std::endl;
    }

    std::cout << "\n=== All sandbox tests passed ===" << std::endl;
    return 0;
}

int main() {
    return test_basic_execution();
}
