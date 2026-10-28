#!/usr/bin/env python3
"""Checks res/lang/*.toml: key parity with English, "{}" parity, keys the code
uses that English lacks, and keys nothing uses.

A language may leave a whole table out (it then reads English there); inside a table it has, every English key must be there,
except the ones in OPTIONAL, which only exist to override one language."""
import re
import sys
import tomllib
from pathlib import Path

root = Path(__file__).resolve().parent.parent
langs = {p.stem: tomllib.loads(p.read_text(encoding="utf8")) for p in sorted((root / "res/lang").glob("*.toml"))}
en = langs["en"]
problems = 0

# Keys that exist in some languages only: the stock game text is right in the others.
OPTIONAL = {("native", "graphics_label")}


def report(msg):
    global problems
    problems += 1
    print(msg)


def flat(table):
    return {(sec, k): v for sec, body in table.items() if isinstance(body, dict) for k, v in body.items()}


en_flat = flat(en)
for code, table in langs.items():
    if code == "en":
        continue
    mine = flat(table)
    sections = {sec for sec, _ in mine}
    for key in sorted(en_flat.keys() - mine.keys()):
        if key[0] in sections:
            report(f"{code}: missing [{key[0]}] {key[1]}")
    for key in sorted(mine.keys() - en_flat.keys() - OPTIONAL):
        report(f"{code}: not in English [{key[0]}] {key[1]}")
    for key in sorted(en_flat.keys() & mine.keys()):
        if en_flat[key].count("{}") != mine[key].count("{}"):
            report(f"{code}: [{key[0]}] {key[1]} has a different number of {{}}")
        if not mine[key].strip():
            report(f"{code}: [{key[0]}] {key[1]} is empty")
    if "name" not in table:
        report(f"{code}: no name")

# Keys the code asks for.
used = set()
sources = [p for p in (root / "src").rglob("*") if p.suffix in (".cpp", ".h")]
pat_tr = re.compile(r'\bTr(?:Or|Current|Args)?\(\s*"(\w+)"\s*,\s*"(\w+)"')
# A key picked by a condition: Tr("section", cond ? "a" : "b", ...).
pat_tr_choice = re.compile(r'\bTr(?:Or|Current|Args)?\(\s*"(\w+)"\s*,[^,"?]*\?\s*"(\w+)"\s*:\s*"(\w+)"')
pat_native = re.compile(r'\bTrNative(?:Current)?\(\s*"(\w+)"')
# native_ui.h's NativeText, as `NativeText name{"key"}` or `NativeText{"key"}`.
pat_native_text = re.compile(r'\bNativeText(?:\s+\w+(?:\[[^\]]*\])?)?\s*\{\s*"(\w+)"')
for path in sources:
    text = path.read_text(encoding="utf8", errors="replace")
    for sec, key in pat_tr.findall(text):
        used.add((sec, key))
    for sec, a, b in pat_tr_choice.findall(text):
        used |= {(sec, a), (sec, b)}
    for key in pat_native.findall(text) + pat_native_text.findall(text):
        used.add(("native", key))

# Keys composed at run time.
# settings.cpp's ConfirmRestart takes its text keys as defaulted parameters.
settings_src = (root / "src/settings.cpp").read_text(encoding="utf8")
for key in re.findall(r'\b\w+_key\s*=\s*"(\w+)"', settings_src):
    used.add(("settings", key))
for args in re.findall(r'\bConfirmRestart\(\s*"\w+"\s*,\s*\w+\s*,([^)]*)\)', settings_src):
    used |= {("settings", key) for key in re.findall(r'"(\w+)"', args)}
# The start screen: intro_text.cpp's key tables, TrErr, and each page's own
# lookup helper (saves_screen.cpp's S/Fill, mods_screen.cpp's T/Fmt).
installer = root / "src/installer"
intro_text = (installer / "intro_text.cpp").read_text(encoding="utf8")
for table in re.findall(r'k(?:Keys|PhaseKeys|ProgressKeys)\s*=\s*\{(.*?)\};', intro_text, re.S):
    used |= {("intro", key) for key in re.findall(r'"(\w+)"', table)}
for path in installer.glob("*.cpp"):
    text = path.read_text(encoding="utf8")
    used |= {("intro", key) for key in re.findall(r'\bTrErr\(\s*"(\w+)"', text)}
    used |= {("sdk_ui", key) for key in re.findall(r'\bSdkUiText\(\s*"(\w+)"', text)}
saves_src = (installer / "saves_screen.cpp").read_text(encoding="utf8")
used |= {("saves", key) for key in re.findall(r'\b(?:S|Fill)\(\s*"(\w+)"', saves_src)}
mods_src = (installer / "mods_screen.cpp").read_text(encoding="utf8")
used |= {("sdk_ui", key) for key in re.findall(r'\b(?:T|Fmt)\(\s*"(\w+)"', mods_src)}
# The SDK's own overlays read [sdk_ui] too (SdkUiText), by UiText("key", ...).
# Those keys are optional: the SDK has its own English for the ones we leave
# out. Its source is the sibling checkout; without it, the table can't be
# checked for unused keys.
sdk_used = set()
sdk_src = root.parent / "rexglue-sdk" / "src"
if sdk_src.is_dir():
    for path in sdk_src.rglob("*.cpp"):
        text = path.read_text(encoding="utf8", errors="replace")
        sdk_used |= {("sdk_ui", key) for key in re.findall(r'\bUiText\(\s*"(\w+)"', text)}
else:
    print(f"note: {sdk_src} not found; [sdk_ui] keys only the SDK reads aren't checked")
    sdk_used = {("sdk_ui", key) for key in en.get("sdk_ui", {})}
enh = (root / "src/enhancements.cpp").read_text(encoding="utf8")
for cvar in re.findall(r'\{Enhancement::\w+,\s*"(enh_\w+)"', enh):
    used |= {("enhancements", cvar), ("enhancements", cvar + "_desc")}
for key in re.findall(r'"(section_\w+)"', enh):
    used.add(("enhancements", key))
# The overlay's enhancement tabs, labelled by section key (settings.cpp).
settings_src = (root / "src/settings.cpp").read_text(encoding="utf8")
for table in re.findall(r'kEnhancementTabs\s*=\s*\{(.*?)\};', settings_src, re.S):
    used |= {("enhancements", key) for key in re.findall(r'"(section_\w+)"', table)}
# The enhancements page's on/off rows, labelled by their cvar's name.
native_enh = (root / "src/native_enhancements.cpp").read_text(encoding="utf8")
for cvar in re.findall(r'\bBoolRow\(\s*\d+\s*,\s*"(\w+)"', native_enh):
    used.add(("native", cvar))
for prefix, extra in (("mode_", "_desc"), ("color_", "")):
    for key in en["enhancements"]:
        if re.fullmatch(prefix + r"\d+(_desc)?", key):
            used.add(("enhancements", key))

for key in sorted(used - en_flat.keys() - OPTIONAL):
    report(f"en: used but missing [{key[0]}] {key[1]}")
for key in sorted(en_flat.keys() - used - sdk_used):
    report(f"en: unused [{key[0]}] {key[1]}")

print(f"{len(langs)} languages, {len(en_flat)} English keys, {problems} problem(s)")
sys.exit(1 if problems else 0)
