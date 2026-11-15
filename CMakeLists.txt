# The following lines of boilerplate have to be in your project's CMakeLists
# in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)
set(PROJECT_NAME "esp32_meizu_remoter_gateway")
set(PROJECT_VER "0.4.0-ack1")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_meizu_remoter_gateway  C CXX)
