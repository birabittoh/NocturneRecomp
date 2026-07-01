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

    # 1b. Title-update delta patch, if staged. Codegen analyses base+patch, so the
    #     patch is a codegen input; including it makes the stamp flip between
    #     vanilla and --tu builds and re-runs codegen when the TU changes.
    xexp_path = xex_path + "p"
    if os.path.exists(xexp_path):
        with open(xexp_path, "rb") as f:
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


def copy_runtime_libs(is_windows, sdk_dir, build_type):
    # The SDK ships all build variants of each shared lib side by side
    # (e.g. rexruntime.dll, rexruntimed.dll, rexruntimerd.dll for release,
    # debug, and relwithdebinfo respectively) — only copy the one matching
    # the variant we just built, identified by its filename suffix.
    variant_suffix = {"release": "", "debug": "d", "relwithdebinfo": "rd"}[build_type]

    src_dir = os.path.join(sdk_dir, "bin" if is_windows else "lib")
    ext = ".dll" if is_windows else ".so"

    if not os.path.isdir(src_dir):
        return
    for name in os.listdir(src_dir):
        if not name.endswith(ext):
            continue
        stem = name[: -len(ext)]
        stem_suffix = "rd" if stem.endswith("rd") else "d" if stem.endswith("d") else ""
        if stem_suffix != variant_suffix:
            continue
        src = os.path.join(src_dir, name)
        print(f"+ cp {src} {name}")
        shutil.copy2(src, name)


def write_launcher(template_name, dest_path, exe):
    script_dir = os.path.dirname(os.path.abspath(__file__))
    template_path = os.path.join(script_dir, "launch_templates", template_name)
    with open(template_path, "r", newline="") as f:
        content = f.read().replace("{{EXE}}", exe)
    newline = "\r\n" if template_name.endswith(".bat") else "\n"
    with open(dest_path, "w", newline=newline) as f:
        f.write(content)


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

    # Static options mirror scripts/run.py's fixed CLI flags. game_data_root is
    # deliberately not set here: the config file overrides CLI args (see the
    # CVar precedence rules), which would defeat the launcher script's detection
    # of the assets/update/mods directories below.
    config_path = os.path.join(pkg_dir, f"{project_name}.toml")
    print(f"+ write {config_path}")
    with open(config_path, "w") as f:
        f.write('gpu_plugin = "xenos"\n')
        f.write("license_mask = 1\n")

    if is_windows:
        launcher_path = os.path.join(pkg_dir, "run.bat")
        print(f"+ write {launcher_path}")
        write_launcher("run.bat", launcher_path, exe)
    else:
        launcher_path = os.path.join(pkg_dir, "run.sh")
        print(f"+ write {launcher_path}")
        write_launcher("run.sh", launcher_path, exe)
        os.chmod(launcher_path, 0o755)

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
    p.add_argument(
        "--tu",
        nargs="+",
        metavar="PACKAGE",
        help="Build with a title update. Pass the TU package(s) (LIVE/CON/PIRS) or a "
        "directory containing them; the variant matching your base XEX is selected by "
        "digest, staged as a sibling assets/default.xexp, and baked in by codegen.",
    )
    return p.parse_args()


def stage_title_update(tu_args, xex_path):
    """Select the TU variant matching xex_path and stage it as the sibling '<xex>p'.

    Also extracts the TU's data files (the replacement audio track) into update/,
    which run.py mounts as the update: device. Returns the target version label.
    Exits on no match.
    """
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import extract_tu

    packages = []
    for arg in tu_args:
        if os.path.isdir(arg):
            packages += sorted(glob.glob(os.path.join(arg, "TU_*")))
        else:
            packages.append(arg)
    if not packages:
        print(f"error: no TU packages found in {tu_args}", file=sys.stderr)
        sys.exit(1)

    pkg, xexp, version = extract_tu.select_matching(packages, xex_path)
    if not pkg:
        print(
            f"error: none of the given TU packages match the base XEX "
            f"({extract_tu.base_signature(xex_path)}). They are for other game revisions.",
            file=sys.stderr,
        )
        sys.exit(1)

    staged = xex_path + "p"  # the loader (codegen + runtime) applies the sibling '<name>p'
    with open(staged, "wb") as f:
        f.write(xexp)
    print(f"+ title update: {os.path.basename(pkg)} -> v{version}, staged {staged}")
    n = extract_tu.extract_update_tree(pkg, "update")
    print(f"+ title update: extracted {n} data file(s) -> update/ (mounted as update:)")
    return version


def derive_tu_manifest(manifest_path, base_config, tu_config):
    """Write a throwaway manifest that points codegen at the standalone TU config.

    The vanilla manifest/config target the unpatched image; the TU config is a
    standalone set of hints for the patched image. Returns the temp manifest path.
    """
    manifest_text = open(manifest_path).read().replace(
        f'"{base_config}"', f'"{tu_config}"'
    )
    tu_manifest = ".tu_build.toml"  # deliberately not '*_manifest.toml'
    with open(tu_manifest, "w") as f:
        f.write(manifest_text)
    return tu_manifest


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

    # Title update: stage the matching delta patch as a sibling assets/default.xexp
    # (codegen + runtime both apply it automatically) and switch codegen to the
    # standalone TU config. A non-TU build clears any stale patch so it reverts to
    # the vanilla image.
    base_config = manifest["entrypoint"]["includes"][0]
    tu_version = None
    sibling_patch = xex_path + "p"
    codegen_manifest = manifest_path
    if args.tu:
        tu_version = stage_title_update(args.tu, xex_path)
        tu_config = "nocturnerecomp_tu_config.toml"
        if not os.path.exists(tu_config):
            print(f"error: {tu_config} not found (needed for --tu codegen)", file=sys.stderr)
            sys.exit(1)
        codegen_manifest = derive_tu_manifest(manifest_path, base_config, tu_config)
    elif os.path.exists(sibling_patch):
        print(f"+ rm {sibling_patch} (not a TU build)")
        os.remove(sibling_patch)

    check_deps()

    build_type = "relwithdebinfo" if args.debug else "release"
    preset = detect_preset(build_type)
    exe_name = f"{project_name}.exe" if is_windows else project_name
    build_output = os.path.join("out", "build", preset, exe_name)

    cxx_compiler = find_clangxx()

    cmake_configure_args = [
        f"-DCMAKE_PREFIX_PATH={sdk_dir}",
        f"-DCMAKE_CXX_COMPILER={cxx_compiler}",
        # Always set explicitly so switching between TU and vanilla builds doesn't
        # inherit a stale value from the CMake cache.
        f"-DNOCTURNE_TU={'ON' if args.tu else 'OFF'}",
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
    new_hash = compute_codegen_hash(load_manifest(codegen_manifest), codegen_manifest)
    generated_present = os.path.exists(os.path.join("generated", "sources.cmake"))
    old_hash = None
    if os.path.exists(stamp_path):
        with open(stamp_path, "r") as f:
            old_hash = f.read().strip()

    if not args.force_codegen and new_hash == old_hash and generated_present:
        print("+ codegen inputs unchanged — skipping codegen (incremental build)")
    else:
        run([rexglue, "codegen", codegen_manifest], check=args.strict_codegen)

        # codegen re-stamps sdk_version into the manifest it was given; blank it
        # back out in the working tree (the .tu_build.toml is a throwaway anyway).
        with open(codegen_manifest, "r") as f:
            lines = f.readlines()
        with open(codegen_manifest, "w") as f:
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

    copy_runtime_libs(is_windows, sdk_dir, build_type)

    if tu_version:
        print(
            f"\nBuilt with title update v{tu_version}. The matching patch is staged at "
            f"'{sibling_patch}'\nand is required at runtime — the loader re-applies it to "
            f"the base image on launch. Run with scripts/run.py."
        )


if __name__ == "__main__":
    main()
