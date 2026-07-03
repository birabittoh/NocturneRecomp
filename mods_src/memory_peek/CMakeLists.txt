cmake_minimum_required(VERSION 3.25)
project(memory_peek LANGUAGES CXX)

include(${CMAKE_CURRENT_LIST_DIR}/../common/mod_cmake/rexmod.cmake)

rexmod_add_plugin(memory_peek
    mod_main.cpp
)
