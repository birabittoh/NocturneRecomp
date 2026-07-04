cmake_minimum_required(VERSION 3.25)
project(blackboard LANGUAGES CXX)

include(${CMAKE_CURRENT_LIST_DIR}/../common/mod_cmake/rexmod.cmake)

rexmod_add_plugin(blackboard
    mod_main.cpp
)
