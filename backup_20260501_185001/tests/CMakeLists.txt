# tests/CMakeLists.txt

find_package(GTest REQUIRED)

# 启用测试
enable_testing()

# 添加测试可执行文件
add_executable(test_types test_types.cpp)
target_link_libraries(test_types core GTest::gtest GTest::gtest_main)

add_executable(test_rule_engine test_rule_engine.cpp)
target_link_libraries(test_rule_engine core GTest::gtest GTest::gtest_main)

add_executable(test_token_monitor test_token_monitor.cpp)
target_link_libraries(test_token_monitor core GTest::gtest GTest::gtest_main)

add_executable(test_loop_controller test_loop_controller.cpp)
target_link_libraries(test_loop_controller core GTest::gtest GTest::gtest_main)

add_executable(test_orchestrator test_orchestrator.cpp)
target_link_libraries(test_orchestrator core GTest::gtest GTest::gtest_main)

add_executable(test_orchestrator_plugins test_orchestrator_plugins.cpp)
target_link_libraries(test_orchestrator_plugins core GTest::gtest GTest::gtest_main)
# 导出全局符号，使 dlopen 加载的插件 .so 能解析到 AgentPlugin 的 typeinfo
set_target_properties(test_orchestrator_plugins PROPERTIES
    LINK_FLAGS "-rdynamic"
)

# 添加测试
add_test(NAME test_types COMMAND test_types)
add_test(NAME test_rule_engine COMMAND test_rule_engine)
add_test(NAME test_token_monitor COMMAND test_token_monitor)
add_test(NAME test_loop_controller COMMAND test_loop_controller)
add_test(NAME test_orchestrator COMMAND test_orchestrator)
# ==================== 新增缺口填补测试 ====================

# PluginManager 并发测试
add_executable(test_plugin_concurrent test_plugin_manager_concurrent.cpp)
target_link_libraries(test_plugin_concurrent core GTest::gtest GTest::gtest_main)

# PluginManager crash recovery 测试
add_executable(test_plugin_recovery test_plugin_manager_recovery.cpp)
target_link_libraries(test_plugin_recovery core GTest::gtest GTest::gtest_main)

# PluginWatcher 集成测试
add_executable(test_plugin_watcher test_plugin_watcher.cpp)
target_link_libraries(test_plugin_watcher core GTest::gtest GTest::gtest_main)

# TokenMonitor 大内容边界测试
add_executable(test_token_monitor_edge test_token_monitor_edge.cpp)
target_link_libraries(test_token_monitor_edge core GTest::gtest GTest::gtest_main)

add_test(NAME test_orchestrator_plugins COMMAND test_orchestrator_plugins)

# 新增测试
add_test(NAME test_plugin_concurrent COMMAND test_plugin_concurrent)
add_test(NAME test_plugin_recovery COMMAND test_plugin_recovery)
add_test(NAME test_plugin_watcher COMMAND test_plugin_watcher)
add_test(NAME test_token_monitor_edge COMMAND test_token_monitor_edge)

# DAG 规划测试
add_executable(test_dag test_dag.cpp)
target_link_libraries(test_dag core GTest::gtest GTest::gtest_main)
add_test(NAME test_dag COMMAND test_dag)