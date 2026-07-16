#!/usr/bin/env python3
import os
import sys
import glob
import platform
import subprocess
import tomllib


def load_manifest(path):
    with open(path, "rb") as f:
        return tomllib.load(f)


def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    root = os.path.normpath(os.path.join(script_dir, ".."))

    is_windows = platform.system() == "Windows"

    manifests = glob.glob(os.path.join(root, "*_manifest.toml"))
    if len(manifests) != 1:
        print(
            f"error: expected exactly one *_manifest.toml in the repo root, "
            f"found: {manifests if manifests else 'none'}",
            file=sys.stderr,
        )
        sys.exit(1)
    manifest = load_manifest(manifests[0])
    project_name = manifest["project"]["name"]

    exe_name = f"{project_name}.exe" if is_windows else project_name
    exe_path = os.path.join(root, exe_name)

    if not os.path.exists(exe_path):
        print(f"error: '{exe_name}' not found — run scripts/build.py first", file=sys.stderr)
        sys.exit(1)

    env = os.environ.copy()

    if platform.system() == "Linux":
        existing = env.get("LD_LIBRARY_PATH", "")
        env["LD_LIBRARY_PATH"] = root + (":" + existing if existing else "")
        env.setdefault("__NV_PRIME_RENDER_OFFLOAD", "1")
        env.setdefault("__VK_LAYER_NV_optimus", "NVIDIA_only")

    assets_path = os.path.join(root, "assets")
    cmd = [exe_path, "--game_data_root", assets_path, "--license_mask=1"]

    # Title-update builds need the TU's data files (the replacement audio track)
    # mounted as the update: device. scripts/build.py --tu extracts them to update/;
    # mount it when present (harmless for vanilla — it won't exist).
    update_path = os.path.join(root, "update")
    if os.path.isdir(update_path):
        cmd += ["--update_data_root", update_path]

    # Mod overlay: pass the mods directory so the runtime can layer enabled mods
    # over game data. Which mods are active is controlled by enabled_mods in the
    # project's .toml config (not a CLI flag).
    mods_path = os.path.join(root, "mods")
    if os.path.isdir(mods_path):
        cmd += ["--mods_data_root", mods_path]

    cmd += sys.argv[1:]
    sys.exit(subprocess.run(cmd, env=env).returncode)


if __name__ == "__main__":
    main()
