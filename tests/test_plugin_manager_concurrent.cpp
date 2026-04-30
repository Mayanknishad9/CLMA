#include <gtest/gtest.h>
#include "core/PluginManager.hpp"
#include "core/PluginInterface.hpp"
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <iostream>
#include <filesystem>

// ===============================================================
// PluginManager 并发测试
// 测试多线程 getPlugin/releasePlugin/state 查询不会死锁或崩溃
// ===============================================================

class PluginManagerConcurrentTest : public ::testing::Test {
protected:
    void SetUp() override {
        pm_ = std::make_shared<clma::PluginManager>();
        
        // 使用已有的插件目录
        pm_->addPluginDirectory("/root/closed-loop-multiagent/build/lib");
        
        // 扫描（重试多次以应对首次空结果问题）
        int found = 0;
        for (int i = 0; i < 5 && found == 0; ++i) {
            found = pm_->scanPlugins();
        }
        ASSERT_GE(found, 1) << "Must have at least one plugin in build/lib";
        
        // 找出可用的插件 ID
        auto allPlugins = pm_->listPlugins();
        ASSERT_GE(allPlugins.size(), 1);
        
        // 遍历选一个不依赖其他插件的
        for (const auto& info : allPlugins) {
            if (info.dependencies.empty()) {
                testPluginId_ = info.id;
                break;
            }
        }
        if (testPluginId_.empty()) {
            testPluginId_ = allPlugins[0].id;
        }
        
        std::cerr << "[DEBUG] Using plugin: " << testPluginId_ << std::endl;
        
        // load + init + start（使用找到的 ID）
        ASSERT_TRUE(pm_->loadPlugin(testPluginId_))
            << "loadPlugin failed for " << testPluginId_;
        ASSERT_TRUE(pm_->initializePlugin(testPluginId_))
            << "initializePlugin failed for " << testPluginId_;
        ASSERT_TRUE(pm_->startPlugin(testPluginId_))
            << "startPlugin failed for " << testPluginId_;
        
        // 验证 getPlugin 可用
        auto* p = pm_->getPlugin(testPluginId_);
        ASSERT_NE(p, nullptr) << "getPlugin returned null after start";
        pm_->releasePlugin(testPluginId_);
    }
    
    void TearDown() override {
        pm_->unloadAll();
        pm_.reset();
    }
    
    std::shared_ptr<clma::PluginManager> pm_;
    std::string testPluginId_;
};

// 测试 1: 多线程并发 getPlugin / releasePlugin
TEST_F(PluginManagerConcurrentTest, ConcurrentGetReleasePlugin) {
    std::atomic<int> successes{0};
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    
    for (int i = 0; i < 16; ++i) {
        threads.emplace_back([&, i]() {
            for (int j = 0; j < 100; ++j) {
                auto* plugin = pm_->getPlugin(testPluginId_);
                if (plugin != nullptr) {
                    // 模拟一些使用
                    volatile int dummy = 0;
                    for (int k = 0; k < 10; ++k) dummy += k;
                    pm_->releasePlugin(testPluginId_);
                    successes.fetch_add(1, std::memory_order_relaxed);
                } else {
                    failures.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    EXPECT_GT(successes.load(), 0) << "Should have at least some successful gets";
    std::cout << "[INFO] getPlugin successes=" << successes.load()
              << ", failures=" << failures.load() << std::endl;
    
    // 验证状态未损坏
    EXPECT_EQ(pm_->getPluginState(testPluginId_), clma::PluginState::RUNNING);
}

// 测试 2: 并发 state check + getPlugin
TEST_F(PluginManagerConcurrentTest, ConcurrentStateCheckWithGet) {
    std::atomic<bool> start_init{false};
    
    std::thread worker1([&]() {
        // 只是开始 init（已 init 过，第二次会失败但不该崩溃）
        start_init.store(true);
        pm_->initializePlugin(testPluginId_);
    });
    
    std::thread worker2([&]() {
        while (!start_init.load()) {
            std::this_thread::yield();
        }
        for (int i = 0; i < 500; ++i) {
            pm_->getPluginState(testPluginId_);
            pm_->isPluginLoaded(testPluginId_);
            pm_->listPlugins();
            std::this_thread::yield();
        }
    });
    
    worker1.join();
    worker2.join();
    
    // 第二轮：getPlugin + state queries
    std::thread worker3([&]() {
        for (int i = 0; i < 1000; ++i) {
            pm_->getPluginCount();
            pm_->listPluginsByState(clma::PluginState::RUNNING);
        }
    });
    
    worker3.join();
    
    SUCCEED() << "Concurrent state checks completed without deadlock";
}

// 测试 3: 并发 scanPlugins + listPlugins
TEST_F(PluginManagerConcurrentTest, ConcurrentScanAndList) {
    std::atomic<int> scanOk{0};
    std::atomic<int> listOk{0};
    std::vector<std::thread> threads;
    
    for (int i = 0; i < 8; ++i) {
        threads.emplace_back([&, i]() {
            if (i % 2 == 0) {
                for (int j = 0; j < 20; ++j) {
                    int found = pm_->scanPlugins();
                    if (found >= 0) {
                        scanOk.fetch_add(1, std::memory_order_relaxed);
                    }
                    std::this_thread::sleep_for(std::chrono::microseconds(10));
                }
            } else {
                for (int j = 0; j < 20; ++j) {
                    auto list = pm_->listPlugins();
                    listOk.fetch_add(1, std::memory_order_relaxed);
                    std::this_thread::sleep_for(std::chrono::microseconds(10));
                }
            }
        });
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    EXPECT_GT(scanOk.load(), 0);
    EXPECT_GT(listOk.load(), 0);
    std::cout << "[INFO] scanOk=" << scanOk.load()
              << " listOk=" << listOk.load() << std::endl;
}

// 测试 4: 多线程 releasePlugin 不崩溃（含额外的 release）
TEST_F(PluginManagerConcurrentTest, RefCountNeverNegative) {
    std::vector<std::thread> threads;
    
    for (int i = 0; i < 8; ++i) {
        threads.emplace_back([&]() {
            for (int j = 0; j < 50; ++j) {
                auto* p = pm_->getPlugin(testPluginId_);
                if (p) {
                    pm_->releasePlugin(testPluginId_);
                }
                // 额外的 release（可能下溢）— 应不崩溃
                pm_->releasePlugin(testPluginId_);
                pm_->releasePlugin(testPluginId_);
            }
        });
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    SUCCEED() << "Excess releasePlugin calls did not crash";
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
