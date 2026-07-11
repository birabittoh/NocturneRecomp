"""Launch nocturnerecomp.exe under RenderDoc and grab one frame capture, unattended.

Working recipe (found the hard way -- see docs/native-renderer-headless-boot.md
step 9): launching/injecting must go through the *officially installed*
renderdoccmd.exe (scoop's C:\\Users\\<user>\\scoop\\apps\\renderdoc\\current), not
the separately-self-built renderdoc.pyd module -- only the installed copy is
registered as a Vulkan implicit layer (Settings > enable Vulkan hooking in
qrenderdoc, a one-time persistent machine setting, must be turned on first).
The self-built renderdoc.pyd (../.tools/renderdoc_runtime) is still useful and
works fine, but only for the target-control connect/trigger step below, and
for opening/analyzing the resulting .rdc via the renderdoc-mcp server.

Usage: python scripts/capture_frame.py
"""
import os
import re
import subprocess
import sys
import time

RENDERDOCCMD = r"C:\Users\m.andronaco\scoop\apps\renderdoc\current\renderdoccmd.exe"

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", ".tools", "renderdoc_runtime"))
import renderdoc as rd

repo_root = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
exe = os.path.join(repo_root, "nocturnerecomp.exe")
capfile = os.path.join(repo_root, "logs", "capture")

print("launching under renderdoccmd...")
proc = subprocess.Popen(
    [RENDERDOCCMD, "capture", "-d", repo_root, "-c", capfile, exe,
     "--game_data_root", "assets", "--license_mask=1", "--fullscreen=false"],
    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)

ident = None
# renderdoccmd prints "Launched as ID <n>" once the child process exists.
deadline = time.time() + 15
while time.time() < deadline:
    line = proc.stdout.readline()
    if not line:
        continue
    print(line, end="")
    m = re.search(r"Launched as ID (\d+)", line)
    if m:
        ident = int(m.group(1))
        break

if ident is None:
    sys.exit("FAILED: never saw 'Launched as ID' from renderdoccmd")

print(f"ident={ident}, waiting ~45s for the game to reach the intro draws "
      "(it takes a while to boot -- don't reduce this)...")
time.sleep(45.0)

target = None
for _ in range(50):
    target = rd.CreateTargetControl("localhost", ident, "capture_frame_script", True)
    if target is not None:
        break
    time.sleep(0.2)
if target is None:
    sys.exit("FAILED to connect target control")

print("triggering capture...")
target.TriggerCapture(1)

saved_path = None
for _ in range(100):
    msg = target.ReceiveMessage(None)
    if msg.type == rd.TargetControlMessageType.NewCapture:
        saved_path = msg.newCapture.path
        break
    time.sleep(0.1)
target.Shutdown()

if saved_path:
    print("CAPTURE_PATH=" + saved_path)
else:
    print("No capture message received -- check that qrenderdoc's Vulkan "
          "hooking setting is enabled (Settings, persists across runs once set).")

proc.terminate()
