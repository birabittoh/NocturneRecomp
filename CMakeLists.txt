
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

# Title-update builds: the achievements bridge hooks the game's XAM thunks by
# their hardcoded vanilla guest addresses (sub_825D8028 etc.). The title update
# relocates the whole image, so those addresses don't exist in the patched image
# and codegen never emits the matching sub_* bodies. -DNOCTURNE_TU=ON compiles
# those hooks out (the achievements overlay is simply inert in TU builds); the
# regular build leaves it OFF and keeps the feature. Set by scripts/build.py --tu.
option(NOCTURNE_TU "Build for a baked-in title update (disables vanilla-address hooks)" OFF)
if(NOCTURNE_TU)
    target_compile_definitions(nocturnerecomp PRIVATE NOCTURNE_TU=1)
endif()
