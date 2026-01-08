# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig")

# Determine subproject from build directory path
# Structure: build/<subproject>/<chip>/<name>/ (e.g., build/node/esp32h2/devkitm)
get_filename_component(BUILD_SETUP ${CMAKE_BINARY_DIR} NAME)        # devkitm
get_filename_component(BUILD_CHIP_DIR ${CMAKE_BINARY_DIR} DIRECTORY) # build/node/esp32h2
get_filename_component(BUILD_CHIP ${BUILD_CHIP_DIR} NAME)            # esp32h2
get_filename_component(BUILD_SUBPROJ_DIR ${BUILD_CHIP_DIR} DIRECTORY) # build/node
get_filename_component(SUBPROJECT ${BUILD_SUBPROJ_DIR} NAME)         # node

# Include the subproject and shared components
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_SOURCE_DIR}/${SUBPROJECT}"
    "${CMAKE_SOURCE_DIR}/components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(${SUBPROJECT})
