cmake_minimum_required(VERSION 3.25)
project(event_ping LANGUAGES CXX)

include(${CMAKE_CURRENT_LIST_DIR}/../common/mod_cmake/rexmod.cmake)

rexmod_add_plugin(event_ping
    mod_main.cpp
)
