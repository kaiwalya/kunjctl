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

# Optional: esp-matter support (requires Docker build with ESP_MATTER_PATH)
if(DEFINED ENV{ESP_MATTER_PATH})
    set(ESP_MATTER_PATH $ENV{ESP_MATTER_PATH})
    set(MATTER_SDK_PATH ${ESP_MATTER_PATH}/connectedhomeip/connectedhomeip)
    list(APPEND EXTRA_COMPONENT_DIRS
        "${ESP_MATTER_PATH}/components"
        "${MATTER_SDK_PATH}/config/esp32/components")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(${SUBPROJECT})

# Matter build options (only when esp-matter is available)
if(DEFINED ENV{ESP_MATTER_PATH})
    idf_build_set_property(CXX_COMPILE_OPTIONS "-std=gnu++17;-Os;-DCHIP_HAVE_CONFIG_H" APPEND)
    idf_build_set_property(C_COMPILE_OPTIONS "-Os" APPEND)
    idf_build_set_property(COMPILE_OPTIONS "-Wno-format-nonliteral;-Wno-format-security" APPEND)
endif()
