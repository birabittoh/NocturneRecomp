
# nocturnerecomp - ReXGlue Recompiled Project
#
# This file is yours to edit. 'rexglue migrate' will NOT overwrite it.
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(nocturnerecomp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Project version: the nearest reachable `vX.Y.Z` git tag, not an exact
# match -- a dev build sitting on commits after v1.2.2 already contains
# everything v1.2.2 did, so it should still satisfy a mod's
# `game_version >= 1.2.0` (see nocturnerecomp_app.h's OnPreSetup and
# docs/making-mods.md). Falls back to "0.0.0" if git or a reachable tag is
# unavailable (e.g. a shallow/--depth=1 checkout with no tags fetched); the
# SDK already treats an unparsable/absent game_version as "can't verify,
# don't block" for any mod that declares the constraint.
find_package(Git QUIET)
set(NOCTURNE_VERSION "0.0.0")
if(GIT_EXECUTABLE)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} describe --tags --abbrev=0 --match "v[0-9]*.[0-9]*.[0-9]*"
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE NOCTURNE_GIT_TAG
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE NOCTURNE_GIT_TAG_RC)
    if(NOCTURNE_GIT_TAG_RC EQUAL 0 AND NOCTURNE_GIT_TAG MATCHES "^v([0-9]+\\.[0-9]+\\.[0-9]+)$")
        set(NOCTURNE_VERSION "${CMAKE_MATCH_1}")
    endif()
endif()
configure_file(
    ${CMAKE_SOURCE_DIR}/src/version.generated.h.in
    ${CMAKE_SOURCE_DIR}/src/version.generated.h
    @ONLY
)

# Embedded overlay fonts: src/fonts/*.ttf are the checked-in source of truth;
# src/fonts.generated.h is a gitignored build artifact regenerated whenever a
# .ttf or the generator script changes.
find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(NOCTURNE_FONTS_HEADER ${CMAKE_SOURCE_DIR}/src/fonts.generated.h)
add_custom_command(
    OUTPUT ${NOCTURNE_FONTS_HEADER}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/gen-fonts.py
    DEPENDS
        ${CMAKE_SOURCE_DIR}/scripts/gen-fonts.py
        ${CMAKE_SOURCE_DIR}/src/fonts/PTSerif-Regular.ttf
    COMMENT "Generating src/fonts.generated.h"
    VERBATIM
)

# Embedded window icon: extracted directly from assets/default.xex (already a
# required codegen input) so no extra asset needs to be checked in or fetched.
# src/icon.generated.h is a gitignored build artifact regenerated whenever the
# XEX or the generator script changes.
set(NOCTURNE_ICON_HEADER ${CMAKE_SOURCE_DIR}/src/icon.generated.h)
set(NOCTURNE_ICON_ICO ${CMAKE_SOURCE_DIR}/src/icon.generated.ico)
add_custom_command(
    OUTPUT ${NOCTURNE_ICON_HEADER} ${NOCTURNE_ICON_ICO}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/gen-icon.py
    DEPENDS
        ${CMAKE_SOURCE_DIR}/scripts/gen-icon.py
        ${CMAKE_SOURCE_DIR}/assets/default.xex
    COMMENT "Generating src/icon.generated.h and src/icon.generated.ico"
    VERBATIM
)

# On Windows, also embed the same icon as the .exe's own resource (Explorer,
# taskbar before the window opens, Alt-Tab), separate from the in-window icon
# applied at runtime via Window::SetIcon (see nocturnerecomp_app.h). The .rc
# needs an absolute path to the .ico since it's not necessarily emitted next
# to it in an out-of-source build.
if(WIN32)
    set(NOCTURNE_ICON_ICO_PATH ${NOCTURNE_ICON_ICO})
    set(NOCTURNE_ICON_RC ${CMAKE_SOURCE_DIR}/src/nocturnerecomp.generated.rc)
    configure_file(
        ${CMAKE_SOURCE_DIR}/src/nocturnerecomp.rc.in
        ${NOCTURNE_ICON_RC}
        @ONLY
    )
    # rc.exe needs icon.generated.ico to exist by the time it compiles this
    # .rc; configure_file only stamped the path into it above, it doesn't
    # establish a build-order dependency on the custom command that (re)writes
    # the .ico itself.
    set_source_files_properties(${NOCTURNE_ICON_RC} PROPERTIES OBJECT_DEPENDS ${NOCTURNE_ICON_ICO})
endif()

# Sources
set(NOCTURNERECOMP_SOURCES
    src/main.cpp
    src/nocturnerecomp_hooks.cpp
    src/accent_color.cpp
    src/achievements_menu.cpp
    src/fast_forward.cpp
    src/frame_pacer.cpp
    src/headless_gpu_bridge.cpp
    src/native_command_processor.cpp
    src/ppc_interpreter.cpp
    src/rando_xex.cpp
    src/room_presence.cpp
    ${NOCTURNE_FONTS_HEADER}
    ${NOCTURNE_ICON_HEADER}
    ${GENERATED_SOURCES}
)

if(WIN32)
    add_executable(nocturnerecomp WIN32 ${NOCTURNERECOMP_SOURCES} ${NOCTURNE_ICON_RC})
else()
    add_executable(nocturnerecomp ${NOCTURNERECOMP_SOURCES})
    set_target_properties(nocturnerecomp PROPERTIES
        BUILD_RPATH "$ORIGIN"
        INSTALL_RPATH "$ORIGIN"
    )
endif()

rexglue_setup_target(nocturnerecomp)

# native_command_processor.cpp's F2 shader-debugger support hashes ucode with
# XXH3 (matching the xenos/Vulkan backend's own Shader::ucode_data_hash()
# convention -- see NativeCommandProcessor::GetShaderSnapshot) so hashes are
# directly comparable to the old xenos-plugin renderer's. xxhash itself isn't
# part of rex::runtime's link interface (its only other caller,
# pipeline_cache.cpp, lives in the separate rexgpu-xenos plugin DLL, not the
# statically-linked runtime), so it needs linking explicitly here.
find_library(NOCTURNE_XXHASH_LIB NAMES xxhash PATHS ${CMAKE_SOURCE_DIR}/sdk/lib REQUIRED)
target_link_libraries(nocturnerecomp PRIVATE ${NOCTURNE_XXHASH_LIB})

# native_command_processor.cpp translates Xenos microcode to SPIR-V by hand
# (SpirvShaderTranslator, no CommandProcessor/PipelineCache dependency), so it
# needs the SDK's rexgpu-shader-translation library -- the same translation
# code the rexgpu-xenos plugin uses, but linkable directly without loading
# that runtime plugin. See docs/native-rendering.md.
target_link_libraries(nocturnerecomp PRIVATE rex::gpu-shader-translation)

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
