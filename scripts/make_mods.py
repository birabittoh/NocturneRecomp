#!/usr/bin/env python3
"""Builds every code mod under mods_src/ and assembles it into mods/<name>/.

Each mods_src/<name>/ directory is a standalone CMake project (its own
CMakeLists.txt including mods_src/common/mod_cmake/rexmod.cmake) that links
against the SDK's rex::runtime, found via CMAKE_PREFIX_PATH pointing at this
project's sdk/ directory. The resulting DLL is copied to
mods/<name>/code/<name>.dll alongside mod.toml and icon.png, matching the
layout the runtime's mod loader expects (see rex::system::LoadModPlugin).

Asset-only mods (mods/hdost, mods/badapple) are untouched -- this script only
ever writes mods/<name>/ for names it finds under mods_src/.
"""
import argparse
import os
import platform
import shutil
import subprocess
import sys


def detect_preset(build_type="release"):
    os_name = platform.system()
    arch = platform.machine().lower()

    if os_name == "Linux":
        os_id = "linux"
    elif os_name == "Windows":
        os_id = "win"
    else:
        raise RuntimeError(f"Unsupported OS: {os_name}")

    if arch in ("x86_64", "amd64"):
        arch_id = "amd64"
    elif arch in ("aarch64", "arm64"):
        arch_id = "arm64"
    else:
        raise RuntimeError(f"Unsupported architecture: {arch}")

    return f"{os_id}-{arch_id}-{build_type}"


def find_clangxx():
    # Versioned binaries (clang++-20, clang++-22, …) only exist on Linux.
    if platform.system() != "Windows":
        for version in range(30, 17, -1):
            if shutil.which(f"clang++-{version}"):
                return f"clang++-{version}"
    if shutil.which("clang++"):
        return "clang++"
    print("error: no clang++ compiler found in PATH", file=sys.stderr)
    sys.exit(1)


def run(args, **kwargs):
    print(f"+ {' '.join(str(a) for a in args)}")
    result = subprocess.run(args, **kwargs)
    if result.returncode != 0:
        sys.exit(result.returncode)
    return result


def find_built_binary(build_dir, name, is_windows):
    """CMake single-config generators (Ninja) drop the binary straight in
    build_dir; look there first, then fall back to a per-config subdir in
    case a multi-config generator is used instead."""
    ext = ".dll" if is_windows else ".so"
    prefix = "" if is_windows else "lib"
    candidates = [os.path.join(build_dir, f"{prefix}{name}{ext}")]
    for config in ("Release", "RelWithDebInfo", "Debug"):
        candidates.append(os.path.join(build_dir, config, f"{prefix}{name}{ext}"))
    for path in candidates:
        if os.path.isfile(path):
            return path
    return None


def build_mod(mod_src_dir, name, sdk_dir, cxx_compiler, is_windows, root):
    build_dir = os.path.join(root, "out", "build", "mods", name)
    os.makedirs(build_dir, exist_ok=True)

    configure_args = [
        "cmake",
        "-S", mod_src_dir,
        "-B", build_dir,
        "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE=Release",
        f"-DCMAKE_PREFIX_PATH={os.path.abspath(sdk_dir)}",
        f"-DCMAKE_CXX_COMPILER={cxx_compiler}",
    ]
    run(configure_args)
    run(["cmake", "--build", build_dir, "--parallel", str(os.cpu_count() or 1)])

    binary = find_built_binary(build_dir, name, is_windows)
    if not binary:
        print(f"error: couldn't find built binary for mod '{name}' under {build_dir}",
              file=sys.stderr)
        sys.exit(1)
    return binary


def assemble_mod(mod_src_dir, name, binary_path, is_windows, root):
    dest_dir = os.path.join(root, "mods", name)
    code_dir = os.path.join(dest_dir, "code")
    os.makedirs(code_dir, exist_ok=True)

    ext = ".dll" if is_windows else ".so"
    prefix = "" if is_windows else "lib"
    dest_binary = os.path.join(code_dir, f"{prefix}{name}{ext}")
    print(f"+ cp {binary_path} {dest_binary}")
    shutil.copy2(binary_path, dest_binary)

    for extra in ("mod.toml", "icon.png"):
        src = os.path.join(mod_src_dir, extra)
        if os.path.isfile(src):
            dest = os.path.join(dest_dir, extra)
            print(f"+ cp {src} {dest}")
            shutil.copy2(src, dest)


def package_mod(name, root, is_windows):
    import zipfile

    mod_dir = os.path.join(root, "mods", name)
    archive_path = os.path.join(root, "mods", f"{name}.zip")
    print(f"+ zip {archive_path}")
    with zipfile.ZipFile(archive_path, "w", zipfile.ZIP_DEFLATED) as zf:
        for dirpath, _dirnames, filenames in os.walk(mod_dir):
            for fname in filenames:
                full = os.path.join(dirpath, fname)
                arcname = os.path.join(name, os.path.relpath(full, mod_dir))
                zf.write(full, arcname)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk-dir", default="sdk", help="Path to the ReXGlue SDK (default: sdk)")
    parser.add_argument("--mod", action="append", metavar="NAME",
                        help="Only build this mod (repeatable); default: all of mods_src/*")
    parser.add_argument("--package", action="store_true",
                        help="Also zip each built mod to mods/<name>.zip")
    args = parser.parse_args()

    script_dir = os.path.dirname(os.path.abspath(__file__))
    root = os.path.normpath(os.path.join(script_dir, ".."))
    os.chdir(root)

    is_windows = platform.system() == "Windows"
    mods_src_dir = os.path.join(root, "mods_src")

    if not os.path.isdir(mods_src_dir):
        print(f"error: {mods_src_dir} not found", file=sys.stderr)
        sys.exit(1)

    all_mods = sorted(
        name for name in os.listdir(mods_src_dir)
        if name != "common" and os.path.isfile(os.path.join(mods_src_dir, name, "CMakeLists.txt"))
    )
    targets = args.mod or all_mods
    unknown = [name for name in targets if name not in all_mods]
    if unknown:
        print(f"error: unknown mod(s) {unknown}; available: {all_mods}", file=sys.stderr)
        sys.exit(1)

    if not os.path.exists(os.path.join(args.sdk_dir, "lib", "cmake", "rexglue")):
        print(f"error: SDK not found at '{args.sdk_dir}' — run "
              f"'python scripts/download-sdk.py' or deploy-sdk.py first", file=sys.stderr)
        sys.exit(1)

    cxx_compiler = find_clangxx()

    for name in targets:
        mod_src_dir = os.path.join(mods_src_dir, name)
        print(f"\n=== Building mod '{name}' ===")
        binary = build_mod(mod_src_dir, name, args.sdk_dir, cxx_compiler, is_windows, root)
        assemble_mod(mod_src_dir, name, binary, is_windows, root)
        if args.package:
            package_mod(name, root, is_windows)

    print(f"\nBuilt {len(targets)} mod(s): {', '.join(targets)}")


if __name__ == "__main__":
    main()
