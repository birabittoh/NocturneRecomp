// nocturnerecomp - serve the composed Japanese font to the guest without
// touching the player's own file.
//
// The guest opens game:\MEDIA\font_ja.xpr, which resolves through the mounted
// game device to the player's extracted copy. InstallJapaneseFontOverlay reads
// that file, composes it with our supplemental glyph pack (font_patch.cpp),
// parks the result in memory behind a one-file VFS device, and registers a
// symbolic link so the guest's open lands on the composed image instead.
//
// With JP installed over World, the font composed is JP's, and strings_ja.bin
// is served next to it: World's ids with JP's wording (the two files are a
// pair, see MergeJapaneseStrings).
//
// The link is registered on the *fully resolved* device path rather than the
// game: alias. VirtualFileSystem::ResolveSymbolicLink loops until no prefix
// matches, so a guest open of game:\MEDIA\font_ja.xpr first rewrites through
// the game: alias and then through ours; an open of the device path hits ours
// directly. Registering on game: instead would race the alias in the symlink
// map, which is unordered.
//
// Failure is always non-fatal: if the player's font is missing or unparseable,
// or nothing in the pack has art yet, no link is registered and the guest
// reads its own file exactly as before.
#pragma once

namespace rex {
class Runtime;
}  // namespace rex

namespace nocturne {

// Composes and installs the overlay. Safe to call once, early in setup and in
// any case before the title first opens its font. Returns the number of glyphs
// added, or 0 if the overlay was not installed.
unsigned InstallJapaneseFontOverlay(rex::Runtime* runtime);

}  // namespace nocturne
