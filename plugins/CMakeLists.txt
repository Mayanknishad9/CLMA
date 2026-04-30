# plugins/CMakeLists.txt

# ============================================================
# Agent 插件 — 每个插件编译为独立的 .so
# 对应 Orchestrator 的 5 个步骤，通过 PluginManager 动态加载
# ============================================================

# 插件管理器编译为静态库（内置于 core）
# PluginInterface.hpp / AgentPlugin.hpp 已在 include/core/ 中

# ---- 预编译插件列表 ----
set(AGENT_PLUGINS
    agent_refiner
    agent_reasoner
    agent_solver
    agent_verifier
    agent_evaluator
)

# 创建统一的插件输出目录
set(PLUGIN_OUTPUT_DIR ${CMAKE_BINARY_DIR}/lib)

# 为每个插件创建共享库
foreach(PLUGIN ${AGENT_PLUGINS})
    add_library(${PLUGIN} SHARED
        ${PLUGIN}/plugin.cpp
    )

    target_include_directories(${PLUGIN} PRIVATE
        ${CMAKE_SOURCE_DIR}/include
    )

    # 链接 core 库，确保插件 .so 能解析 AgentPlugin 符号
    target_link_libraries(${PLUGIN} core)

    # 设置输出目录
    set_target_properties(${PLUGIN} PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${PLUGIN_OUTPUT_DIR}"
        # 可选：移除 lib 前缀，使插件名更清晰
        # PREFIX ""
    )

    # 安装
    install(TARGETS ${PLUGIN}
        LIBRARY DESTINATION plugins
    )
endforeach()

# 示例插件（保留）
add_library(example_tool SHARED
    example_tool/plugin.cpp
)

target_include_directories(example_tool PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)

# 示例插件只使用 PluginInterface，不需要链接 core
target_link_libraries(example_tool core)

set_target_properties(example_tool PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${PLUGIN_OUTPUT_DIR}"
)

install(TARGETS example_tool
    LIBRARY DESTINATION plugins
)
