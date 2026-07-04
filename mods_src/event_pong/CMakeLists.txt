cmake_minimum_required(VERSION 3.25)
project(event_pong LANGUAGES CXX)

include(${CMAKE_CURRENT_LIST_DIR}/../common/mod_cmake/rexmod.cmake)

rexmod_add_plugin(event_pong
    mod_main.cpp
)
