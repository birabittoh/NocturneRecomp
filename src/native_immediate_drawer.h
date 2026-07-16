// nocturnerecomp - ReXGlue Recompiled Project
//
// This file is yours to edit. 'rexglue migrate' will NOT overwrite it.

#pragma once

#include <memory>

#include <rex/logging.h>
#include <rex/ui/immediate_drawer.h>
#include <rex/ui/vulkan/immediate_drawer.h>
#include <rex/ui/vulkan/provider.h>
#include <rex/ui/window.h>

namespace nocturne {

// Detached-mode ImmediateDrawer for the native (non-xenos) renderer. Per the
// OnCreateImmediateDrawer contract (rex/rex_app.h), this must be constructible
// before any GPU device exists -- SetupPresentation calls
// NocturnerecompApp::OnCreateImmediateDrawer() right after the window opens,
// well before OnPreLaunchModule builds the Vulkan device/swapchain. All real
// work is therefore deferred to InitializeVulkan(), called once the device is
// ready, and CreateTexture() returns nullptr (never crashes) until then.
class NativeImmediateDrawer : public rex::ui::ImmediateDrawer {
 public:
  NativeImmediateDrawer() = default;

  // Called once from NocturnerecompApp::OnPreLaunchModule after the
  // rex::ui::vulkan::VulkanProvider/Presenter are constructed and the
  // presenter is attached to the window (so its paint loop is already live).
  // Wraps the SDK's existing VulkanImmediateDrawer -- reusing the same
  // texture/pipeline machinery the xenos Vulkan backend uses -- rather than
  // reimplementing immediate-mode rendering from scratch.
  bool InitializeVulkan(rex::ui::vulkan::VulkanProvider* provider, rex::ui::Presenter* presenter) {
    inner_ = provider->CreateImmediateDrawer();
    if (!inner_) {
      REXGPU_ERROR("NativeImmediateDrawer: failed to create the underlying Vulkan drawer");
      return false;
    }
    inner_->SetPresenter(presenter);
    return true;
  }

  std::unique_ptr<rex::ui::ImmediateTexture> CreateTexture(uint32_t width, uint32_t height,
                                                            rex::ui::ImmediateTextureFilter filter,
                                                            bool is_repeated,
                                                            const uint8_t* data) override {
    // Required by the detached-mode contract: the SDK's ImGui drawer may try
    // to upload its font atlas before InitializeVulkan() has run.
    if (!inner_) {
      return nullptr;
    }
    return inner_->CreateTexture(width, height, filter, is_repeated, data);
  }

  void Begin(rex::ui::UIDrawContext& ui_draw_context, float coordinate_space_width,
             float coordinate_space_height) override {
    if (!inner_) {
      return;
    }
    inner_->Begin(ui_draw_context, coordinate_space_width, coordinate_space_height);
  }

  void BeginDrawBatch(const rex::ui::ImmediateDrawBatch& batch) override {
    if (inner_) {
      inner_->BeginDrawBatch(batch);
    }
  }

  void Draw(const rex::ui::ImmediateDraw& draw) override {
    if (inner_) {
      inner_->Draw(draw);
    }
  }

  void EndDrawBatch() override {
    if (inner_) {
      inner_->EndDrawBatch();
    }
  }

  void End() override {
    if (inner_) {
      inner_->End();
    }
  }

 private:
  // Owns the SDK's Vulkan-backed ImmediateDrawer once InitializeVulkan() has
  // run; null before that (headless-boot window, no device yet).
  std::unique_ptr<rex::ui::ImmediateDrawer> inner_;
};

}  // namespace nocturne
