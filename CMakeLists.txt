# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Only bsp_extra (GPIO/LEDC/RMT waveform generator) is used by the ESP32
# DevKit V1 web-UI build. bsp_display/bsp_illuminate/bsp_i2c/bsp_stc8h1kxx
# remain under peripheral/ for the original ESP32-P4 CrowPanel build but are
# intentionally not registered here so they are not compiled or required.
set(EXTRA_COMPONENT_DIRS "${CMAKE_SOURCE_DIR}/peripheral/bsp_extra")
set(PROJECT_VER "0.6.0")
project(HeadlessScopeBuddy)
