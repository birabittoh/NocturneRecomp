// memory_peek mod - generic, build-agnostic guest-memory hex viewer.
//
// Demonstrates that a mod can read guest memory through the same public API
// the base game's accent-color reader uses (rex::memory::Memory::
// TranslateVirtual + LookupHeap), with zero hardcoded addresses -- the user
// types in whatever guest virtual address they want to inspect. Useful as a
// quick interactive companion to the live-memory-scan RE workflow (see
// scripts/scan_guest_memory.py): once a candidate address is found, F10 lets
// you watch it update in real time without a debugger attached.

#include <rex/system/mod_plugin.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include <imgui.h>

#include <rex/runtime.h>
#include <rex/system/xmemory.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/keybinds.h>

namespace {

constexpr int kBytesPerRow = 16;
constexpr int kRows = 8;

class MemoryPeekDialog : public rex::ui::ImGuiDialog {
 public:
  MemoryPeekDialog(rex::ui::ImGuiDrawer* drawer, rex::Runtime* runtime)
      : ImGuiDialog(drawer), runtime_(runtime) {
    rex::ui::RegisterBind("bind_memory_peek", "F10", "Toggle memory peek overlay",
                          [this] { visible_ = !visible_; });
    std::snprintf(address_buf_, sizeof(address_buf_), "%08X", 0u);
  }

  ~MemoryPeekDialog() override { rex::ui::UnregisterBind("bind_memory_peek"); }

 protected:
  void OnDraw(ImGuiIO& io) override {
    (void)io;
    if (!visible_) {
      return;
    }

    ImGui::SetNextWindowSize(ImVec2(520, 0), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Memory Peek", &visible_)) {
      ImGui::End();
      return;
    }

    ImGui::TextUnformatted("Guest virtual address (hex):");
    ImGui::SetNextItemWidth(140);
    ImGui::InputText("##address", address_buf_, sizeof(address_buf_),
                     ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_CharsUppercase);

    auto* memory = runtime_ ? runtime_->memory() : nullptr;
    uint32_t address = static_cast<uint32_t>(std::strtoul(address_buf_, nullptr, 16));
    address &= ~static_cast<uint32_t>(kBytesPerRow - 1);  // row-align

    constexpr uint32_t kSpanSize = kRows * kBytesPerRow;
    auto* heap = memory ? memory->LookupHeap(address) : nullptr;
    // LookupHeap only confirms *some* heap covers the start address -- it says
    // nothing about whether that heap's pages are actually committed, and a
    // heap can span reserved-but-uncommitted (or guard) pages that fault on
    // touch. QueryRangeAccess walks the real page table for the whole 128-byte
    // span we're about to read, so a bad/half-mapped address shows a message
    // instead of crashing the process.
    bool readable = heap && heap->QueryRangeAccess(address, address + kSpanSize - 1) !=
                                rex::memory::PageAccess::kNoAccess;

    if (!memory) {
      ImGui::TextDisabled("Runtime memory not available.");
    } else if (!readable) {
      ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                         "Address range is not readable (unmapped or no heap).");
    } else {
      const uint8_t* base = memory->TranslateVirtual<const uint8_t*>(address);
      ImGui::Separator();
      for (int row = 0; row < kRows; ++row) {
        char line[128];
        int pos = std::snprintf(line, sizeof(line), "%08X  ", address + row * kBytesPerRow);
        for (int col = 0; col < kBytesPerRow && pos < static_cast<int>(sizeof(line)); ++col) {
          pos += std::snprintf(line + pos, sizeof(line) - pos, "%02X ",
                               base[row * kBytesPerRow + col]);
        }
        ImGui::TextUnformatted(line);
      }
    }

    ImGui::End();
  }

 private:
  rex::Runtime* runtime_ = nullptr;
  bool visible_ = false;
  char address_buf_[16] = {};
};

class MemoryPeekMod : public rex::system::IModPlugin {
 public:
  explicit MemoryPeekMod(rex::Runtime* runtime) : runtime_(runtime) {}

  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    dialog_ = std::make_unique<MemoryPeekDialog>(drawer, runtime_);
  }

 private:
  rex::Runtime* runtime_ = nullptr;
  std::unique_ptr<MemoryPeekDialog> dialog_;
};

}  // namespace

extern "C" REX_MOD_PLUGIN_EXPORT uint32_t rex_mod_abi_version(void) {
  return rex::system::kModPluginAbiVersion;
}

extern "C" REX_MOD_PLUGIN_EXPORT rex::system::IModPlugin* rex_mod_create(
    uint32_t abi_version, const rex::system::ModHostContext* ctx) {
  if (abi_version != rex::system::kModPluginAbiVersion || !ctx) {
    return nullptr;
  }
  return new MemoryPeekMod(ctx->runtime);
}
