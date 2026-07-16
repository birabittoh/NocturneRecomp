cmake_minimum_required(VERSION 3.25)
project(function_override_demo LANGUAGES CXX)

include(${CMAKE_CURRENT_LIST_DIR}/../common/mod_cmake/rexmod.cmake)

rexmod_add_plugin(function_override_demo
    mod_main.cpp
)
