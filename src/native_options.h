// nocturnerecomp - appends extra rows to the game's own settings screen
// (main menu, or pause -> help & options -> settings), driven by the same
// left/right in-place cycling the stock rows use.
//
// To add a row, append an entry to kCustomRows in native_options.cpp -- see
// the "Adding a row" section at the top of that file. It also documents the
// reverse-engineering the whole thing rests on.
#pragma once

namespace rex {
class Runtime;
}  // namespace rex

namespace nocturne {

class NativeOptions {
 public:
  // Installs the guest function overrides. Call once runtime() is live
  // (OnPostSetup), same as GraphicsSettings::Bind -- every address here is
  // hardcoded against the game's own pinned build.
  void Bind(rex::Runtime* runtime);

 private:
  rex::Runtime* runtime_ = nullptr;
};

// Process-wide instance shared between the app hooks.
NativeOptions& GetNativeOptions();

}  // namespace nocturne
