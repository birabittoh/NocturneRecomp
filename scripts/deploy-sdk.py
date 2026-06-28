#!/usr/bin/env python3
import os
import sys
import shutil
import platform
import subprocess
import argparse


def detect_platform_id():
    os_name = platform.system()
    arch = platform.machine().lower()

    if os_name == "Linux":
        if arch in ("x86_64", "amd64"):
            return "linux-amd64"
        elif arch in ("aarch64", "arm64"):
            return "linux-arm64"
        else:
            raise RuntimeError(f"Unsupported architecture: {arch}")
    elif os_name == "Windows":
        if arch in ("x86_64", "amd64"):
            return "win-amd64"
        else:
            raise RuntimeError(f"Unsupported architecture: {arch}")
    else:
        raise RuntimeError(f"Unsupported OS: {os_name}")


def run(args, **kwargs):
    print(f"+ {' '.join(str(a) for a in args)}")
    result = subprocess.run(args, **kwargs)
    if result.returncode != 0:
        sys.exit(result.returncode)


def main():
    parser = argparse.ArgumentParser(
        description="Build the rexglue-sdk submodule and install it into the sdk directory"
    )
    parser.add_argument(
        "out_dir",
        nargs="?",
        default="sdk",
        help="Output directory (default: sdk)",
    )
    parser.add_argument(
        "--config",
        default="Release",
        choices=["Release", "RelWithDebInfo", "Debug"],
        help="CMake build configuration (default: Release)",
    )
    args = parser.parse_args()

    script_dir = os.path.dirname(os.path.abspath(__file__))
    root = os.path.normpath(os.path.join(script_dir, ".."))
    submodule_dir = os.path.join(root, "rexglue-sdk")
    out_dir = os.path.join(root, args.out_dir)

    if not os.path.isdir(submodule_dir) or not os.listdir(submodule_dir):
        print("Initializing rexglue-sdk submodule...")
        run(["git", "submodule", "update", "--init", "--recursive", "rexglue-sdk"], cwd=root)

    platform_id = detect_platform_id()
    config_lower = args.config.lower()
    if config_lower == "relwithdebinfo":
        config_lower = "relwithdebinfo"
    build_preset = f"{platform_id}-{config_lower}"
    jobs = str(os.cpu_count() or 1)

    print(f"Building rexglue-sdk ({platform_id}, {args.config})...")
    run(["cmake", "--preset", platform_id], cwd=submodule_dir)
    run(["cmake", "--build", "--preset", build_preset, "--parallel", jobs], cwd=submodule_dir)

    if os.path.exists(out_dir):
        print(f"Removing existing SDK at {out_dir}...")
        shutil.rmtree(out_dir)

    build_dir = os.path.join(submodule_dir, "out", "build", platform_id)
    run([
        "cmake", "--install", build_dir,
        "--config", args.config,
        "--prefix", out_dir,
    ])

    if platform.system() == "Linux":
        bin_path = os.path.join(out_dir, "bin", "rexglue")
        if os.path.exists(bin_path):
            os.chmod(bin_path, os.stat(bin_path).st_mode | 0o111)
            print(f"Marked {bin_path} executable")

    print(f"SDK installed to {out_dir}")


if __name__ == "__main__":
    main()
