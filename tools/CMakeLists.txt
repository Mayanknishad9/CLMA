# tools/CMakeLists.txt

add_executable(test_plugin_system test_plugin_system.cpp)
target_link_libraries(test_plugin_system core)
target_include_directories(test_plugin_system PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)

add_executable(test_agent_plugins test_agent_plugins.cpp)
target_link_libraries(test_agent_plugins core)
target_include_directories(test_agent_plugins PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)
# 导出全局符号，使 dlopen 加载的插件 .so 能解析到 AgentPlugin 的 typeinfo
set_target_properties(test_agent_plugins PROPERTIES
    LINK_FLAGS "-rdynamic"
)

add_executable(test_sandbox ../tests/test_sandbox.cpp)
target_link_libraries(test_sandbox core)
target_include_directories(test_sandbox PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)
