#!/usr/bin/env python3
import hashlib
import os
import sys
import glob
import platform
import shutil
import subprocess
import tomllib


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


def check_deps():
    missing = [dep for dep in ("cmake", "ninja") if shutil.which(dep) is None]
    if missing:
        print(f"error: missing required tool(s): {', '.join(missing)}", file=sys.stderr)
        sys.exit(1)


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


def run(args, check=True, **kwargs):
    print(f"+ {' '.join(str(a) for a in args)}")
    result = subprocess.run(args, **kwargs)
    if result.returncode != 0 and check:
        sys.exit(result.returncode)
    return result


def load_manifest(path):
    with open(path, "rb") as f:
        return tomllib.load(f)


def compute_codegen_hash(manifest, manifest_path):
    """Hash every input that determines codegen output."""
    h = hashlib.sha256()

    # 1. XEX binary
    xex_path = manifest["entrypoint"]["file_path"]
    with open(xex_path, "rb") as f:
        while chunk := f.read(1 << 20):
            h.update(chunk)

    # 2. Include files (e.g. nocturnerecomp_config.toml)
    for inc in manifest["entrypoint"].get("includes", []):
        with open(inc, "rb") as f:
            h.update(f.read())

    # 3. Manifest itself, with sdk_version normalized out (codegen re-stamps it,
    #    build.py blanks it again — either way the hash must be stable).
    with open(manifest_path, "r") as f:
        for line in f:
            if not line.startswith("sdk_version"):
                h.update(line.encode())

    # 4. SDK identity
    sdk_version_file = os.path.join(os.path.dirname(manifest_path), ".sdk-version")
    if os.path.exists(sdk_version_file):
        with open(sdk_version_file, "rb") as f:
            h.update(f.read())

    return h.hexdigest()


def copy_runtime_libs(is_windows, sdk_dir):
    src_dir = os.path.join(sdk_dir, "bin" if is_windows else "lib")
    suffix = ".dll" if is_windows else ".so"

    if not os.path.isdir(src_dir):
        return
    for name in os.listdir(src_dir):
        if name.endswith(suffix):
            src = os.path.join(src_dir, name)
            print(f"+ cp {src} {name}")
            shutil.copy2(src, name)


def do_package(name, project_name, is_windows):
    import zipfile
    import tarfile

    pkg_dir = "pkg"
    os.makedirs(pkg_dir, exist_ok=True)

    exe = f"{project_name}.exe" if is_windows else project_name
    lib_suffix = ".dll" if is_windows else ".so"
    candidates = [exe] + sorted(f for f in os.listdir(".") if f.endswith(lib_suffix))
    for src in candidates:
        if os.path.isfile(src):
            print(f"+ cp {src} {pkg_dir}/")
            shutil.copy2(src, pkg_dir)

    if is_windows:
        archive_path = f"{name}.zip"
        print(f"+ zip {archive_path}")
        with zipfile.ZipFile(archive_path, "w", zipfile.ZIP_DEFLATED) as zf:
            for f in sorted(os.listdir(pkg_dir)):
                zf.write(os.path.join(pkg_dir, f), f)
    else:
        archive_path = f"{name}.tar.gz"
        print(f"+ tar {archive_path}")
        with tarfile.open(archive_path, "w:gz") as tf:
            for f in sorted(os.listdir(pkg_dir)):
                tf.add(os.path.join(pkg_dir, f), arcname=f)

    github_env = os.environ.get("GITHUB_ENV")
    if github_env:
        with open(github_env, "a") as fh:
            fh.write(f"ARTIFACT_PATH={archive_path}\n")
            fh.write(f"ARTIFACT_NAME={name}\n")


def parse_args():
    import argparse
    p = argparse.ArgumentParser()
    p.add_argument("--sdk-dir", default="sdk", help="Path to the ReXGlue SDK (default: sdk)")
    p.add_argument("--package", metavar="NAME", help="Package built output into NAME.zip (Windows) or NAME.tar.gz (Linux); skips the build")
    p.add_argument("--debug", action="store_true", help="Build with debug symbols (uses the debug CMake preset)")
    p.add_argument("--force-codegen", action="store_true", help="Force codegen even if inputs are unchanged")
    p.add_argument("--strict-codegen", action="store_true", help="Abort the build if codegen returns a non-zero exit code")
    return p.parse_args()


def main():
    args = parse_args()

    script_dir = os.path.dirname(os.path.abspath(__file__))
    root = os.path.normpath(os.path.join(script_dir, ".."))
    os.chdir(root)

    is_windows = platform.system() == "Windows"

    manifests = glob.glob("*_manifest.toml")
    if len(manifests) != 1:
        print(
            f"error: expected exactly one *_manifest.toml in the repo root, "
            f"found: {manifests if manifests else 'none'}",
            file=sys.stderr,
        )
        sys.exit(1)
    manifest_path = manifests[0]
    manifest = load_manifest(manifest_path)
    project_name = manifest["project"]["name"]

    if args.package:
        do_package(args.package, project_name, is_windows)
        return

    sdk_dir = args.sdk_dir
    rexglue = os.path.join(sdk_dir, "bin", "rexglue.exe" if is_windows else "rexglue")

    if not os.path.exists(rexglue):
        print(f"SDK not found at '{sdk_dir}' — downloading pinned version...")
        run([sys.executable, os.path.join(script_dir, "download-sdk.py"), os.path.abspath(sdk_dir), "--pinned"])

    xex_path = manifest["entrypoint"]["file_path"]

    if not os.path.exists(xex_path):
        print(f"error: XEX not found at '{xex_path}' — place the game's default.xex there before building", file=sys.stderr)
        sys.exit(1)

    check_deps()

    preset = detect_preset("relwithdebinfo" if args.debug else "release")
    exe_name = f"{project_name}.exe" if is_windows else project_name
    build_output = os.path.join("out", "build", preset, exe_name)

    cxx_compiler = find_clangxx()

    cmake_configure_args = [
        f"-DCMAKE_PREFIX_PATH={sdk_dir}",
        f"-DCMAKE_CXX_COMPILER={cxx_compiler}",
    ]
    if shutil.which("sccache"):
        cmake_configure_args += [
            "-DCMAKE_CXX_COMPILER_LAUNCHER=sccache",
        ]

    lib_suffix = ".dll" if is_windows else ".so"
    to_remove = [exe_name] + [f for f in os.listdir(".") if f.endswith(lib_suffix)]
    for name in to_remove:
        if os.path.isfile(name):
            print(f"+ rm {name}")
            os.remove(name)

    stamp_path = os.path.join("out", "codegen.stamp")
    new_hash = compute_codegen_hash(manifest, manifest_path)
    generated_present = os.path.exists(os.path.join("generated", "sources.cmake"))
    old_hash = None
    if os.path.exists(stamp_path):
        with open(stamp_path, "r") as f:
            old_hash = f.read().strip()

    if not args.force_codegen and new_hash == old_hash and generated_present:
        print("+ codegen inputs unchanged — skipping codegen (incremental build)")
    else:
        run([rexglue, "codegen", manifest_path], check=args.strict_codegen)

        with open(manifest_path, "r") as f:
            lines = f.readlines()
        with open(manifest_path, "w") as f:
            for line in lines:
                if line.startswith("sdk_version"):
                    f.write('sdk_version = ""\n')
                else:
                    f.write(line)

        os.makedirs("out", exist_ok=True)
        with open(stamp_path, "w") as f:
            f.write(new_hash)

    run(["cmake", "--preset", preset] + cmake_configure_args)
    run(["cmake", "--build", "--preset", preset, "--parallel", str(os.cpu_count() or 1)])

    print(f"+ cp {build_output} {exe_name}")
    shutil.copy2(build_output, exe_name)

    copy_runtime_libs(is_windows, sdk_dir)


if __name__ == "__main__":
    main()
