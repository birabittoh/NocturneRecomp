import os, sys, time
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", ".tools", "renderdoc_runtime"))
import renderdoc as rd

idents = rd.EnumerateRemoteTargets("localhost", 0)
print("idents:", idents)
if not idents:
    sys.exit("no target running")
ident = idents
target = rd.CreateTargetControl("localhost", ident, "trigger_script", True)
if target is None:
    sys.exit("failed to connect")
print("connected, api:", target.GetAPI())
target.TriggerCapture(1)
for i in range(200):
    msg = target.ReceiveMessage(None)
    if msg.type != rd.TargetControlMessageType.Noop:
        print(i, msg.type)
    if msg.type == rd.TargetControlMessageType.NewCapture:
        print("CAPTURE_PATH=" + msg.newCapture.path)
        break
    time.sleep(0.1)
target.Shutdown()
