cmake_minimum_required(VERSION 3.25)
project(game_symbols LANGUAGES CXX)

include(${CMAKE_CURRENT_LIST_DIR}/../common/mod_cmake/rexmod.cmake)

rexmod_add_plugin(game_symbols
    mod_main.cpp
)
