cmake_minimum_required(VERSION 3.15)
project(ClosedLoopMultiAgent VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 输出目录
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# 依赖查找
find_package(yaml-cpp REQUIRED)
find_package(SQLite3 REQUIRED)

# 包含目录
include_directories(${CMAKE_SOURCE_DIR}/include)

# 子目录
add_subdirectory(src/core)

# 启用测试（必须在顶层调用才能注册 CTest）
enable_testing()

# add_subdirectory(src/agents)
# 工具
add_subdirectory(tools)
# add_subdirectory(src/utils)
add_subdirectory(tests)

# 插件构建（可选，需要位置无关代码）
option(BUILD_PLUGINS "Build plugin system and example plugins" ON)
if(BUILD_PLUGINS)
    add_subdirectory(plugins)
endif()

# Python 绑定
add_subdirectory(python_bindings)

# 可执行文件（测试用）
add_executable(main src/main.cpp)
target_link_libraries(main core)

# 安装配置
install(TARGETS main DESTINATION bin)
install(DIRECTORY include/ DESTINATION include)