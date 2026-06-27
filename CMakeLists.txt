
# nocturnerecomp - ReXGlue Recompiled Project
#
# This file is yours to edit. 'rexglue migrate' will NOT overwrite it.
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(nocturnerecomp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
set(NOCTURNERECOMP_SOURCES
    src/main.cpp
    src/nocturnerecomp_hooks.cpp
    src/achievements_menu.cpp
    ${GENERATED_SOURCES}
)

if(WIN32)
    add_executable(nocturnerecomp WIN32 ${NOCTURNERECOMP_SOURCES})
else()
    add_executable(nocturnerecomp ${NOCTURNERECOMP_SOURCES})
    set_target_properties(nocturnerecomp PROPERTIES
        BUILD_RPATH "$ORIGIN"
        INSTALL_RPATH "$ORIGIN"
    )
endif()

rexglue_setup_target(nocturnerecomp)
