#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <stop_token>
#include <vector>

#include <rex/guest.h>
#include <rex/utility.h>

namespace bs1::renderer {

class RenderCommands;
class RenderD3D11;
class RenderResources;
class RenderShaders;

/**
 * Decodes Xenos Type-0/1/2/3 PM4 packets on a dedicated GPU host thread,
 * maintains the 0x5000-register file, gamma LUT, and bin masks, and dispatches
 * draws, resolves, and swaps to RenderCommands / RenderD3D11.
 */
class Pm4Processor {
 public:
  using SwapCallback = std::function<void()>;
  using FrameStartCallback = std::function<void(uint64_t frame)>;

  void Initialize(rex::IPhysicalMemory* memory, rex::IGuestInterrupts* interrupts,
                  RenderD3D11* d3d11, RenderResources* resources, RenderShaders* shaders,
                  RenderCommands* commands, SwapCallback onSwap,
                  FrameStartCallback onFrameStart);
  void Shutdown();

  void InitializeRingBuffer(uint32_t address, uint32_t sizeLog2);
  void EnableReadPointerWriteBack(uint32_t address, uint32_t blockSizeLog2);
  void UpdateWritePointer(uint32_t writePtr);

  uint32_t ReadRegister(uint32_t index) const;
  uint32_t ReadReg(uint32_t index) const { return ReadRegister(index); }
  void WriteRegister(uint32_t index, uint32_t value);
  const uint32_t* Registers() const { return registers_.data(); }

  /** Consumes and clears dirty constant span [first, end) in dwords (0=VS, 1=PS, 2=Bool+Loop). */
  bool TakeConstantSpan(uint32_t range, uint32_t& firstOut, uint32_t& endOut);

  /** Copies the 256-entry 10-bit DC_LUT gamma table if available. */
  bool CopyGammaTable(uint32_t outTable[256], uint64_t& outVersion) const;

  void IncrementCounter() { counter_.fetch_add(1u, std::memory_order_relaxed); }

  uint64_t DrawCount() const { return drawCount_.load(std::memory_order_relaxed); }
  uint64_t ResolveCount() const { return resolveCount_.load(std::memory_order_relaxed); }
  uint64_t SkippedDrawCount() const { return skippedDrawCount_.load(std::memory_order_relaxed); }

 private:
  struct DwordReader {
    const uint8_t* base = nullptr;
    uint32_t capacityDwords = 0;
    uint32_t readIndex = 0;
    uint32_t remainingDwords = 0;

    bool Empty() const { return remainingDwords == 0; }
    uint32_t ReadSwap();
    void Advance(uint32_t dwords);
    /** Returns physical guest address (masked) of the next dword, for IM_LOAD_IMMEDIATE. */
    const uint8_t* ContiguousBytes(uint32_t dwords, std::vector<uint32_t>& scratch) const;
  };

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

  rex::IPhysicalMemory* memory_ = nullptr;
  rex::IGuestInterrupts* interrupts_ = nullptr;
  RenderD3D11* d3d11_ = nullptr;
  RenderResources* resources_ = nullptr;
  RenderShaders* shaders_ = nullptr;
  RenderCommands* commands_ = nullptr;
  SwapCallback onSwap_;
  FrameStartCallback onFrameStart_;

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

  // Dirty register ranges for 0=VS floats, 1=PS floats, 2=Bools+Loops.
  uint32_t dirtyFirst_[3] = {0, 0, 0};
  uint32_t dirtyEnd_[3] = {1024, 1024, 40};

  mutable std::mutex lutMutex_;
  uint32_t gammaLut_[256] = {};
  uint32_t lutIndex_ = 0;
  uint32_t lutMask_ = 7;
  uint32_t lutSeqComp_ = 0;
  uint64_t lutVersion_ = 0;
  bool lutWritten_ = false;

  uint64_t frameIndex_ = 0;
  bool frameStarted_ = false;
  std::atomic<uint64_t> drawCount_{0};
  std::atomic<uint64_t> resolveCount_{0};
  std::atomic<uint64_t> skippedDrawCount_{0};
};

}  // namespace bs1::renderer
