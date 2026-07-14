cmake_minimum_required(VERSION 3.25)
project(fast_forward LANGUAGES CXX)

include(${CMAKE_CURRENT_LIST_DIR}/../common/mod_cmake/rexmod.cmake)

rexmod_add_plugin(fast_forward
    mod_main.cpp
)
