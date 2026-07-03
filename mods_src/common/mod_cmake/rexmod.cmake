# rexmod.cmake - shared CMake helper for mods_src/* code mods.
#
# Include this (after project()) from a mod's own CMakeLists.txt, then call
# rexmod_add_plugin() instead of add_library(). Keeps every mod's build
# config (C++ standard, rex::runtime link, toolchain expectations) consistent
# with scripts/build.py's own settings, since a mod DLL must be ABI-compatible
# with the exe it loads into.
cmake_minimum_required(VERSION 3.25)

if(NOT TARGET rex::runtime)
    find_package(rexglue CONFIG REQUIRED)
endif()

function(rexmod_add_plugin target_name)
    add_library(${target_name} SHARED ${ARGN})
    set_target_properties(${target_name} PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
    )
    # PRIVATE: a mod plugin has no consumers of its own. Linking rex::runtime
    # (never the rexcore/rexui OBJECT libraries) reuses the same in-process
    # ImGui drawer, keybind registry, and kernel state as the host exe --
    # mirrors how the rexgpu-xenos GPU plugin links against rexruntime.
    target_link_libraries(${target_name} PRIVATE rex::runtime)
endfunction()
