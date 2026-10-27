
# nocturnerecomp - ReXGlue Recompiled Project
#
# This file is yours to edit. 'rexglue migrate' will NOT overwrite it.
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
# C is here only for the embedded shader blobs: Plume's shader helper emits each
# compiled blob as a .c byte array, and CMake needs a C compiler to build them.
project(nocturnerecomp LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Plume, the RHI the native renderer is built on (see src/gpu/native_renderer.h).
# It is a low level interface over D3D12, Vulkan and Metal that does no shader
# compilation of its own, which is what makes it the right fit here: the guest's
# shaders are translated offline by KryptonRecomp rather than at runtime. Both
# live under ext/ as submodules so local and CI builds use the same revisions.
set(NOCTURNE_PLUME_DIR "${CMAKE_SOURCE_DIR}/ext/plume")
if(NOT EXISTS "${NOCTURNE_PLUME_DIR}/plume_render_interface.h")
    message(FATAL_ERROR
        "Plume submodule is missing. Run: git submodule update --init --recursive")
endif()
# ReXGlue owns this project's SDL window. Have Plume create its Vulkan surface
# from that SDL_Window directly instead of asking the host to expose raw X11
# handles, which Window::GetNativeWindowHandle() does not represent. Android has
# native VK_KHR_android_surface support, and Apple must keep Plume's own
# {window, view} RenderWindow (fed to vkCreateMetalSurfaceEXT), so neither gets it.
if(UNIX AND NOT APPLE AND NOT ANDROID)
    set(PLUME_SDL3_VULKAN_ENABLED ON CACHE BOOL "Enable SDL3 Vulkan integration" FORCE)
endif()
add_subdirectory("${NOCTURNE_PLUME_DIR}" "${CMAKE_BINARY_DIR}/plume" EXCLUDE_FROM_ALL)

# Plume's D3D12 backend returns D3D12_HEAP_TYPE_GPU_UPLOAD from its heap type
# mapping without guarding it, so it only builds against a Windows SDK new
# enough to declare it (10.0.26100+). Supply the one missing enumerator, but
# only after checking it is actually missing. 5 is the value the Agility SDK
# assigns it; nothing here requests a GPU upload heap, so it only has to compile.
if(WIN32)
    include(CheckCXXSourceCompiles)
    set(CMAKE_REQUIRED_QUIET ON)
    check_cxx_source_compiles(
        "#include <d3d12.h>\nint main() { return D3D12_HEAP_TYPE_GPU_UPLOAD; }"
        NOCTURNE_HAVE_D3D12_GPU_UPLOAD_HEAP)
    unset(CMAKE_REQUIRED_QUIET)
    if(NOT NOCTURNE_HAVE_D3D12_GPU_UPLOAD_HEAP)
        message(STATUS "nocturnerecomp - Windows SDK predates D3D12_HEAP_TYPE_GPU_UPLOAD, shimming it for Plume")
        target_compile_definitions(plume PRIVATE
            "D3D12_HEAP_TYPE_GPU_UPLOAD=((D3D12_HEAP_TYPE)5)")
    endif()
endif()

# The D3D12 debug layer, opt in with -DNOCTURNE_D3D12_DEBUG_LAYER=ON. Plume only
# enables it when NDEBUG is off, which this project's default build is not.
option(NOCTURNE_D3D12_DEBUG_LAYER "Enable Plume's D3D12 debug layer" OFF)
if(WIN32 AND NOCTURNE_D3D12_DEBUG_LAYER)
    target_compile_definitions(plume PRIVATE
        D3D12_DEBUG_LAYER_ENABLED
        D3D12_DEBUG_LAYER_BREAK_ON_ERROR=false
        D3D12_DEBUG_LAYER_BREAK_ON_WARNING=false
        D3D12_DEBUG_LAYER_SUPRESS_SAMPLE_POSITIONS_ERROR)
endif()

# Plume's shader helper fetches a DXC build (the Windows SDK's has no SPIR-V
# backend). NOCTURNE_DXC_SOURCE_DIR points FetchContent at an existing checkout
# of that repository instead, for offline builds.
if(DEFINED ENV{NOCTURNE_DXC_SOURCE_DIR} AND NOT FETCHCONTENT_SOURCE_DIR_PLUME_DXC)
    set(FETCHCONTENT_SOURCE_DIR_PLUME_DXC "$ENV{NOCTURNE_DXC_SOURCE_DIR}")
endif()
include(${NOCTURNE_PLUME_DIR}/examples/cmake/PlumeShaders.cmake)
plume_shaders_init()

# DXIL is Windows-only; SPIR-V is built everywhere, since Plume reaches Vulkan
# on every platform.
if(WIN32)
    set(NOCTURNE_SHADER_FORMATS "dxil,spirv")
else()
    set(NOCTURNE_SHADER_FORMATS "spirv")
endif()

# Release version the self-updater compares GitHub tags against: an explicit
# override, else the CI tag (NOCTURNE_VERSION_NAME, also what the Android Gradle
# build stamps), else the newest reachable v* tag, else empty, which leaves the
# updater off. Recomputed on every configure so a new tag reaches an old build dir.
# Mods' `game_version >= X.Y.Z` constraints compare against it too; an empty
# version is "can't verify, don't block" (see docs/making-mods.md).
set(NOCTURNE_VERSION_OVERRIDE "" CACHE STRING "Version embedded for the self-updater (vX.Y.Z or X.Y.Z); empty to derive it")
set(NOCTURNE_VERSION "${NOCTURNE_VERSION_OVERRIDE}")
if(NOCTURNE_VERSION STREQUAL "")
    set(NOCTURNE_VERSION "$ENV{NOCTURNE_VERSION_NAME}")
endif()
if(NOCTURNE_VERSION STREQUAL "")
    execute_process(
        COMMAND git describe --tags --abbrev=0 --match "v*"
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE NOCTURNE_VERSION
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
endif()
string(REGEX REPLACE "^v" "" NOCTURNE_VERSION "${NOCTURNE_VERSION}")
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
        ${CMAKE_SOURCE_DIR}/src/fonts/DotGothic16-Subset.ttf
        ${CMAKE_SOURCE_DIR}/src/fonts/Roboto-Medium.ttf
    COMMENT "Generating src/fonts.generated.h"
    VERBATIM
)

# Interface text, one file per language (res/lang/*.toml, the checked-in source
# of truth); src/lang.generated.h is a gitignored build artifact. The files are
# globbed and listed in a file the command depends on, so adding a language
# regenerates the header.
set(NOCTURNE_LANG_HEADER ${CMAKE_SOURCE_DIR}/src/lang.generated.h)
file(GLOB NOCTURNE_LANG_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/res/lang/*.toml)
set(NOCTURNE_LANG_LIST ${CMAKE_BINARY_DIR}/lang_sources.txt)
string(REPLACE ";" "\n" _lang_list "${NOCTURNE_LANG_SOURCES}")
file(CONFIGURE OUTPUT ${NOCTURNE_LANG_LIST} CONTENT "${_lang_list}\n")
add_custom_command(
    OUTPUT ${NOCTURNE_LANG_HEADER}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/gen-lang.py
    DEPENDS
        ${CMAKE_SOURCE_DIR}/scripts/gen-lang.py
        ${NOCTURNE_LANG_LIST}
        ${NOCTURNE_LANG_SOURCES}
    COMMENT "Generating src/lang.generated.h"
    VERBATIM
)

# Supplemental Japanese glyphs: font_ja.xpr is a subset font missing 33 of the
# characters the achievement strings use, and it is the player's own extracted
# copyrighted asset, so we ship our own art for the 27 that need it (the other
# 6 are fullwidth forms aliased onto stock ASCII glyphs) and compose the two in
# memory at load time (see src/font_patch.cpp). The PNGs in
# src/fonts/glyphs_ja/ are the checked-in source of truth;
# src/glyph_pack.generated.h is a gitignored build artifact.
set(NOCTURNE_GLYPHS_HEADER ${CMAKE_SOURCE_DIR}/src/glyph_pack.generated.h)
# CONFIGURE_DEPENDS so dropping a newly drawn glyph into the folder (or taking
# one out) re-runs the generator on the next build instead of needing a manual
# CMake reconfigure.
file(GLOB NOCTURNE_GLYPH_ART CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/fonts/glyphs_ja/*.png)
add_custom_command(
    OUTPUT ${NOCTURNE_GLYPHS_HEADER}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/gen-glyphs.py
    DEPENDS
        ${CMAKE_SOURCE_DIR}/scripts/gen-glyphs.py
        ${CMAKE_SOURCE_DIR}/src/fonts/glyphs_ja/metrics.toml
        ${NOCTURNE_GLYPH_ART}
    COMMENT "Generating src/glyph_pack.generated.h"
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

# The randomizer's code patches compiled into hooks (see src/rando_hooks.h):
# translated from the instructions in assets/default.xex and the core's code
# caves. src/rando_hooks.generated.cpp is a gitignored build artifact; the
# generator also checks that nocturnerecomp_rando_hooks.toml (which codegen
# reads) is current.
set(NOCTURNE_RANDO_HOOKS ${CMAKE_SOURCE_DIR}/src/rando_hooks.generated.cpp)
add_custom_command(
    OUTPUT ${NOCTURNE_RANDO_HOOKS}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/gen-rando-hooks.py
    DEPENDS
        ${CMAKE_SOURCE_DIR}/scripts/gen-rando-hooks.py
        ${CMAKE_SOURCE_DIR}/ext/KryptonRecomp/xex_image.py
        ${CMAKE_SOURCE_DIR}/ext/SOTN_XB_RANDO/Source/xb_rando.cpp
        ${CMAKE_SOURCE_DIR}/nocturnerecomp_rando_hooks.toml
        ${CMAKE_SOURCE_DIR}/assets/default.xex
    COMMENT "Generating src/rando_hooks.generated.cpp"
    VERBATIM
)

# The guest image, linked in so the game is loaded without the game folder's
# default.xex (src/guest_image.h), built from src/guest_data with no code
# sections (scripts/gen-guest-image.py).
option(NOCTURNE_EMBED_GUEST_IMAGE "Link the guest image built from source into the executable" ON)
if(NOCTURNE_EMBED_GUEST_IMAGE)
    set(NOCTURNE_GUEST_IMAGE ${CMAKE_BINARY_DIR}/guest-image.xex)
    file(GLOB NOCTURNE_GUEST_DATA_SOURCES CONFIGURE_DEPENDS
        ${CMAKE_SOURCE_DIR}/src/guest_data/*.txt
        ${CMAKE_SOURCE_DIR}/src/guest_data/*/*.txt)
    # The data grids taken from the game (scripts/guest_data.py), into the
    # gitignored assets/guest_data.
    set(NOCTURNE_GUEST_DATA_STAMP ${CMAKE_BINARY_DIR}/guest-data.stamp)
    add_custom_command(
        OUTPUT ${NOCTURNE_GUEST_DATA_STAMP}
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/guest_data.py
            extract ${CMAKE_SOURCE_DIR}/assets/default.xex
        COMMAND ${CMAKE_COMMAND} -E touch ${NOCTURNE_GUEST_DATA_STAMP}
        DEPENDS
            ${CMAKE_SOURCE_DIR}/scripts/guest_data.py
            ${CMAKE_SOURCE_DIR}/src/guest_data/assets.txt
            ${CMAKE_SOURCE_DIR}/src/guest_data/xdbf.txt
            ${CMAKE_SOURCE_DIR}/assets/default.xex
        COMMENT "Extracting the guest data grids"
        VERBATIM
    )
    add_custom_command(
        OUTPUT ${NOCTURNE_GUEST_IMAGE}
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/gen-guest-image.py
            ${NOCTURNE_GUEST_IMAGE}
        DEPENDS
            ${CMAKE_SOURCE_DIR}/scripts/gen-guest-image.py
            ${CMAKE_SOURCE_DIR}/scripts/guest_data.py
            ${CMAKE_SOURCE_DIR}/scripts/xex_header.py
            ${NOCTURNE_GUEST_DATA_SOURCES}
            ${CMAKE_SOURCE_DIR}/ext/KryptonRecomp/xex_image.py
            ${NOCTURNE_GUEST_DATA_STAMP}
        COMMENT "Building the guest image from source"
        VERBATIM
    )
    set(NOCTURNE_GUEST_IMAGE_ASM ${CMAKE_BINARY_DIR}/guest-image.S)
    file(CONFIGURE OUTPUT ${NOCTURNE_GUEST_IMAGE_ASM} CONTENT [=[
#if defined(__APPLE__)
#define SYM(name) _##name
.section __TEXT,__const
#elif defined(_WIN32)
#define SYM(name) name
.section .rdata,"dr"
#else
#define SYM(name) name
.section .rodata
#endif

.p2align 4
.global SYM(kGuestImageData)
SYM(kGuestImageData):
.incbin "@NOCTURNE_GUEST_IMAGE@"
.global SYM(kGuestImageDataEnd)
SYM(kGuestImageDataEnd):
]=] @ONLY)
    set_source_files_properties(${NOCTURNE_GUEST_IMAGE_ASM}
        PROPERTIES OBJECT_DEPENDS ${NOCTURNE_GUEST_IMAGE})
endif()

# The guest shader pack: translate the shader files in assets/MEDIA with
# KryptonRecomp, compile them with DXC, and pack the results. The generator also
# writes a `.S` that `.incbin`s the pack into read-only data, so there is no
# loose file to deploy (see src/gpu/guest_shaders.h).
set(NOCTURNE_GUEST_SHADER_DIR ${CMAKE_BINARY_DIR}/guest_shaders)
set(NOCTURNE_GUEST_SHADER_PACK ${CMAKE_BINARY_DIR}/guest_shaders.bin)
set(NOCTURNE_GUEST_SHADER_PACK_ASM ${CMAKE_BINARY_DIR}/guest_shaders.bin.S)
if(NOT CMAKE_ASM_COMPILER)
    set(CMAKE_ASM_COMPILER ${CMAKE_CXX_COMPILER})
endif()
enable_language(ASM)
if(PLUME_DXC_LIB_DIR)
    set(NOCTURNE_DXC_LIB_ARG --dxc-lib-dir ${PLUME_DXC_LIB_DIR})
else()
    set(NOCTURNE_DXC_LIB_ARG "")
endif()
# The game library keeps each version in its own folder (assets/xbla-world);
# CI and older checkouts have a flat assets/MEDIA.
set(NOCTURNE_GUEST_MEDIA_DIR ${CMAKE_SOURCE_DIR}/assets/MEDIA)
if(NOT EXISTS ${NOCTURNE_GUEST_MEDIA_DIR} AND EXISTS ${CMAKE_SOURCE_DIR}/assets/xbla-world/MEDIA)
    set(NOCTURNE_GUEST_MEDIA_DIR ${CMAKE_SOURCE_DIR}/assets/xbla-world/MEDIA)
endif()
file(GLOB NOCTURNE_GUEST_SHADER_FILES CONFIGURE_DEPENDS
    ${NOCTURNE_GUEST_MEDIA_DIR}/*.xvu ${NOCTURNE_GUEST_MEDIA_DIR}/*.xpu)
add_custom_command(
    OUTPUT ${NOCTURNE_GUEST_SHADER_PACK} ${NOCTURNE_GUEST_SHADER_PACK_ASM}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/gen-guest-shaders.py
        --media ${NOCTURNE_GUEST_MEDIA_DIR}
        --out ${NOCTURNE_GUEST_SHADER_DIR}
        --pack ${NOCTURNE_GUEST_SHADER_PACK}
        --pack-asm ${NOCTURNE_GUEST_SHADER_PACK_ASM}
        --dxc ${PLUME_DXC_EXECUTABLE}
        ${NOCTURNE_DXC_LIB_ARG}
        --formats ${NOCTURNE_SHADER_FORMATS}
    DEPENDS
        ${CMAKE_SOURCE_DIR}/scripts/gen-guest-shaders.py
        ${CMAKE_SOURCE_DIR}/ext/KryptonRecomp/shader_build.py
        ${CMAKE_SOURCE_DIR}/ext/KryptonRecomp/shader_extract.py
        ${CMAKE_SOURCE_DIR}/ext/KryptonRecomp/xenos_hlsl.py
        ${CMAKE_SOURCE_DIR}/ext/KryptonRecomp/xenos_ucode.py
        ${NOCTURNE_GUEST_SHADER_FILES}
    COMMENT "Translating and compiling the guest's shaders"
    VERBATIM
)

# Start screen art: every res/*.png, embedded by file stem (see
# scripts/gen-images.py). None ships yet; dropping res/background.png or
# res/logo.png in is picked up on the next configure.
set(NOCTURNE_IMAGES_HEADER ${CMAKE_SOURCE_DIR}/src/images.generated.h)
file(GLOB NOCTURNE_IMAGE_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/res/*.png)
set(NOCTURNE_IMAGE_LIST ${CMAKE_BINARY_DIR}/image_sources.txt)
string(REPLACE ";" "
" _image_list "${NOCTURNE_IMAGE_SOURCES}")
file(CONFIGURE OUTPUT ${NOCTURNE_IMAGE_LIST} CONTENT "${_image_list}
")
add_custom_command(
    OUTPUT ${NOCTURNE_IMAGES_HEADER}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/gen-images.py
    DEPENDS
        ${CMAKE_SOURCE_DIR}/scripts/gen-images.py
        ${NOCTURNE_IMAGE_LIST}
        ${NOCTURNE_IMAGE_SOURCES}
    COMMENT "Generating src/images.generated.h"
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
    src/tu_hooks.cpp
    src/xact_compat.cpp
    src/accent_color.cpp
    src/achievement_icons.cpp
    src/achievements_menu.cpp
    src/achievements_screen.cpp
    src/fast_forward.cpp
    src/font_overlay.cpp
    src/font_patch.cpp
    src/graphics_settings.cpp
    src/host_timer_resolution.cpp
    src/intro_skips.cpp
    src/jp_artwork.cpp
    src/jp_voices.cpp
    src/language_switch.cpp
    src/movie_player.cpp
    src/help_options_menu.cpp
    src/native_enhancements.cpp
    src/native_options.cpp
    src/native_ui.cpp
    src/player_status.cpp
    src/enhancements.cpp
    src/rando_menu.cpp
    src/rando_runtime.cpp
    src/rando_hooks.generated.cpp
    src/rando/rando_core.cpp
    src/rando/rando_host.cpp
    src/room_presence.cpp
    src/settings.cpp
    src/target.cpp
    src/installer/game_data.cpp
    src/installer/game_library.cpp
    src/installer/install_pipeline.cpp
    src/installer/intro_music.cpp
    src/installer/intro_screen.cpp
    src/installer/intro_text.cpp
    src/installer/mods_screen.cpp
    src/installer/release_id.cpp
    src/installer/save_slots.cpp
    src/installer/saves_screen.cpp
    src/installer/xbla_package.cpp
    src/platform/document_picker.cpp
    src/platform/host_menu.cpp
    src/platform/touch_controls_overlay.cpp
    src/platform/touch_input_driver.cpp
    src/ui_text.cpp
    src/video_mode.cpp
    src/xbox_live.cpp
    src/gpu/guest_shaders.cpp
    src/gpu/native_renderer.cpp
    src/gpu/native_renderer_d3d.cpp
    src/gpu/native_renderer_draw.cpp
    src/gpu/native_renderer_overlay.cpp
    src/gpu/native_renderer_plume.cpp
    src/gpu/native_renderer_tessellation.cpp
    src/gpu/native_renderer_texture.cpp
    ${NOCTURNE_FONTS_HEADER}
    ${NOCTURNE_GLYPHS_HEADER}
    ${NOCTURNE_LANG_HEADER}
    ${NOCTURNE_ICON_HEADER}
    ${NOCTURNE_IMAGES_HEADER}
    ${CMAKE_SOURCE_DIR}/ext/KryptonRecomp/guest_shader_pack.cpp
    ${NOCTURNE_GUEST_SHADER_PACK_ASM}
)
if(ANDROID)
    list(APPEND NOCTURNERECOMP_SOURCES src/platform/android_gpu_driver.cpp)
endif()
if(NOCTURNE_EMBED_GUEST_IMAGE)
    list(APPEND NOCTURNERECOMP_SOURCES src/guest_image.cpp ${NOCTURNE_GUEST_IMAGE_ASM})
endif()

if(WIN32)
    add_executable(nocturnerecomp WIN32 ${NOCTURNERECOMP_SOURCES} ${NOCTURNE_ICON_RC})
elseif(ANDROID)
    # A shared library SDLActivity loads. Everything linked into it has to be
    # position independent, including the OBJECT library rexglue_setup_target()
    # creates for the recompiled code, so this must be set before that call.
    add_library(nocturnerecomp SHARED ${NOCTURNERECOMP_SOURCES})
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    # Plume hides symbols by default; SDLActivity needs to dlsym SDL_main.
    set_target_properties(nocturnerecomp PROPERTIES
        CXX_VISIBILITY_PRESET default
        C_VISIBILITY_PRESET default
    )
    # System.loadLibrary opens the library RTLD_LOCAL, and bionic's
    # dlsym(RTLD_DEFAULT) skips local libraries, hiding the mod API from mods.
    # DF_1_GLOBAL makes the linker treat it as RTLD_GLOBAL.
    target_link_options(nocturnerecomp PRIVATE "LINKER:-z,global")
else()
    add_executable(nocturnerecomp ${NOCTURNERECOMP_SOURCES})
    # ELF executables do not export their own symbols, so without ENABLE_EXPORTS
    # the mod API is invisible to native mods' dlsym(RTLD_DEFAULT).
    set_target_properties(nocturnerecomp PROPERTIES
        BUILD_RPATH "$ORIGIN"
        INSTALL_RPATH "$ORIGIN"
        ENABLE_EXPORTS ON
    )
endif()

rexglue_setup_target(nocturnerecomp)
if(NOCTURNE_EMBED_GUEST_IMAGE)
    target_compile_definitions(nocturnerecomp PRIVATE NOCTURNE_EMBED_GUEST_IMAGE=1)
endif()

target_link_libraries(nocturnerecomp PRIVATE plume)

# libadrenotools loads a user supplied Vulkan driver (Turnip) in place of the
# stock one. Its hook libraries are dlopen'd by name from nativeLibraryDir, so
# they are built next to libnocturnerecomp.so for staging into jniLibs.
if(ANDROID)
    add_subdirectory(ext/libadrenotools EXCLUDE_FROM_ALL)
    set_target_properties(main_hook file_redirect_hook gsl_alloc_hook hook_impl PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
    target_link_libraries(nocturnerecomp PRIVATE adrenotools)
    add_dependencies(nocturnerecomp main_hook file_redirect_hook gsl_alloc_hook hook_impl)
endif()
target_include_directories(nocturnerecomp PRIVATE src src/gpu src/installer src/platform ext/KryptonRecomp)

# MottZilla's SOTN_XB_RANDO core (see src/rando/rando_core.cpp). It is a Win32
# program's source: off Windows, src/rando/compat supplies the two headers
# (windows.h, process.h) it includes. Its code is compiled as shipped, so its
# warnings are not ours to fix.
set(NOCTURNE_RANDO_DIR "${CMAKE_SOURCE_DIR}/ext/SOTN_XB_RANDO/Source")
if(NOT EXISTS "${NOCTURNE_RANDO_DIR}/xb_rando.cpp")
    message(FATAL_ERROR
        "SOTN_XB_RANDO submodule is missing. Run: git submodule update --init --recursive")
endif()
target_include_directories(nocturnerecomp PRIVATE "${NOCTURNE_RANDO_DIR}")
if(NOT WIN32)
    target_include_directories(nocturnerecomp PRIVATE src/rando/compat)
endif()
set_source_files_properties(src/rando/rando_core.cpp PROPERTIES COMPILE_OPTIONS "-w")
# The `.S` only names the pack; the pack itself must exist before it assembles.
set_source_files_properties(${NOCTURNE_GUEST_SHADER_PACK_ASM} PROPERTIES
    OBJECT_DEPENDS ${NOCTURNE_GUEST_SHADER_PACK})
if(WIN32)
    # Plume's CMake does not link the D3D12 import library itself, leaving
    # D3D12CreateDevice and D3D12SerializeRootSignature to the consumer.
    target_link_libraries(nocturnerecomp PRIVATE d3d12)
endif()

# Overlay shaders. Plume compiles nothing at runtime, so these are built ahead
# of time to DXIL and SPIR-V and embedded as byte arrays.
plume_compile_vertex_shader(nocturnerecomp
    "${CMAKE_SOURCE_DIR}/src/shaders/imgui.vert.hlsl" "imguiVert" "VSMain")
plume_compile_pixel_shader(nocturnerecomp
    "${CMAKE_SOURCE_DIR}/src/shaders/imgui.frag.hlsl" "imguiFrag" "PSMain")

# The present blit, which scales the guest's image into the window.
plume_compile_vertex_shader(nocturnerecomp
    "${CMAKE_SOURCE_DIR}/src/shaders/blit.vert.hlsl" "blitVert" "VSMain")
plume_compile_pixel_shader(nocturnerecomp
    "${CMAKE_SOURCE_DIR}/src/shaders/blit.frag.hlsl" "blitFrag" "PSMain")
