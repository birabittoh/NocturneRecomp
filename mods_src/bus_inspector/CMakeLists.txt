cmake_minimum_required(VERSION 3.25)
project(bus_inspector LANGUAGES CXX)

include(${CMAKE_CURRENT_LIST_DIR}/../common/mod_cmake/rexmod.cmake)

rexmod_add_plugin(bus_inspector
    mod_main.cpp
)
