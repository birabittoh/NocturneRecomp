#!/usr/bin/env python3
"""Packages a built macOS binary as an .app inside a .dmg.

Run after `build.py --release`, which leaves the executable and the SDK's
dylibs in the repo root. This only assembles.

Ships the Vulkan loader and MoltenVK, since macOS has neither; see
BootstrapAppleVulkanRuntime in native_renderer_plume.cpp for the runtime half.
Everything goes in Contents/MacOS rather than Contents/Frameworks, because the
dylibs' @executable_path rpaths already resolve there.

Ad-hoc signed at the end because arm64 will not run unsigned binaries and
copying a dylib invalidates its signature. Not notarised, so a first launch
needs right-click -> Open.
"""
import argparse
import glob
import importlib.util
import json
import os
import plistlib
import shutil
import subprocess
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

BUNDLE_ID = "com.birabittoh.nocturnerecomp"
DISPLAY_NAME = "NocturneRecomp"
# Matches CMAKE_OSX_DEPLOYMENT_TARGET in the mac presets.
MINIMUM_SYSTEM_VERSION = "13.3"

# Searched in order: the Homebrew prefixes CI installs, then VULKAN_SDK.
VULKAN_LOADER_NAMES = ["libvulkan.1.dylib", "libvulkan.dylib"]
MOLTENVK_NAME = "libMoltenVK.dylib"
ICD_RELATIVE_PATHS = [
    os.path.join("share", "vulkan", "icd.d", "MoltenVK_icd.json"),
    os.path.join("etc", "vulkan", "icd.d", "MoltenVK_icd.json"),
]


def run(cmd, **kwargs):
    print("+ " + " ".join(cmd))
    subprocess.run(cmd, check=True, **kwargs)


def brew_prefix(formula):
    try:
        out = subprocess.run(["brew", "--prefix", formula], check=True,
                             capture_output=True, text=True)
        return out.stdout.strip()
    except (subprocess.CalledProcessError, FileNotFoundError):
        return None


def vulkan_search_roots():
    # The SDK's CMake stages a pinned loader, MoltenVK and manifest into
    # <build dir>/vulkan, which build.py copies to the repo root. Preferred over
    # Homebrew: it is the runtime the SDK was actually built against.
    roots = [os.path.join(ROOT, "vulkan")]
    for formula in ("vulkan-loader", "molten-vk"):
        prefix = brew_prefix(formula)
        if prefix:
            roots.append(prefix)
    sdk = os.environ.get("VULKAN_SDK")
    if sdk:
        roots.append(sdk)
        roots.append(os.path.join(sdk, "macOS"))
    roots += ["/opt/homebrew", "/usr/local"]
    return [r for r in roots if os.path.isdir(r)]


def find_in_roots(roots, relative_paths):
    for root in roots:
        for relative in relative_paths:
            candidate = os.path.join(root, relative)
            if os.path.exists(candidate):
                return candidate
    return None


def stage_vulkan_runtime(macos_dir, resources_dir):
    """Copies the loader, the driver and a rewritten manifest into the bundle.

    Warns rather than fails when it is missing: the bundle still runs on a
    machine that has Vulkan installed.
    """
    roots = vulkan_search_roots()

    loader = find_in_roots(roots, [os.path.join("lib", n) for n in VULKAN_LOADER_NAMES])
    driver = find_in_roots(roots, [os.path.join("lib", MOLTENVK_NAME)])
    icd = find_in_roots(roots, ICD_RELATIVE_PATHS)

    if not loader or not driver:
        print(f"warning: no Vulkan runtime found (loader={loader}, driver={driver}); "
              f"the .app will need one installed on the target machine",
              file=sys.stderr)
        return False

    # copy2 follows symlinks, so brew's versioned target lands as one real file
    # under the name the loader is dlopen'd by.
    print(f"+ cp {loader} {macos_dir}/libvulkan.1.dylib")
    shutil.copy2(loader, os.path.join(macos_dir, "libvulkan.1.dylib"))
    print(f"+ cp {driver} {macos_dir}/{MOLTENVK_NAME}")
    shutil.copy2(driver, os.path.join(macos_dir, MOLTENVK_NAME))

    # The manifest goes in Resources, not next to the driver: codesign --deep
    # reads any directory under Contents/MacOS as a nested code component and
    # rejects the bundle over it. A relative library_path resolves against the
    # manifest's own directory. The rest is copied from the installed manifest
    # so api_version is not guessed.
    manifest = {"file_format_version": "1.0.0", "ICD": {"api_version": "1.2.0"}}
    if icd:
        try:
            with open(icd, "r", encoding="utf-8") as fh:
                manifest = json.load(fh)
        except (OSError, ValueError) as exc:
            print(f"warning: could not read {icd} ({exc}); writing a default manifest",
                  file=sys.stderr)
    manifest.setdefault("ICD", {})["library_path"] = f"../../../MacOS/{MOLTENVK_NAME}"

    icd_dir = os.path.join(resources_dir, "vulkan", "icd.d")
    os.makedirs(icd_dir, exist_ok=True)
    icd_out = os.path.join(icd_dir, "MoltenVK_icd.json")
    print(f"+ write {icd_out}")
    with open(icd_out, "w", encoding="utf-8") as fh:
        json.dump(manifest, fh, indent=4)
    return True


def stage_icon(resources_dir):
    """Builds AppIcon.icns from the title icon inside default.xex.

    Same source as the window icon, so there is no separate asset. Non-fatal.
    """
    try:
        # Loaded by path because the filename is not a valid module name.
        spec = importlib.util.spec_from_file_location(
            "gen_icon", os.path.join(ROOT, "scripts", "gen-icon.py"))
        gen_icon = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(gen_icon)
        png = gen_icon.extract_icon_png(os.path.join(ROOT, "assets", "default.xex"))
    except Exception as exc:  # noqa: BLE001 - any failure here is non-fatal
        print(f"warning: no icon ({exc}); the bundle will use the generic one",
              file=sys.stderr)
        return None

    iconset = os.path.join(resources_dir, "AppIcon.iconset")
    os.makedirs(iconset, exist_ok=True)
    base = os.path.join(iconset, "base.png")
    with open(base, "wb") as fh:
        fh.write(png)

    # iconutil only accepts these names. Mostly upscales: a soft icon beats a
    # missing size, which makes Finder fall back to the generic one.
    for size in (16, 32, 64, 128, 256, 512, 1024):
        names = []
        if size in (16, 32, 128, 256, 512):
            names.append(f"icon_{size}x{size}.png")
        if size in (32, 64, 256, 512, 1024):
            names.append(f"icon_{size // 2}x{size // 2}@2x.png")
        for name in names:
            out = os.path.join(iconset, name)
            run(["sips", "-z", str(size), str(size), base, "--out", out],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    os.remove(base)
    icns = os.path.join(resources_dir, "AppIcon.icns")
    run(["iconutil", "-c", "icns", iconset, "-o", icns])
    shutil.rmtree(iconset)
    return "AppIcon"


def write_info_plist(contents_dir, executable, icon_name, version):
    info = {
        "CFBundleDevelopmentRegion": "en",
        "CFBundleDisplayName": DISPLAY_NAME,
        "CFBundleExecutable": executable,
        "CFBundleIdentifier": BUNDLE_ID,
        "CFBundleInfoDictionaryVersion": "6.0",
        "CFBundleName": DISPLAY_NAME,
        "CFBundlePackageType": "APPL",
        "CFBundleShortVersionString": version,
        "CFBundleVersion": version,
        "LSApplicationCategoryType": "public.app-category.games",
        "LSMinimumSystemVersion": MINIMUM_SYSTEM_VERSION,
        # Without this the window is upscaled by the window server, which on
        # Retina is a visibly soft 1280x720.
        "NSHighResolutionCapable": True,
    }
    if icon_name:
        info["CFBundleIconFile"] = icon_name

    path = os.path.join(contents_dir, "Info.plist")
    print(f"+ write {path}")
    with open(path, "wb") as fh:
        plistlib.dump(info, fh)


def build_app(app_path, project_name, version):
    if os.path.exists(app_path):
        shutil.rmtree(app_path)

    contents = os.path.join(app_path, "Contents")
    macos_dir = os.path.join(contents, "MacOS")
    resources = os.path.join(contents, "Resources")
    os.makedirs(macos_dir)
    os.makedirs(resources)

    payload = [project_name]
    payload += sorted(f for f in os.listdir(ROOT) if f.endswith(".dylib"))
    for name in payload:
        src = os.path.join(ROOT, name)
        if not os.path.isfile(src):
            print(f"warning: {name} not found next to the build; skipping", file=sys.stderr)
            continue
        print(f"+ cp {name} Contents/MacOS/")
        shutil.copy2(src, os.path.join(macos_dir, name))

    exe = os.path.join(macos_dir, project_name)
    if not os.path.isfile(exe):
        print(f"error: {project_name} not found in {ROOT}; run build.py first", file=sys.stderr)
        sys.exit(1)
    os.chmod(exe, 0o755)

    stage_vulkan_runtime(macos_dir, resources)
    icon_name = stage_icon(resources)
    write_info_plist(contents, project_name, icon_name, version)

    # --deep is deprecated for real identities but right for an ad-hoc pass
    # over copied dylibs. Fatal on failure: on arm64 the result would not run.
    run(["codesign", "--force", "--deep", "--sign", "-", app_path])


def build_dmg(app_path, dmg_path, volume_name):
    staging = os.path.join(ROOT, "pkg-dmg")
    if os.path.exists(staging):
        shutil.rmtree(staging)
    os.makedirs(staging)

    shutil.copytree(app_path, os.path.join(staging, os.path.basename(app_path)),
                    symlinks=True)
    # The drag-to-install affordance, and why a dmg beats a zip: an app dragged
    # out of an image is freed from App Translocation.
    os.symlink("/Applications", os.path.join(staging, "Applications"))

    if os.path.exists(dmg_path):
        os.remove(dmg_path)
    run(["hdiutil", "create", "-volname", volume_name, "-srcfolder", staging,
         "-ov", "-format", "UDZO", dmg_path])
    shutil.rmtree(staging)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--name", required=True,
                        help="Base name for the .dmg, without the extension")
    parser.add_argument("--version", default="1.0.0",
                        help="CFBundleShortVersionString / CFBundleVersion")
    args = parser.parse_args()

    if sys.platform != "darwin":
        print("error: this only runs on macOS (needs codesign, hdiutil, iconutil)",
              file=sys.stderr)
        sys.exit(1)

    os.chdir(ROOT)

    manifests = glob.glob("*_manifest.toml")
    if len(manifests) != 1:
        print(f"error: expected exactly one *_manifest.toml, found {manifests}", file=sys.stderr)
        sys.exit(1)
    # Same minimal read build.py does: the project name is the executable name.
    project_name = None
    with open(manifests[0], "r", encoding="utf-8") as fh:
        in_project = False
        for line in fh:
            line = line.strip()
            if line.startswith("["):
                in_project = line == "[project]"
            elif in_project and line.startswith("name"):
                project_name = line.split("=", 1)[1].strip().strip('"\'')
                break
    if not project_name:
        print(f"error: no [project] name in {manifests[0]}", file=sys.stderr)
        sys.exit(1)

    app_path = os.path.join(ROOT, f"{DISPLAY_NAME.replace(' ', '')}.app")
    build_app(app_path, project_name, args.version)

    # Into pkg/, which is what CI uploads and where build.py packages too.
    pkg_dir = os.path.join(ROOT, "pkg")
    os.makedirs(pkg_dir, exist_ok=True)
    dmg_path = os.path.join(pkg_dir, f"{args.name}.dmg")
    build_dmg(app_path, dmg_path, DISPLAY_NAME)
    print(f"packaged {dmg_path}")

    github_env = os.environ.get("GITHUB_ENV")
    if github_env:
        with open(github_env, "a") as fh:
            fh.write(f"ARTIFACT_PATH={os.path.relpath(dmg_path, ROOT)}\n")
            fh.write(f"ARTIFACT_NAME={args.name}\n")


if __name__ == "__main__":
    main()
