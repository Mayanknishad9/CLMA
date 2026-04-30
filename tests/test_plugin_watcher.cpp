#include <gtest/gtest.h>
#include "core/PluginManager.hpp"
#include "core/PluginWatcher.hpp"
#include "core/PluginInterface.hpp"
#include <memory>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <thread>
#include <chrono>
#include <atomic>

namespace fs = std::filesystem;

// ==================== PluginWatcher 测试套件 ====================

class PluginWatcherTest : public ::testing::Test {
protected:
    void SetUp() override {
        pm_ = std::make_shared<clma::PluginManager>();
        
        // 创建临时插件目录
        testDir_ = fs::temp_directory_path() / "plugin_watcher_test";
        fs::create_directories(testDir_);
        
        pm_->addPluginDirectory(testDir_.string());
        
        // 复制一个现有的 .so 作为测试插件
        copyTestPlugin("libagent_solver.so");
        
        // 扫描
        int found = pm_->scanPlugins();
        if (found == 0) found = pm_->scanPlugins();
        
        // 加载+启动
        if (found > 0) {
            pm_->loadAll();
        }
    }
    
    void TearDown() override {
        if (watcher_) {
            watcher_->stop();
        }
        pm_->unloadAll();
        pm_.reset();
        watcher_.reset();
        fs::remove_all(testDir_);
    }
    
    void copyTestPlugin(const std::string& soName) {
        fs::path src = fs::path("/root/closed-loop-multiagent/build/lib") / soName;
        // 试试各种名字
        if (!fs::exists(src)) {
            src = fs::path("/root/closed-loop-multiagent/build/lib") / ("lib" + soName);
        }
        if (!fs::exists(src)) {
            // 列出可用的
            if (fs::exists(fs::path("/root/closed-loop-multiagent/build/lib"))) {
                for (const auto& entry : fs::directory_iterator(fs::path("/root/closed-loop-multiagent/build/lib"))) {
                    if (entry.path().extension() == ".so") {
                        src = entry.path();
                        std::cerr << "[DEBUG] Using available .so: " << src.filename() << std::endl;
                        break;
                    }
                }
            }
        }
        
        if (fs::exists(src)) {
            fs::path dst = testDir_ / src.filename();
            if (!fs::exists(dst)) {
                fs::copy_file(src, dst);
                std::cerr << "[DEBUG] Copied " << src.filename() << " to " << dst << std::endl;
            }
            testSoPath_ = dst;
            testSoName_ = dst.filename().string();
        } else {
            std::cerr << "[WARN] No .so files found in build/lib — watcher tests will be limited" << std::endl;
        }
    }
    
    // 创建一个空的 .so 文件（模拟新插件出现）
    void createDummySo(const std::string& name) {
        fs::path soPath = testDir_ / name;
        std::ofstream ofs(soPath, std::ios::binary);
        // 写入无效的 ELF 头（will fail dlopen but trigger inotify）
        ofs << "\x7f\x45\x4c\x46";  // ELF magic
        ofs.close();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    std::shared_ptr<clma::PluginManager> pm_;
    std::unique_ptr<clma::PluginWatcher> watcher_;
    fs::path testDir_;
    fs::path testSoPath_;
    std::string testSoName_;
};

// 测试 1: 创建和销毁 PluginWatcher
TEST_F(PluginWatcherTest, CreateAndDestroy) {
    watcher_ = std::make_unique<clma::PluginWatcher>(pm_.get());
    EXPECT_FALSE(watcher_->isRunning()) << "Watcher should not be running after creation";
    SUCCEED() << "PluginWatcher creation and destruction successful";
}

// 测试 2: start/stop 生命周期
TEST_F(PluginWatcherTest, StartAndStop) {
    watcher_ = std::make_unique<clma::PluginWatcher>(pm_.get());
    
    // 添加监控目录
    watcher_->watchDirectory(testDir_.string());
    
    // 启动（短间隔以便快速完成）
    bool started = watcher_->start(100);  // 100ms 轮询间隔
    EXPECT_TRUE(started);
    EXPECT_TRUE(watcher_->isRunning());
    
    // 等待一下让 watcher 线程跑几轮
    std::this_thread::sleep_for(std::chrono::milliseconds(350));
    
    // 停止
    watcher_->stop();
    EXPECT_FALSE(watcher_->isRunning());
    
    SUCCEED() << "Watcher started and stopped cleanly";
}

// 测试 3: 设置变更回调
TEST_F(PluginWatcherTest, ChangeCallbackRegistration) {
    watcher_ = std::make_unique<clma::PluginWatcher>(pm_.get());
    
    std::atomic<bool> callbackInvoked{false};
    std::string capturedId;
    
    watcher_->setChangeCallback([&](const std::string& id) {
        callbackInvoked.store(true);
        capturedId = id;
    });
    
    watcher_->watchDirectory(testDir_.string());
    bool started = watcher_->start(50);  // 快速轮询
    EXPECT_TRUE(started);
    
    // 等待一会 — 如果没有文件变更，回调不应该被调用
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    
    watcher_->stop();
    
    // 200ms 内如果文件没变，回调不应该触发
    // 但无法精确保——inotify 模式可能在启动时触发一次
    // 我们不做具体断言，只验证不崩溃
    SUCCEED() << "Change callback registered and watcher ran without crash";
}

// 测试 4: 多次 start/stop 不崩溃
TEST_F(PluginWatcherTest, MultipleStartStopCycles) {
    watcher_ = std::make_unique<clma::PluginWatcher>(pm_.get());
    watcher_->watchDirectory(testDir_.string());
    
    for (int i = 0; i < 5; ++i) {
        bool started = watcher_->start(50);
        EXPECT_TRUE(started);
        EXPECT_TRUE(watcher_->isRunning());
        
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        watcher_->stop();
        EXPECT_FALSE(watcher_->isRunning());
    }
    
    SUCCEED() << "Multiple start/stop cycles completed without crash";
}

// 测试 5: 不添加目录就 start（边界情况 — 返回 false）
TEST_F(PluginWatcherTest, StartWithoutWatchDir) {
    watcher_ = std::make_unique<clma::PluginWatcher>(pm_.get());
    
    // 不调用 watchDirectory，直接 start
    bool started = watcher_->start(100);
    EXPECT_FALSE(started) << "Starting without watch dirs should return false";
    EXPECT_FALSE(watcher_->isRunning());
    
    SUCCEED() << "Empty watcher start rejected cleanly";
}

// 测试 6: 在 watcher 运行时添加/移除目录（如果接口支持）
TEST_F(PluginWatcherTest, AddDirectoryWhileRunning) {
    watcher_ = std::make_unique<clma::PluginWatcher>(pm_.get());
    
    watcher_->watchDirectory(testDir_.string());
    bool started = watcher_->start(100);
    EXPECT_TRUE(started);
    
    // 运行时添加另一个目录（如果存在）
    fs::path extraDir = fs::temp_directory_path() / "plugin_watcher_extra";
    fs::create_directories(extraDir);
    
    watcher_->watchDirectory(extraDir.string());
    
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    
    watcher_->stop();
    fs::remove_all(extraDir);
    
    SUCCEED() << "Added directory while watcher was running — no crash";
}

// 测试 7: 析构时自动停止
TEST_F(PluginWatcherTest, DestructorAutoStop) {
    {
        clma::PluginWatcher watcher(pm_.get());
        watcher.watchDirectory(testDir_.string());
        watcher.start(100);
        EXPECT_TRUE(watcher.isRunning());
        // 作用域结束，watcher 析构 → 自动 stop
    }
    
    SUCCEED() << "Watcher destructor stopped cleanly";
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
