# src/core/CMakeLists.txt

# 核心库 — 使用 SHARED 确保插件 .so 能通过 dlopen 解析 RTTI/typeinfo
# 不能使用 STATIC，否则动态加载的插件 .so 找不到 AgentPlugin 等核心符号
add_library(core SHARED
    Types.cpp
    RuleEngine.cpp
    TokenMonitor.cpp
    LoopController.cpp
    Orchestrator.cpp
    PluginManager.cpp
    AgentPlugin.cpp
    PluginWatcher.cpp
    Sandbox.cpp
)

target_include_directories(core PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

target_compile_options(core PUBLIC -fPIC)

target_link_libraries(core
    yaml-cpp::yaml-cpp
    SQLite3::SQLite3
    seccomp
)

# 设置 RPATH 让测试/主程序运行时能找到 libcore.so
# $ORIGIN/../lib 相对于可执行文件位置的 lib/ 目录
set_target_properties(core PROPERTIES
    INSTALL_RPATH "$ORIGIN/../lib"
    BUILD_RPATH "$ORIGIN/../lib"
)

target_compile_definitions(core PUBLIC HAS_YAML_CPP)