#include "custom_driver.h"

#include <rex/logging/log_category.h>
#include <rex/settings_registry.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>

namespace bs1::renderer {

rex::Result<void> CustomXenosGraphicsDriver::Initialize(rex::IDriverContext& context) {
  settingsRegistry_ = &context.GetSettingsRegistry();
  if (auto reg = settingsRegistry_->Register(settings_); !reg) {
    return reg;
  }

  if (!d3d11_.Initialize(&pipeline_, &resources_, &pm4_)) {
    return rex::Err(rex::ErrorCategory::kRuntime, "Failed to initialize D3D11 device");
  }

  pipeline_.Initialize(d3d11_.Device());
  std::filesystem::path cacheDir =
      std::filesystem::temp_directory_path() / "bioshock1-custom-renderer-cache";
  shaders_.Initialize(d3d11_.Device(), cacheDir);
  rendererReady_ = true;

  REXLOG_INFO(
      "custom-renderer: initialized D3D11 Xenos driver (internal_resolution={}, filter={})",
      settings_.internal_resolution.Get(), settings_.present_filter.Get());
  return {};
}

void CustomXenosGraphicsDriver::Shutdown() {
  DetachTitle();
  if (rendererReady_) {
    shaders_.Shutdown();
    pipeline_.Shutdown();
    d3d11_.Shutdown();
    rendererReady_ = false;
  }
  if (settingsRegistry_) {
    settingsRegistry_->Unregister(settings_);
    settingsRegistry_ = nullptr;
  }
}

rex::Result<void> CustomXenosGraphicsDriver::SetSurface(const rex::NativeWindow& window) {
  if (window.type != rex::NativeWindow::Type::kWin32 || !window.window) {
    return rex::Err(rex::ErrorCategory::kRuntime,
                    "Custom D3D11 renderer requires a Win32 HWND surface");
  }
  HWND hwnd = static_cast<HWND>(window.window);
  RECT rect{};
  uint32_t width = 1280;
  uint32_t height = 720;
  if (GetClientRect(hwnd, &rect)) {
    width = static_cast<uint32_t>(std::max<LONG>(1, rect.right - rect.left));
    height = static_cast<uint32_t>(std::max<LONG>(1, rect.bottom - rect.top));
  }
  d3d11_.SetSurface(hwnd, width, height);
  return {};
}

rex::Result<void> CustomXenosGraphicsDriver::AttachTitle(const rex::TitleInfo& info,
                                                         rex::IPhysicalMemory& memory,
                                                         rex::IPhysicalWriteWatch& watch,
                                                         rex::IGuestInterrupts& interrupts) {
  DetachTitle();
  videoMode_ = info.video_mode;
  guestInterrupts_ = &interrupts;

  {
    std::lock_guard lock(d3d11_.ContextMutex());
    resources_.Initialize(d3d11_.Device(), d3d11_.Context(), &memory, &watch);
    commands_.Initialize(&resources_, &pipeline_, &shaders_);
    ApplyLiveSettings(0);
    d3d11_.ForgetBindings();
  }

  pm4_.Initialize(&memory, &interrupts, &d3d11_, &resources_, &shaders_, &commands_,
                  [this]() { NotifySwap(); },
                  [this](uint64_t frame) { ApplyLiveSettings(frame); });

  if (auto vsync = interrupts.StartHostThread("VBlank (Custom)",
                                              [this](std::stop_token stop) { VsyncLoop(stop); })) {
    vsyncThread_ = std::move(*vsync);
  } else {
    return rex::Err(vsync.error());
  }

  titleAttached_ = true;
  REXLOG_INFO("custom-renderer: attached title {:08X} ({}x{} @ {:.1f}Hz)", info.title_id,
              info.video_mode.width, info.video_mode.height, info.video_mode.refresh_rate);
  return {};
}

void CustomXenosGraphicsDriver::DetachTitle() {
  if (!titleAttached_) return;
  titleAttached_ = false;
  vsyncThread_ = rex::ScopeGuard();
  pm4_.Shutdown();
  guestInterrupts_ = nullptr;
  {
    std::lock_guard lock(d3d11_.ContextMutex());
    commands_.Reset();
    resources_.Shutdown();
    d3d11_.ForgetBindings();
  }
}

void CustomXenosGraphicsDriver::ApplyLiveSettings(uint64_t /*frame*/) {
  const uint32_t baseHeight = commands_.ObservedMainHeight();
  const float scale = ComputeEffectiveScale(settings_, baseHeight);
  resources_.RequestScale(scale);

  const int msaaSetting = settings_.msaa.Get();
  const uint32_t msaa = msaaSetting >= 4 ? 4u : msaaSetting >= 2 ? 2u : 1u;
  resources_.RequestMultisample(msaa);

  const uint32_t aniso = static_cast<uint32_t>(std::clamp(settings_.anisotropy.Get(), 1, 16));
  const RenderPipeline::Filter filter =
      aniso > 1 ? RenderPipeline::Filter::Anisotropic : RenderPipeline::Filter::Trilinear;
  if (filter != pipeline_.GetFiltering() || aniso != pipeline_.GetAnisotropy()) {
    pipeline_.SetFiltering(filter, aniso);
    d3d11_.ForgetBindings();
  }

  const bool lanczos = settings_.present_filter.Get() == "lanczos";
  d3d11_.SetPresentOptions(lanczos, settings_.fxaa.Get(), settings_.letterbox.Get(),
                           settings_.vsync.Get());

  if (std::abs(scale - lastAppliedScale_) > 0.005f || msaa != lastAppliedMsaa_) {
    const uint32_t effHeight =
        static_cast<uint32_t>(std::lround(static_cast<double>(baseHeight) * scale));
    REXLOG_INFO(
        "custom-renderer: live internal resolution -> preset={} scale={:.2f}x (~{}p over {}p guest) msaa={}x",
        settings_.internal_resolution.Get(), scale, effHeight, baseHeight, msaa);
    lastAppliedScale_ = scale;
    lastAppliedMsaa_ = msaa;
  }
}

void CustomXenosGraphicsDriver::NotifySwap() {
  uint64_t swaps = 0;
  {
    std::lock_guard lock(frameMutex_);
    swaps = ++swapCount_;
  }
  frameCv_.notify_all();
}

void CustomXenosGraphicsDriver::VsyncLoop(std::stop_token stop) {
  auto next = std::chrono::steady_clock::now();
  while (!stop.stop_requested()) {
    next += std::chrono::microseconds(16667);
    const auto now = std::chrono::steady_clock::now();
    if (next > now) {
      std::this_thread::sleep_until(next);
    } else {
      next = now;
    }
    if (stop.stop_requested()) break;
    pm4_.IncrementCounter();
    if (guestInterrupts_) {
      (void)guestInterrupts_->RaiseGraphicsInterrupt(0u, 2u);
    }
  }
}

rex::gpu::FrameStatus CustomXenosGraphicsDriver::WaitForFrame(rex::u32 timeoutMs) {
  std::unique_lock lock(frameMutex_);
  frameCv_.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                    [this]() { return swapCount_ != swapSeen_; });
  rex::gpu::FrameStatus status{};
  status.new_frame = (swapCount_ != swapSeen_);
  swapSeen_ = swapCount_;
  return status;
}

void CustomXenosGraphicsDriver::ResizeSurface(rex::u32 width, rex::u32 height) {
  d3d11_.ResizeSurface(width, height);
}

bool CustomXenosGraphicsDriver::BeginPresent() {
  // Also sync lightweight present options immediately so F2/ImGui present changes never wait a swap.
  const bool lanczos = settings_.present_filter.Get() == "lanczos";
  d3d11_.SetPresentOptions(lanczos, settings_.fxaa.Get(), settings_.letterbox.Get(),
                           settings_.vsync.Get());
  return d3d11_.BeginPresent();
}

void CustomXenosGraphicsDriver::RenderOverlays(ImDrawData& drawData) {
  d3d11_.RenderOverlays(drawData);
}

void CustomXenosGraphicsDriver::EndPresent() { d3d11_.EndPresent(); }

void CustomXenosGraphicsDriver::InitializeRingBuffer(rex::u32 address, rex::u32 sizeLog2) {
  pm4_.InitializeRingBuffer(address, sizeLog2);
}

void CustomXenosGraphicsDriver::EnableReadPointerWriteBack(rex::u32 address,
                                                           rex::u32 blockSizeLog2) {
  pm4_.EnableReadPointerWriteBack(address, blockSizeLog2);
}

rex::u32 CustomXenosGraphicsDriver::ReadRegister(rex::u32 index) {
  if (const auto fixed = rex::xenos::ReadFixedRegister(index, videoMode_)) {
    return *fixed;
  }
  return pm4_.ReadRegister(index);
}

void CustomXenosGraphicsDriver::WriteRegister(rex::u32 index, rex::u32 value) {
  pm4_.WriteRegister(index, value);
}

}  // namespace bs1::renderer
