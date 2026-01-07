# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig")

# Tell ESP-IDF that "node" is the main component (not default "main")
set(EXTRA_COMPONENT_DIRS "${CMAKE_SOURCE_DIR}/node")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(node)
