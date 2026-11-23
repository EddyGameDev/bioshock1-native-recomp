#pragma once

#include "../custom_settings.h"
#include "gles3_backend.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <stop_token>
#include <string_view>
#include <vector>

#include <rex/driver_context.h>
#include <rex/gpu/xenos_graphics_driver.h>
#include <rex/title.h>
#include <rex/utility.h>

namespace bs1::renderer {

/**
 * Standalone custom OpenGL ES 3.2 Xenos graphics driver for BioShock 1 on Android.
 * Provides true live, zero-restart fractional internal resolution scaling
 * (360p, 540p, 720p, 900p, 1080p, 1440p) and Lanczos2 / Bilinear present filtering.
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
  struct DwordReader {
    const uint8_t* base = nullptr;
    uint32_t capacityDwords = 0;
    uint32_t readIndex = 0;
    uint32_t remainingDwords = 0;

    bool Empty() const { return remainingDwords == 0; }
    inline uint32_t ReadSwap() {
      if (!base || remainingDwords == 0 || capacityDwords == 0) return 0;
      uint32_t raw = 0;
      std::memcpy(&raw, base + static_cast<size_t>(readIndex) * 4u, 4);
      if (++readIndex >= capacityDwords) readIndex = 0;
      --remainingDwords;
      return __builtin_bswap32(raw);
    }
    inline void ReadSwapBulk(uint32_t* dst, uint32_t n) {
      if (!base || n == 0 || capacityDwords == 0) return;
      const uint32_t avail = std::min(n, remainingDwords);
      if (readIndex + avail <= capacityDwords) {
        const auto* src = reinterpret_cast<const uint32_t*>(base + static_cast<size_t>(readIndex) * 4u);
        for (uint32_t i = 0; i < avail; ++i) {
          dst[i] = __builtin_bswap32(src[i]);
        }
        readIndex += avail;
        if (readIndex >= capacityDwords) readIndex = 0;
        remainingDwords -= avail;
        return;
      }
      for (uint32_t i = 0; i < avail; ++i) dst[i] = ReadSwap();
    }
    inline void Advance(uint32_t dwords) {
      const uint32_t skip = std::min(dwords, remainingDwords);
      if (capacityDwords > 0) {
        readIndex += skip;
        if (readIndex >= capacityDwords) readIndex %= capacityDwords;
      }
      remainingDwords -= skip;
    }
    const uint8_t* ContiguousBytes(uint32_t dwords, std::vector<uint32_t>& scratch) const;
  };

  void ApplyLiveSettings();
  void VsyncLoop(std::stop_token stop);
  void WorkerLoop(std::stop_token stop);
  uint32_t ExecutePrimaryBuffer(uint32_t readIndex, uint32_t writeIndex, std::stop_token stop);
  void ExecuteIndirectBuffer(uint32_t physAddr, uint32_t dwordCount, std::stop_token stop);
  bool ExecutePacket(DwordReader& reader, std::stop_token stop);
  bool ExecuteType0(DwordReader& reader, uint32_t packet);
  bool ExecuteType1(DwordReader& reader, uint32_t packet);
  bool ExecuteType3(DwordReader& reader, uint32_t packet, std::stop_token stop);
  bool ExecuteDrawPacket(DwordReader& reader, uint32_t countRemaining, bool hasVizQuery);

  void HandleLutWrite(uint32_t index, uint32_t value);
  void SyncCoherency();
  void StorePhysical32Host(uint32_t physAddr, uint32_t value);
  void StorePhysical32GpuSwap(uint32_t physAddr, uint32_t value, uint32_t endian);
  uint32_t LoadPhysical32GpuSwap(uint32_t physAddr, uint32_t endian);
  void LoadShaderMicrocode(bool pixel, const uint8_t* bytes, uint32_t dwords);

  rex::ISettingsRegistry* settingsRegistry_ = nullptr;
  rex::IPhysicalMemory* memory_ = nullptr;
  rex::IPhysicalWriteWatch* watch_ = nullptr;
  rex::IGuestInterrupts* interrupts_ = nullptr;
  CustomRendererSettings settings_;
  gles3::Gles3Backend backend_;

  rex::VideoMode videoMode_{};
  bool titleAttached_ = false;
  float lastAppliedScale_ = -1.0f;

  rex::ScopeGuard vsyncThread_;
  rex::ScopeGuard workerThread_;
  std::mutex ringMutex_;
  std::condition_variable ringCv_;
  bool stopping_ = false;
  uint32_t ringAddress_ = 0;
  uint32_t ringSizeBytes_ = 0;
  uint32_t readIndex_ = 0;
  std::atomic<uint32_t> writeIndex_{0xBAADF00Du};
  uint32_t writebackAddress_ = 0;
  std::atomic<uint32_t> counter_{0};

  std::array<uint32_t, 0x5000> registers_{};
  uint64_t binMask_ = 0xFFFFFFFFFFFFFFFFULL;
  uint64_t binSelect_ = 0xFFFFFFFFFFFFFFFFULL;

  uint32_t gammaLut_[256] = {};
  uint32_t lutIndex_ = 0;
  uint32_t lutMask_ = 7;
  uint32_t lutSeqComp_ = 0;
  uint64_t lutVersion_ = 0;
  bool lutWritten_ = false;

  uint64_t frameIndex_ = 0;
  bool frameStarted_ = false;
  std::vector<uint32_t> shaderWordScratch_;
  std::vector<uint32_t> imLoadScratch_;

  // Direct-mapped cache for static PM4_IM_LOAD shader microcode (validated via write watch).
  struct ShaderLoadCacheEntry {
    uint32_t addr = 0;
    uint32_t dwords = 0;
    uint32_t handle = 0;
    uint32_t seq = 0;
    bool pixel = false;
    bool watchArmed = false;
    bool valid = false;
  };
  ShaderLoadCacheEntry shaderLoadCache_[256] = {};
};

}  // namespace bs1::renderer
