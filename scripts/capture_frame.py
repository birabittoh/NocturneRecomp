"""Launch nocturnerecomp.exe under RenderDoc, trigger a capture a few seconds in, save the .rdc, then kill it.

Usage: python scripts/capture_frame.py
Requires ../.tools/renderdoc_runtime (renderdoc.pyd + renderdoc.dll) on sys.path.
"""
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", ".tools", "renderdoc_runtime"))
import renderdoc as rd

exe = os.path.abspath("out/build/win-amd64-release/nocturnerecomp.exe")
workdir = os.path.abspath(".")
args = "--game_data_root assets --license_mask=1 --fullscreen=false"
capfile = os.path.abspath("logs/capture")

opts = rd.CaptureOptions()
opts.allowVSync = True
opts.apiValidation = False

print("launching under renderdoc...")
result = rd.ExecuteAndInject(exe, workdir, args, [], capfile, opts, False)
print("ident:", result.ident, "result:", result.result)
ident = result.ident

# connect target control
target = None
for attempt in range(50):
    target = rd.CreateTargetControl("localhost", ident, "capture_frame_script", True)
    if target is not None:
        break
    time.sleep(0.2)

if target is None:
    print("FAILED to connect target control")
    sys.exit(1)

print("connected to target, waiting for app to render a few frames...")
time.sleep(6.0)

print("triggering capture...")
target.TriggerCapture(1)

# pump messages so the capture-saved notification arrives
saved_path = None
for _ in range(100):
    msg = target.ReceiveMessage(None)
    if msg.type == rd.TargetControlMessageType.NewCapture:
        saved_path = msg.newCapture.path
        print("new capture saved at:", saved_path)
        break
    time.sleep(0.1)

target.Shutdown()

if saved_path:
    print("CAPTURE_PATH=" + saved_path)
else:
    print("No capture message received")
