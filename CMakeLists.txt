# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# (Not part of the boilerplate)
# This example uses an extra component for common functions such as Wi-Fi and Ethernet connection.
set(EXTRA_COMPONENT_DIRS
    $ENV{IDF_PATH}/examples/common_components/protocol_examples_common
)

# 本地已有 managed_components 时禁用组件管理器，避免联网访问 components registry
# （当前环境 DNS 无法解析 api.components.espressif.com 会导致配置失败）
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/managed_components")
    set(ENV{IDF_COMPONENT_MANAGER} "0")
    list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/managed_components")
    message(STATUS "Using local managed_components (IDF_COMPONENT_MANAGER=0, offline)")
endif()

include_directories(${CMAKE_CURRENT_SOURCE_DIR})

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(ESP32_DAPLink)
