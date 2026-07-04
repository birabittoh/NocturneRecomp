#!/usr/bin/env python3
"""Builds every code mod under mods_src/ and assembles it into mods/<name>/.

Each mods_src/<name>/ directory is a standalone CMake project (its own
CMakeLists.txt including mods_src/common/mod_cmake/rexmod.cmake) that links
against the SDK's rex::runtime, found via CMAKE_PREFIX_PATH pointing at an
SDK directory. The resulting binary is copied to mods/<name>/code/<name>.dll
(or lib<name>.so) alongside mod.toml and icon.png, matching the layout the
runtime's mod loader expects (see rex::system::LoadModPlugin).

By default every mod is built for *both* windows and linux (--target to
narrow it down), so mods/<name>/code/ ends up with both a .dll and a .so
regardless of what machine ran this script. For whichever target matches
the host OS, the build happens directly with the local clang++. For the
other one, the script falls back to Docker:
  - linux target on a non-Linux host: a plain Linux container (clang++).
  - windows target on a non-Windows host: a Linux container cross-compiling
    MSVC-ABI binaries via clang-cl + xwin (see
    scripts/docker/windows-mod-build.Dockerfile), unless clang-cl and an
    xwin sysroot are already present locally, in which case that's used
    directly instead of spinning up Docker.

Each target's SDK lives in its own subdirectory (<sdk-root>/win-amd64,
<sdk-root>/linux-amd64), fetched on demand inside the relevant Docker
container via scripts/download-sdk.py --pinned --platform ...

After building, each mod's `platform` key in mod.toml is (re)written to
record which platform(s) mods/<name>/code/ actually ships a binary for right
now (e.g. "windows-x64,linux-x64", or just "windows-x64" after a
--target windows-only run) -- this key is script-managed, not something a
mod author sets by hand.
"""
import argparse
import os
import platform
import re
import shutil
import subprocess
import sys


PLATFORM_INFO = {
    "windows": {"ext": ".dll", "prefix": "", "sdk_subdir": "win-amd64", "manifest_key": "windows-x64"},
    "linux": {"ext": ".so", "prefix": "lib", "sdk_subdir": "linux-amd64", "manifest_key": "linux-x64"},
}

LINUX_DOCKER_IMAGE = "nocturnerecomp-linux-mod-builder"
WINDOWS_CROSS_DOCKER_IMAGE = "nocturnerecomp-windows-cross-mod-builder"
XWIN_CACHE_DIR = "/opt/xwin-cache"


def host_plat():
    system = platform.system()
    if system == "Windows":
        return "windows"
    if system == "Linux":
        return "linux"
    return None  # macOS or anything else: no target can be built natively


def can_cross_build_windows_locally():
    """True if clang-cl + an xwin sysroot are already on this machine (either
    because it's our windows-mod-build.Dockerfile image, or because someone
    set up the same tools by hand outside Docker)."""
    return (
        host_plat() != "windows"
        and shutil.which("clang-cl") is not None
        and os.path.isdir(XWIN_CACHE_DIR)
    )


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


def find_built_binary(build_dir, name, plat):
    """CMake single-config generators (Ninja) drop the binary straight in
    build_dir; look there first, then fall back to a per-config subdir in
    case a multi-config generator is used instead."""
    info = PLATFORM_INFO[plat]
    candidates = [os.path.join(build_dir, f"{info['prefix']}{name}{info['ext']}")]
    for config in ("Release", "RelWithDebInfo", "Debug"):
        candidates.append(os.path.join(build_dir, config, f"{info['prefix']}{name}{info['ext']}"))
    for path in candidates:
        if os.path.isfile(path):
            return path
    return None


def built_platforms_for_mod(root, name):
    """Which platform(s) mods/<name>/code/ currently ships a binary for,
    based on what's actually on disk -- not just what this run built, so a
    windows-only run doesn't erase the record of a .so from an earlier run."""
    code_dir = os.path.join(root, "mods", name, "code")
    built = []
    for plat, info in PLATFORM_INFO.items():
        binary = os.path.join(code_dir, f"{info['prefix']}{name}{info['ext']}")
        if os.path.isfile(binary):
            built.append(info["manifest_key"])
    return built


def update_mod_platform_field(mods_src_dir, name, manifest_keys):
    """Writes the built platform list back into mods_src/<name>/mod.toml's
    `platform` key (comma-separated, e.g. "windows-x64,linux-x64"), so the
    manifest always reflects what mods/<name>/code/ actually ships -- this is
    the one field make_mods.py manages itself rather than the mod author."""
    manifest_path = os.path.join(mods_src_dir, name, "mod.toml")
    if not os.path.isfile(manifest_path):
        return

    value = ",".join(manifest_keys)
    with open(manifest_path, "r", encoding="utf-8") as f:
        lines = f.readlines()

    pattern = re.compile(r'^\s*platform\s*=')
    new_line = f'platform = "{value}"\n'
    replaced = False
    for i, line in enumerate(lines):
        if pattern.match(line):
            lines[i] = new_line
            replaced = True
            break
    if not replaced:
        lines.append(new_line)

    with open(manifest_path, "w", encoding="utf-8") as f:
        f.writelines(lines)


def is_valid_sdk(sdk_dir):
    return os.path.exists(os.path.join(sdk_dir, "lib", "cmake", "rexglue"))


def resolve_native_sdk_dir(sdk_root, sdk_dir, plat):
    """Native (host-matching) builds also accept a flat SDK directly at
    sdk_root -- e.g. one deployed by ../rexglue-sdk/scripts/deploy-sdk.py,
    which always writes straight to <project>/sdk with no per-platform
    subdir -- so an existing local dev setup doesn't need a redundant
    re-download just to pick up the platform subdir convention used for
    the Docker cross-build targets."""
    if is_valid_sdk(sdk_dir):
        return sdk_dir
    if is_valid_sdk(sdk_root):
        return sdk_root
    return sdk_dir


def require_sdk(sdk_dir, plat):
    if not is_valid_sdk(sdk_dir):
        print(f"error: {plat} SDK not found at '{sdk_dir}' — run "
              f"'python scripts/download-sdk.py {sdk_dir} --pinned --platform "
              f"{PLATFORM_INFO[plat]['sdk_subdir']}' first", file=sys.stderr)
        sys.exit(1)


def build_mod_native(mod_src_dir, name, sdk_dir, cxx_compiler, root, plat):
    build_dir = os.path.join(root, "out", "build", "mods", plat, name)
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

    binary = find_built_binary(build_dir, name, plat)
    if not binary:
        print(f"error: couldn't find built binary for mod '{name}' under {build_dir}",
              file=sys.stderr)
        sys.exit(1)
    return binary


def build_mod_windows_cross(mod_src_dir, name, sdk_dir, root):
    build_dir = os.path.join(root, "out", "build", "mods", "windows-cross", name)
    os.makedirs(build_dir, exist_ok=True)
    toolchain_file = os.path.join(root, "scripts", "docker", "windows-msvc-cross-toolchain.cmake")

    configure_args = [
        "cmake",
        "-S", mod_src_dir,
        "-B", build_dir,
        "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE=Release",
        f"-DCMAKE_TOOLCHAIN_FILE={toolchain_file}",
        f"-DCMAKE_PREFIX_PATH={os.path.abspath(sdk_dir)}",
    ]
    run(configure_args)
    run(["cmake", "--build", build_dir, "--parallel", str(os.cpu_count() or 1)])

    binary = find_built_binary(build_dir, name, "windows")
    if not binary:
        print(f"error: couldn't find built binary for mod '{name}' under {build_dir}",
              file=sys.stderr)
        sys.exit(1)
    return binary


def assemble_mod(mod_src_dir, name, binary_path, plat, root):
    info = PLATFORM_INFO[plat]
    dest_dir = os.path.join(root, "mods", name)
    code_dir = os.path.join(dest_dir, "code")
    os.makedirs(code_dir, exist_ok=True)

    dest_binary = os.path.join(code_dir, f"{info['prefix']}{name}{info['ext']}")
    print(f"+ cp {binary_path} {dest_binary}")
    shutil.copy2(binary_path, dest_binary)

    for extra in ("mod.toml", "icon.png"):
        src = os.path.join(mod_src_dir, extra)
        if os.path.isfile(src):
            dest = os.path.join(dest_dir, extra)
            print(f"+ cp {src} {dest}")
            shutil.copy2(src, dest)


def build_targets_via_docker(plat, names, root, sdk_dir):
    """Builds `names` for `plat` inside the matching Docker image, then
    re-invokes this same script inside the container -- which will either
    take the native path (linux target: the container IS linux) or the
    local cross-compile path (windows target: the image ships clang-cl +
    an xwin sysroot), so the actual build logic lives in one place."""
    if shutil.which("docker") is None:
        print(f"error: can't build '{plat}' natively on this host and no `docker` found in "
              f"PATH to cross-build it instead", file=sys.stderr)
        sys.exit(1)

    if plat == "linux":
        dockerfile = os.path.join(root, "scripts", "docker", "linux-mod-build.Dockerfile")
        image = LINUX_DOCKER_IMAGE
    else:
        dockerfile = os.path.join(root, "scripts", "docker", "windows-mod-build.Dockerfile")
        image = WINDOWS_CROSS_DOCKER_IMAGE

    run(["docker", "build", "-t", image, "-f", dockerfile, root])

    mod_args = []
    for name in names:
        mod_args += ["--mod", name]
    sdk_subdir = PLATFORM_INFO[plat]["sdk_subdir"]
    inner_cmd = (
        f"python3 scripts/download-sdk.py {sdk_dir} --pinned --platform {sdk_subdir} && "
        f"python3 scripts/make_mods.py --target {plat} --sdk-dir {sdk_dir} " + " ".join(mod_args)
    )
    run([
        "docker", "run", "--rm",
        "-v", f"{root}:/workspace",
        "-w", "/workspace",
        image,
        "bash", "-c", inner_cmd,
    ])


def package_mod(name, root):
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
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--sdk-dir", default="sdk",
                        help="SDK root; each platform's SDK is expected at "
                             "<sdk-dir>/win-amd64 or <sdk-dir>/linux-amd64 (default: sdk)")
    parser.add_argument("--mod", action="append", metavar="NAME",
                        help="Only build this mod (repeatable); default: all of mods_src/*")
    parser.add_argument("--target", action="append", dest="targets", choices=["windows", "linux"],
                        metavar="{windows,linux}",
                        help="Only build for this platform (repeatable); default: both")
    parser.add_argument("--package", action="store_true",
                        help="Also zip each built mod to mods/<name>.zip")
    args = parser.parse_args()

    script_dir = os.path.dirname(os.path.abspath(__file__))
    root = os.path.normpath(os.path.join(script_dir, ".."))
    os.chdir(root)

    mods_src_dir = os.path.join(root, "mods_src")
    if not os.path.isdir(mods_src_dir):
        print(f"error: {mods_src_dir} not found", file=sys.stderr)
        sys.exit(1)

    all_mods = sorted(
        name for name in os.listdir(mods_src_dir)
        if name != "common" and os.path.isfile(os.path.join(mods_src_dir, name, "CMakeLists.txt"))
    )
    mod_names = args.mod or all_mods
    unknown = [name for name in mod_names if name not in all_mods]
    if unknown:
        print(f"error: unknown mod(s) {unknown}; available: {all_mods}", file=sys.stderr)
        sys.exit(1)

    platforms = args.targets or ["windows", "linux"]
    host = host_plat()

    for plat in platforms:
        print(f"\n=== Platform: {plat} ===")

        # Forward slashes always: this path is also embedded into a bash -c
        # string run inside a Linux Docker container when cross-building.
        sdk_root = args.sdk_dir.replace("\\", "/").rstrip("/")
        sdk_dir = f"{sdk_root}/{PLATFORM_INFO[plat]['sdk_subdir']}"

        if plat == host:
            sdk_dir = resolve_native_sdk_dir(sdk_root, sdk_dir, plat)
            require_sdk(sdk_dir, plat)
            cxx_compiler = find_clangxx()
            for name in mod_names:
                mod_src_dir = os.path.join(mods_src_dir, name)
                print(f"\n--- Building mod '{name}' natively for {plat} ---")
                binary = build_mod_native(mod_src_dir, name, sdk_dir, cxx_compiler, root, plat)
                assemble_mod(mod_src_dir, name, binary, plat, root)

        elif plat == "windows" and can_cross_build_windows_locally():
            require_sdk(sdk_dir, plat)
            for name in mod_names:
                mod_src_dir = os.path.join(mods_src_dir, name)
                print(f"\n--- Cross-building mod '{name}' for windows (clang-cl + xwin) ---")
                binary = build_mod_windows_cross(mod_src_dir, name, sdk_dir, root)
                assemble_mod(mod_src_dir, name, binary, plat, root)

        else:
            print(f"host can't build '{plat}' directly -- falling back to Docker")
            build_targets_via_docker(plat, mod_names, root, sdk_dir)

    # Record which platform(s) each mod actually ships a binary for, then
    # re-copy the updated manifest over the one already assembled above.
    for name in mod_names:
        mod_src_dir = os.path.join(mods_src_dir, name)
        built = built_platforms_for_mod(root, name)
        update_mod_platform_field(mods_src_dir, name, built)
        manifest_src = os.path.join(mod_src_dir, "mod.toml")
        if os.path.isfile(manifest_src):
            shutil.copy2(manifest_src, os.path.join(root, "mods", name, "mod.toml"))

    if args.package:
        for name in mod_names:
            package_mod(name, root)

    print(f"\nBuilt {len(mod_names)} mod(s) for {', '.join(platforms)}: {', '.join(mod_names)}")


if __name__ == "__main__":
    main()
