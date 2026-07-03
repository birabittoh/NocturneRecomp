cmake_minimum_required(VERSION 3.25)
project(music_player LANGUAGES CXX)

include(${CMAKE_CURRENT_LIST_DIR}/../common/mod_cmake/rexmod.cmake)

rexmod_add_plugin(music_player
    audio_player.cpp
    mod_main.cpp
)
