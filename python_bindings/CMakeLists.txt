# python_bindings/CMakeLists.txt
# Python bindings using pybind11

# Find pybind11 (from venv or system)
set(PYBIND11_DIR "/root/closed-loop-multiagent/venv/lib/python3.14/site-packages/pybind11/share/cmake/pybind11")
list(APPEND CMAKE_PREFIX_PATH "${PYBIND11_DIR}")
find_package(pybind11 REQUIRED)

# Get Python include dirs from the venv
set(PYTHON_INCLUDE_DIRS "/root/closed-loop-multiagent/venv/include")
set(PYTHON_LIBRARIES "")

# Build the Python module
pybind11_add_module(clma_core
    bindings.cpp
)

target_include_directories(clma_core
    PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${PYTHON_INCLUDE_DIRS}
)

target_link_libraries(clma_core
    PRIVATE
        core
        pybind11::headers
)

# Set output directory so Python can find it
set_target_properties(clma_core
    PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/python_interface"
        OUTPUT_NAME "clma_core"
)
