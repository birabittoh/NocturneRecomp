# Cross-compiles MSVC-ABI Windows binaries from a non-Windows host using
# clang-cl + lld-link against Microsoft's redistributable CRT/SDK headers
# and import libraries, fetched by `xwin` (see windows-mod-build.Dockerfile).
#
# Needed because the SDK's Windows build (and its rex::runtime import lib)
# is produced by plain clang++ targeting x86_64-pc-windows-msvc, so a mod
# must link against the same ABI -- a mingw-w64 cross build would not.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

if(NOT DEFINED XWIN_CACHE)
    set(XWIN_CACHE "/opt/xwin-cache" CACHE PATH "xwin splat output directory")
endif()

set(CMAKE_C_COMPILER clang-cl)
set(CMAKE_CXX_COMPILER clang-cl)
set(CMAKE_LINKER lld-link)
set(CMAKE_AR llvm-lib)

set(CMAKE_C_COMPILER_TARGET x86_64-pc-windows-msvc)
set(CMAKE_CXX_COMPILER_TARGET x86_64-pc-windows-msvc)

set(_xwin_includes
    "/imsvc${XWIN_CACHE}/crt/include"
    "/imsvc${XWIN_CACHE}/sdk/include/ucrt"
    "/imsvc${XWIN_CACHE}/sdk/include/um"
    "/imsvc${XWIN_CACHE}/sdk/include/shared"
)
string(JOIN " " _xwin_includes_str ${_xwin_includes})
set(CMAKE_C_FLAGS_INIT "${_xwin_includes_str}")
set(CMAKE_CXX_FLAGS_INIT "${_xwin_includes_str}")

set(_xwin_libpaths
    "/libpath:${XWIN_CACHE}/crt/lib/x86_64"
    "/libpath:${XWIN_CACHE}/sdk/lib/um/x86_64"
    "/libpath:${XWIN_CACHE}/sdk/lib/ucrt/x86_64"
)
string(JOIN " " _xwin_libpaths_str ${_xwin_libpaths})
set(CMAKE_EXE_LINKER_FLAGS_INIT "${_xwin_libpaths_str}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_xwin_libpaths_str}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${_xwin_libpaths_str}")

# Can't execute Windows binaries on the build host to probe compiler
# features via try_run.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
