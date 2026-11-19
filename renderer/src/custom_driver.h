#pragma once

#include "custom_settings.h"
#include "pm4_processor.h"
#include "render_commands.h"
#include "render_d3d11.h"
#include "render_pipeline.h"
#include "render_resources.h"
#include "render_shaders.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string_view>

#include <rex/driver_context.h>
#include <rex/gpu/xenos_graphics_driver.h>
#include <rex/title.h>
#include <rex/utility.h>
#include <rex/xenos/register.h>

namespace bs1::renderer {

/**
 * Custom Direct3D 11 Xenos graphics driver for BioShock 1 on Windows.
 * Replaces ReXGlue's default Xenos D3D12 driver so internal render scale,
 * MSAA, filtering, FXAA, and letterbox respond live with zero restart.
 */
class CustomXenosGraphicsDriver final : public rex::gpu::IXenosGraphicsDriver {
 public:
  std::string_view GetName() const override { return "custom"; }

  [[nodiscard]] rex::Result<void> Initialize(rex::IDriverContext& context) override;
  void Shutdown() override;

  [[nodiscard]] rex::Result<void> SetSurface(const rex::NativeWindow& window) override;
  [[nodiscard]] rex::Result<void> AttachTitle(const rex::TitleInfo& info,
                                              rex::IPhysicalMemory& memory,
                                              rex::IPhysicalWriteWatch& watch,
                                              rex::IGuestInterrupts& interrupts) override;
  void DetachTitle() override;

  rex::gpu::FrameStatus WaitForFrame(rex::u32 timeoutMs) override;
  void ResizeSurface(rex::u32 width, rex::u32 height) override;
  [[nodiscard]] bool BeginPresent() override;
  void RenderOverlays(ImDrawData& drawData) override;
  void EndPresent() override;

  void InitializeRingBuffer(rex::u32 address, rex::u32 sizeLog2) override;
  void EnableReadPointerWriteBack(rex::u32 address, rex::u32 blockSizeLog2) override;
  rex::u32 ReadRegister(rex::u32 index) override;
  void WriteRegister(rex::u32 index, rex::u32 value) override;

 private:
  void ApplyLiveSettings(uint64_t frame);
  void NotifySwap();
  void VsyncLoop(std::stop_token stop);

  rex::ISettingsRegistry* settingsRegistry_ = nullptr;
  rex::IGuestInterrupts* guestInterrupts_ = nullptr;
  CustomRendererSettings settings_;

  RenderD3D11 d3d11_;
  RenderPipeline pipeline_;
  RenderShaders shaders_;
  RenderResources resources_;
  RenderCommands commands_;
  Pm4Processor pm4_;

  rex::VideoMode videoMode_{};
  bool titleAttached_ = false;
  bool rendererReady_ = false;

  rex::ScopeGuard vsyncThread_;
  std::mutex frameMutex_;
  std::condition_variable frameCv_;
  uint64_t swapCount_ = 0;
  uint64_t swapSeen_ = 0;

  // Track last logged live scale so resolution switches are clearly reported once per change.
  float lastAppliedScale_ = -1.0f;
  uint32_t lastAppliedMsaa_ = 0;
};

}  // namespace bs1::renderer
