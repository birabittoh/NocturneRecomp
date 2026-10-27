#!/usr/bin/env python3
"""Generate Android adaptive-icon PNGs from the game's XEX title icon.

Reuses the icon extraction from gen-icon.py, then resizes the PNG into the
standard Android mipmap densities. The output goes into the android/ source
tree so Gradle picks them up at APK build time.

Requires Pillow (pip install Pillow). If Pillow is not available, copies the
raw PNG as a single xxxhdpi icon and skips resizing.
"""
import os
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
sys.path.insert(0, os.path.join(ROOT, "scripts"))

import importlib
gen_icon = importlib.import_module("gen-icon")
extract_icon_png = gen_icon.extract_icon_png

XEX_PATH = os.path.join(ROOT, "assets", "default.xex")
ANDROID_RES = os.path.join(ROOT, "android", "app", "src", "main", "res")

# Android adaptive icon foreground sizes per density bucket.
# The foreground layer is 108dp; these are the px sizes at each density.
DENSITIES = {
    "mipmap-mdpi":    (48, 48),
    "mipmap-hdpi":    (72, 72),
    "mipmap-xhdpi":   (96, 96),
    "mipmap-xxhdpi":  (144, 144),
    "mipmap-xxxhdpi": (192, 192),
}


def main():
    if not os.path.exists(XEX_PATH):
        print(f"Skipping Android icon generation: {XEX_PATH} not found")
        return

    png_data = extract_icon_png(XEX_PATH)

    try:
        from PIL import Image
        import io

        img = Image.open(io.BytesIO(png_data)).convert("RGBA")
        for bucket, (w, h) in DENSITIES.items():
            out_dir = os.path.join(ANDROID_RES, bucket)
            os.makedirs(out_dir, exist_ok=True)
            resized = img.resize((w, h), Image.LANCZOS)
            out_path = os.path.join(out_dir, "ic_launcher.png")
            resized.save(out_path, "PNG")
            print(f"+ wrote {out_path} ({w}x{h})")

        # Also save the foreground layer for adaptive icons.
        for bucket, (w, h) in DENSITIES.items():
            out_dir = os.path.join(ANDROID_RES, bucket)
            # Adaptive icon foreground is 108dp with 18dp padding on each side,
            # so the visible area is 72dp centered. Scale icon to 2/3 of the
            # full layer size and center it on a transparent background.
            fg_size = (int(w * 108 / 48), int(h * 108 / 48))
            icon_size = (int(fg_size[0] * 66 / 108), int(fg_size[1] * 66 / 108))
            fg = Image.new("RGBA", fg_size, (0, 0, 0, 0))
            icon_resized = img.resize(icon_size, Image.LANCZOS)
            paste_x = (fg_size[0] - icon_size[0]) // 2
            paste_y = (fg_size[1] - icon_size[1]) // 2
            fg.paste(icon_resized, (paste_x, paste_y))
            out_path = os.path.join(out_dir, "ic_launcher_foreground.png")
            fg.save(out_path, "PNG")
            print(f"+ wrote {out_path} ({fg_size[0]}x{fg_size[1]})")

    except ImportError:
        # No Pillow — just drop the raw PNG as xxxhdpi.
        out_dir = os.path.join(ANDROID_RES, "mipmap-xxxhdpi")
        os.makedirs(out_dir, exist_ok=True)
        out_path = os.path.join(out_dir, "ic_launcher.png")
        with open(out_path, "wb") as f:
            f.write(png_data)
        print(f"+ wrote {out_path} (raw, no resize — install Pillow for all densities)")


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        print(f"error: {exc}", file=sys.stderr)
        sys.exit(1)
