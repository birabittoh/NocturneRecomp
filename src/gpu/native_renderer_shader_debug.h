// nocturnerecomp - ReXGlue Recompiled Project
//
// The native renderer's side of the SDK's shader debugger overlay (F2).
//
// The overlay is written against the emulated-Xenos command processor, which
// tracks shaders by microcode hash because it discovers them at runtime. Here
// the inventory is the closed set of containers in guest_shaders.bin,
// addressed by guest table slot. ReXApp exposes a ShaderDebuggerOverride hook
// for this case: slots are presented as if they were hashes, and the four
// questions the dialog asks are answered: what exists, what is running, what is
// slow, and what should stop drawing.
//
// The identifier handed to the dialog is the guest table slot, with 0x100 set
// for a pixel shader. The type bit stays below bit 32 because the dialog
// derives an ImGui id with `PushID(static_cast<int>(hash))`, which truncates a
// higher bit and would give vs_007 and ps_007 the same id. The identifier is
// stable across runs, so the names and disable flags the dialog persists in
// shaders.toml stay meaningful: a name written against vs_017 still means
// vs_017. The slot is what every other part of this renderer speaks, so a
// number read out of the overlay can be pasted into a log filter.
//
// Disabling is enforced in the draw path (see IssueGuestDraw): a draw whose
// pipeline was built from a disabled vertex or pixel shader is dropped and
// counted like any other drop. The emulator substitutes a no-op translation;
// dropping the draw is what answers "which shader draws this?".
//
// No Plume types appear in this header.

#pragma once

#include <cstdint>
#include <vector>

#include <rex/ui/overlay/shader_debugger_overlay.h>

namespace nocturne {

// The dialog's identifier for a shader, and the way back out of it.
uint64_t GuestShaderDebugId(bool pixel, uint32_t slot);

// --- The draw path's side. Called on the guest thread. ---

// True when either bound shader has been switched off in the overlay. Both
// slots may be -1 (unresolved), which is never disabled.
bool GuestShaderDrawDisabled(int vertex_slot, int pixel_slot);

// Record that a draw went through with these shaders bound. `elapsed_ns` is
// only read while profiling is on; pass 0 otherwise. Marks both shaders active
// for the current frame, which is what the overlay's "only active" filter and
// its green rows are showing.
void NoteGuestShaderDraw(int vertex_slot, int pixel_slot, uint64_t elapsed_ns);

// Whether the overlay has asked for per-shader timing. Checked before taking a
// clock reading, so the draw path pays nothing while the debugger is closed.
bool GuestShaderProfilingEnabled();

// Guest frame boundary: rolls the "active this frame" flags over. Called from
// the Swap hook, so a shader that stops being bound goes inactive within a
// frame rather than staying lit for the rest of the run.
void GuestShaderDebugEndFrame();

// --- The overlay's side. Called on the UI thread. ---

std::vector<rex::ui::ShaderDebuggerEntry> GuestShaderSnapshot();
rex::ui::ShaderDebuggerDetails GuestShaderDetails(uint64_t id);
// False when the identifier is not one this renderer issued, which shaders.toml
// can carry: the file is shared with the emulated-Xenos backend, whose
// identifiers are real microcode hashes.
bool SetGuestShaderDisabled(uint64_t id, bool disabled);
void SetGuestShaderProfiling(bool enabled);
void ResetGuestShaderProfiling();

// Applies a set of identifiers read out of shaders.toml before the overlay has
// ever been opened, so a shader switched off in a previous session stays off
// from the first frame. See ShaderDebuggerDialog::ReadShaderBlacklistFromToml.
void SetGuestShaderBlacklist(const std::vector<uint64_t>& ids);

}  // namespace nocturne
