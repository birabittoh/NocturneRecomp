import lldb
import time
import sys

import os

debugger = lldb.SBDebugger.Create()
debugger.SetAsync(False)
target = debugger.CreateTarget(None)
error = lldb.SBError()
pid = int(os.environ["ATTACH_PID"])
process = target.AttachToProcessWithID(debugger.GetListener(), pid, error)
if not error.Success():
    print("ATTACH FAILED:", error)
    sys.exit(1)

print("Attached. Continuing...")
process.Continue()

# Wait/poll for a stop that isn't the initial attach breakpoint.
for i in range(600):
    state = process.GetState()
    if state == lldb.eStateStopped:
        thread = process.GetSelectedThread()
        reason = thread.GetStopReason()
        # Skip the synthetic attach breakpoint / signal stops that aren't crashes.
        stop_desc = thread.GetStopDescription(1024) or ""
        print(f"Stopped: reason={reason} desc={stop_desc}")
        if "breakpoint" not in stop_desc.lower() and reason != lldb.eStopReasonNone:
            print("=== CRASH DETECTED ===")
            for t_idx in range(process.GetNumThreads()):
                t = process.GetThreadAtIndex(t_idx)
                print(f"--- thread {t_idx} tid={t.GetThreadID()} ---")
                for f_idx in range(min(t.GetNumFrames(), 30)):
                    f = t.GetFrameAtIndex(f_idx)
                    print(f"  #{f_idx} {f}")
            break
        process.Continue()
    elif state in (lldb.eStateExited, lldb.eStateDetached, lldb.eStateCrashed):
        print("Process state:", state)
        break
    time.sleep(1)
else:
    print("Timed out waiting for crash")

print("DONE")
