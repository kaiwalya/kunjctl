# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig")

# Determine subproject from build directory path (e.g., build/node/esp32h2 -> node)
get_filename_component(BUILD_CHIP ${CMAKE_BINARY_DIR} NAME)
get_filename_component(BUILD_PARENT ${CMAKE_BINARY_DIR} DIRECTORY)
get_filename_component(SUBPROJECT ${BUILD_PARENT} NAME)

# Include the subproject and shared components
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_SOURCE_DIR}/${SUBPROJECT}"
    "${CMAKE_SOURCE_DIR}/components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(${SUBPROJECT})
