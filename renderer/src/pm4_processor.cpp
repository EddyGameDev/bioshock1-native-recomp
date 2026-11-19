#include "pm4_processor.h"

#include "render_commands.h"
#include "render_d3d11.h"
#include "render_resources.h"
#include "render_shaders.h"
#include "render_state.h"

#include <rex/logging/log_category.h>
#include <rex/xenos/format.h>
#include <rex/xenos/packet.h>
#include <rex/xenos/register.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>
#include <utility>

namespace bs1::renderer {

namespace {

constexpr uint32_t kPhysicalMask = 0x1FFFFFFFu;
constexpr uint32_t kSwapSignatureBE = 0x53574150u;  // 'SWAP'
constexpr uint32_t kSwapSignatureLE = 0x50415753u;  // 'PAWS'

inline uint32_t ByteSwap32(uint32_t v) {
  return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
}

inline uint32_t SwapGpuEndian(uint32_t v, uint32_t endian) {
  return rex::xenos::GPUSwap(v, static_cast<rex::xenos::Endian>(endian & 3u));
}

}  // namespace

uint32_t Pm4Processor::DwordReader::ReadSwap() {
  if (!base || remainingDwords == 0 || capacityDwords == 0) return 0;
  uint32_t raw = 0;
  std::memcpy(&raw, base + static_cast<size_t>(readIndex) * 4u, 4);
  readIndex = (readIndex + 1u) % capacityDwords;
  --remainingDwords;
  return ByteSwap32(raw);
}

void Pm4Processor::DwordReader::Advance(uint32_t dwords) {
  const uint32_t skip = std::min(dwords, remainingDwords);
  if (capacityDwords > 0) readIndex = (readIndex + skip) % capacityDwords;
  remainingDwords -= skip;
}

const uint8_t* Pm4Processor::DwordReader::ContiguousBytes(uint32_t dwords,
                                                          std::vector<uint32_t>& scratch) const {
  if (!base || dwords == 0 || capacityDwords == 0 || remainingDwords < dwords) return nullptr;
  if (readIndex + dwords <= capacityDwords) {
    return base + static_cast<size_t>(readIndex) * 4u;
  }
  scratch.resize(dwords);
  uint32_t idx = readIndex;
  for (uint32_t i = 0; i < dwords; ++i) {
    std::memcpy(&scratch[i], base + static_cast<size_t>(idx) * 4u, 4);
    idx = (idx + 1u) % capacityDwords;
  }
  return reinterpret_cast<const uint8_t*>(scratch.data());
}

void Pm4Processor::Initialize(rex::IPhysicalMemory* memory, rex::IGuestInterrupts* interrupts,
                              RenderD3D11* d3d11, RenderResources* resources, RenderShaders* shaders,
                              RenderCommands* commands, SwapCallback onSwap,
                              FrameStartCallback onFrameStart) {
  Shutdown();
  memory_ = memory;
  interrupts_ = interrupts;
  d3d11_ = d3d11;
  resources_ = resources;
  shaders_ = shaders;
  commands_ = commands;
  onSwap_ = std::move(onSwap);
  onFrameStart_ = std::move(onFrameStart);

  registers_.fill(0);
  binMask_ = 0xFFFFFFFFFFFFFFFFULL;
  binSelect_ = 0xFFFFFFFFFFFFFFFFULL;
  ringAddress_ = 0;
  ringSizeBytes_ = 0;
  readIndex_ = 0;
  writeIndex_.store(0xBAADF00Du, std::memory_order_relaxed);
  writebackAddress_ = 0;
  stopping_ = false;
  frameIndex_ = 0;
  frameStarted_ = false;

  dirtyFirst_[0] = dirtyFirst_[1] = dirtyFirst_[2] = 0;
  dirtyEnd_[0] = 1024;
  dirtyEnd_[1] = 1024;
  dirtyEnd_[2] = 40;

  if (interrupts_) {
    auto thread = interrupts_->StartHostThread("GPU Commands (Custom)",
                                               [this](std::stop_token stop) { WorkerLoop(stop); });
    if (thread) {
      workerThread_ = std::move(*thread);
    } else {
      REXLOG_ERROR("custom-renderer: unable to start GPU thread: {}", thread.error().message);
    }
  }
}

void Pm4Processor::Shutdown() {
  {
    std::lock_guard lock(ringMutex_);
    stopping_ = true;
  }
  ringCv_.notify_all();
  workerThread_ = rex::ScopeGuard();
  memory_ = nullptr;
  interrupts_ = nullptr;
}

void Pm4Processor::InitializeRingBuffer(uint32_t address, uint32_t sizeLog2) {
  {
    std::lock_guard lock(ringMutex_);
    ringAddress_ = address & kPhysicalMask;
    ringSizeBytes_ = 1u << (sizeLog2 + 3u);
    readIndex_ = 0;
    writeIndex_.store(0, std::memory_order_release);
  }
  ringCv_.notify_all();
}

void Pm4Processor::EnableReadPointerWriteBack(uint32_t address, uint32_t /*blockSizeLog2*/) {
  std::lock_guard lock(ringMutex_);
  writebackAddress_ = address & kPhysicalMask;
}

void Pm4Processor::UpdateWritePointer(uint32_t writePtr) {
  writeIndex_.store(writePtr, std::memory_order_release);
  ringCv_.notify_all();
}

uint32_t Pm4Processor::ReadRegister(uint32_t index) const {
  return index < registers_.size() ? registers_[index] : 0;
}

void Pm4Processor::HandleLutWrite(uint32_t index, uint32_t value) {
  using Reg = rex::xenos::Register;
  std::lock_guard lock(lutMutex_);
  auto ensureInit = [&]() {
    if (lutWritten_) return;
    for (uint32_t i = 0; i < 256; ++i) {
      const uint32_t v = (i << 2) | (i >> 6);
      gammaLut_[i] = (v << 20) | (v << 10) | v;
    }
    lutWritten_ = true;
  };
  switch (index) {
    case static_cast<uint32_t>(Reg::DC_LUT_RW_INDEX):
      lutIndex_ = value & 0xFFu;
      lutSeqComp_ = 0;
      break;
    case static_cast<uint32_t>(Reg::DC_LUT_WRITE_EN_MASK):
      lutMask_ = value & 7u;
      break;
    case static_cast<uint32_t>(Reg::DC_LUT_30_COLOR): {
      ensureInit();
      uint32_t entry = gammaLut_[lutIndex_];
      if (lutMask_ & 1u) entry = (entry & ~0x3FFu) | (value & 0x3FFu);
      if (lutMask_ & 2u) entry = (entry & ~(0x3FFu << 10)) | (value & (0x3FFu << 10));
      if (lutMask_ & 4u) entry = (entry & ~(0x3FFu << 20)) | (value & (0x3FFu << 20));
      gammaLut_[lutIndex_] = entry;
      lutIndex_ = (lutIndex_ + 1u) & 0xFFu;
      ++lutVersion_;
      break;
    }
    case static_cast<uint32_t>(Reg::DC_LUT_SEQ_COLOR): {
      ensureInit();
      const uint32_t c10 = (value >> 6) & 0x3FFu;
      const uint32_t shift = (2u - lutSeqComp_) * 10u;
      if ((lutMask_ >> (2u - lutSeqComp_)) & 1u) {
        gammaLut_[lutIndex_] = (gammaLut_[lutIndex_] & ~(0x3FFu << shift)) | (c10 << shift);
      }
      if (++lutSeqComp_ >= 3u) {
        lutSeqComp_ = 0;
        lutIndex_ = (lutIndex_ + 1u) & 0xFFu;
        ++lutVersion_;
      }
      break;
    }
    default:
      break;
  }
}

void Pm4Processor::WriteRegister(uint32_t index, uint32_t value) {
  using Reg = rex::xenos::Register;
  if (index == static_cast<uint32_t>(Reg::CP_RB_WPTR)) {
    registers_[index] = value;
    UpdateWritePointer(value);
    return;
  }
  if (index >= registers_.size()) return;

  registers_[index] = value;

  if (index >= RenderState::VertexConstants && index < RenderState::VertexConstants + 1024u) {
    const uint32_t off = index - RenderState::VertexConstants;
    dirtyFirst_[0] = std::min(dirtyFirst_[0], off);
    dirtyEnd_[0] = std::max(dirtyEnd_[0], off + 1u);
  } else if (index >= RenderState::PixelConstants && index < RenderState::PixelConstants + 1024u) {
    const uint32_t off = index - RenderState::PixelConstants;
    dirtyFirst_[1] = std::min(dirtyFirst_[1], off);
    dirtyEnd_[1] = std::max(dirtyEnd_[1], off + 1u);
  } else if (index >= RenderState::BoolConstants && index < RenderState::BoolConstants + 40u) {
    const uint32_t off = index - RenderState::BoolConstants;
    dirtyFirst_[2] = std::min(dirtyFirst_[2], off);
    dirtyEnd_[2] = std::max(dirtyEnd_[2], off + 1u);
  } else if (index >= static_cast<uint32_t>(Reg::SCRATCH_REG0) &&
             index <= static_cast<uint32_t>(Reg::SCRATCH_REG7)) {
    const uint32_t slot = index - static_cast<uint32_t>(Reg::SCRATCH_REG0);
    if ((registers_[static_cast<uint32_t>(Reg::SCRATCH_UMSK)] & (1u << slot)) != 0) {
      const uint32_t addr = registers_[static_cast<uint32_t>(Reg::SCRATCH_ADDR)] + slot * 4u;
      StorePhysical32Host(addr, value);
    }
  } else if (index == static_cast<uint32_t>(Reg::COHER_STATUS_HOST)) {
    registers_[index] |= 0x80000000u;
  } else if (index == static_cast<uint32_t>(Reg::DC_LUT_RW_INDEX) ||
             index == static_cast<uint32_t>(Reg::DC_LUT_WRITE_EN_MASK) ||
             index == static_cast<uint32_t>(Reg::DC_LUT_30_COLOR) ||
             index == static_cast<uint32_t>(Reg::DC_LUT_SEQ_COLOR)) {
    HandleLutWrite(index, value);
  }
}

bool Pm4Processor::TakeConstantSpan(uint32_t range, uint32_t& firstOut, uint32_t& endOut) {
  if (range >= 3) return false;
  firstOut = dirtyFirst_[range];
  endOut = dirtyEnd_[range];
  if (firstOut >= endOut) return false;
  dirtyFirst_[range] = 1024;
  dirtyEnd_[range] = 0;
  return true;
}

bool Pm4Processor::CopyGammaTable(uint32_t outTable[256], uint64_t& outVersion) const {
  std::lock_guard lock(lutMutex_);
  if (!lutWritten_) return false;
  std::memcpy(outTable, gammaLut_, sizeof(uint32_t) * 256);
  outVersion = lutVersion_;
  return true;
}

void Pm4Processor::SyncCoherency() {
  registers_[static_cast<uint32_t>(rex::xenos::Register::COHER_STATUS_HOST)] = 0;
}

void Pm4Processor::StorePhysical32Host(uint32_t physAddr, uint32_t value) {
  if (!memory_) return;
  (void)memory_->StoreRelease32(physAddr & kPhysicalMask, value);
}

void Pm4Processor::StorePhysical32GpuSwap(uint32_t physAddr, uint32_t value, uint32_t endian) {
  StorePhysical32Host(physAddr, ByteSwap32(SwapGpuEndian(value, endian)));
}

uint32_t Pm4Processor::LoadPhysical32GpuSwap(uint32_t physAddr, uint32_t endian) {
  if (!memory_) return 0;
  auto loaded = memory_->LoadAcquire32(physAddr & kPhysicalMask);
  if (!loaded) return 0;
  return SwapGpuEndian(ByteSwap32(*loaded), endian);
}

void Pm4Processor::WorkerLoop(std::stop_token stop) {
  while (!stop.stop_requested()) {
    uint32_t writePtr = 0;
    uint32_t curRead = 0;
    uint32_t rbAddr = 0;
    uint32_t rbSize = 0;
    {
      std::unique_lock lock(ringMutex_);
      ringCv_.wait(lock, [&]() {
        if (stopping_ || stop.stop_requested()) return true;
        const uint32_t w = writeIndex_.load(std::memory_order_acquire);
        return ringSizeBytes_ > 0 && w != 0xBAADF00Du && w != readIndex_;
      });
      if (stopping_ || stop.stop_requested()) break;
      writePtr = writeIndex_.load(std::memory_order_acquire);
      curRead = readIndex_;
      rbAddr = ringAddress_;
      rbSize = ringSizeBytes_;
    }

    if (rbSize == 0 || curRead == writePtr) continue;
    const uint32_t nextRead = ExecutePrimaryBuffer(curRead, writePtr, stop);
    {
      std::lock_guard lock(ringMutex_);
      readIndex_ = nextRead;
      registers_[static_cast<uint32_t>(rex::xenos::Register::CP_RB_RPTR)] = nextRead;
      if (writebackAddress_ != 0) {
        StorePhysical32Host(writebackAddress_, nextRead);
      }
    }
  }
}

uint32_t Pm4Processor::ExecutePrimaryBuffer(uint32_t readIndex, uint32_t writeIndex,
                                            std::stop_token stop) {
  if (!memory_ || ringSizeBytes_ == 0) return writeIndex;
  auto span = memory_->GetReadOnlyPhysicalSpan({ringAddress_, ringSizeBytes_});
  if (!span) return writeIndex;

  const uint32_t capDwords = ringSizeBytes_ / 4u;
  const uint32_t r = readIndex % capDwords;
  const uint32_t w = writeIndex % capDwords;
  const uint32_t count = (w >= r) ? (w - r) : (capDwords - r + w);

  DwordReader reader{reinterpret_cast<const uint8_t*>(span->data()), capDwords, r, count};
  while (!reader.Empty() && !stop.stop_requested()) {
    if (!ExecutePacket(reader, stop)) break;
  }
  return writeIndex;
}

void Pm4Processor::ExecuteIndirectBuffer(uint32_t physAddr, uint32_t dwordCount,
                                         std::stop_token stop) {
  if (!memory_ || dwordCount == 0) return;
  const uint32_t addr = physAddr & kPhysicalMask;
  auto span = memory_->GetReadOnlyPhysicalSpan({addr, dwordCount * 4u});
  if (!span) return;
  DwordReader reader{reinterpret_cast<const uint8_t*>(span->data()), dwordCount, 0, dwordCount};
  while (!reader.Empty() && !stop.stop_requested()) {
    if (!ExecutePacket(reader, stop)) break;
  }
}

bool Pm4Processor::ExecutePacket(DwordReader& reader, std::stop_token stop) {
  const uint32_t packet = reader.ReadSwap();
  if (packet == 0) return true;
  const uint32_t type = packet >> 30;
  switch (type) {
    case 0: return ExecuteType0(reader, packet);
    case 1: return ExecuteType1(reader, packet);
    case 2: return true;
    case 3: return ExecuteType3(reader, packet, stop);
    default: return false;
  }
}

bool Pm4Processor::ExecuteType0(DwordReader& reader, uint32_t packet) {
  const uint32_t count = ((packet >> 16) & 0x3FFFu) + 1u;
  if (reader.remainingDwords < count) return false;
  const uint32_t baseIndex = packet & 0x7FFFu;
  const bool oneReg = ((packet >> 15) & 1u) != 0;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t data = reader.ReadSwap();
    WriteRegister(oneReg ? baseIndex : (baseIndex + i), data);
  }
  return true;
}

bool Pm4Processor::ExecuteType1(DwordReader& reader, uint32_t packet) {
  if (reader.remainingDwords < 2) return false;
  const uint32_t r1 = packet & 0x7FFu;
  const uint32_t r2 = (packet >> 11) & 0x7FFu;
  const uint32_t d1 = reader.ReadSwap();
  const uint32_t d2 = reader.ReadSwap();
  WriteRegister(r1, d1);
  WriteRegister(r2, d2);
  return true;
}

bool Pm4Processor::ExecuteDrawPacket(DwordReader& reader, uint32_t countRemaining,
                                     bool hasVizQuery) {
  if (hasVizQuery) {
    if (countRemaining == 0) return false;
    reader.ReadSwap();
    --countRemaining;
  }
  if (countRemaining == 0) return false;
  const uint32_t initiator = reader.ReadSwap();
  --countRemaining;
  WriteRegister(static_cast<uint32_t>(rex::xenos::Register::VGT_DRAW_INITIATOR), initiator);

  const uint32_t sourceSelect = (initiator >> 6) & 3u;
  uint32_t indexBase = 0;
  if (sourceSelect == 0 && countRemaining >= 2) {
    const uint32_t dmaBase = reader.ReadSwap();
    const uint32_t dmaSize = reader.ReadSwap();
    countRemaining -= 2;
    WriteRegister(static_cast<uint32_t>(rex::xenos::Register::VGT_DMA_BASE), dmaBase);
    WriteRegister(static_cast<uint32_t>(rex::xenos::Register::VGT_DMA_SIZE), dmaSize);
    const bool wide = ((initiator >> 11) & 1u) != 0;
    indexBase = dmaBase & ~(wide ? 3u : 1u);
  }
  reader.Advance(countRemaining);

  const uint32_t viz = ReadRegister(static_cast<uint32_t>(rex::xenos::Register::PA_SC_VIZ_QUERY));
  if ((viz & 1u) != 0 && (viz & 0x100u) != 0) {
    return true;
  }

  std::lock_guard ctxLock(d3d11_->ContextMutex());
  if (!frameStarted_) {
    frameStarted_ = true;
    if (onFrameStart_) onFrameStart_(frameIndex_);
    if (resources_->BeginFrame(frameIndex_)) {
      d3d11_->ForgetBindings();
    }
  }

  const uint32_t mode = ReadRegister(RenderState::ModeControl) & 7u;
  if (mode == 6u) {
    RenderState::ResolveCommand resolve;
    const bool ok = commands_->PrepareResolve(*this, resolve);
    if (ok) {
      d3d11_->ExecuteResolve(resolve);
      resolveCount_.fetch_add(1u, std::memory_order_relaxed);
    }
  } else if (mode != 0u) {
    RenderState::DrawCommand draw;
    const bool ok = commands_->PrepareDraw(*this, initiator, indexBase, draw);
    if (ok) {
      d3d11_->ExecuteDraw(draw);
      drawCount_.fetch_add(1u, std::memory_order_relaxed);
    } else {
      skippedDrawCount_.fetch_add(1u, std::memory_order_relaxed);
    }
  }
  return true;
}

bool Pm4Processor::ExecuteType3(DwordReader& reader, uint32_t packet, std::stop_token stop) {
  const uint32_t opcode = (packet >> 8) & 0x7Fu;
  const uint32_t count = ((packet >> 16) & 0x3FFFu) + 1u;
  if (reader.remainingDwords < count) return false;

  if ((packet & 1u) != 0) {
    const bool anyPass = (binSelect_ & binMask_) != 0;
    if (!anyPass || opcode == rex::xenos::PM4_XE_SWAP) {
      reader.Advance(count);
      return true;
    }
  }

  switch (opcode) {
    case rex::xenos::PM4_ME_INIT:
    case rex::xenos::PM4_NOP:
    case rex::xenos::PM4_INVALIDATE_STATE:
    case rex::xenos::PM4_CONTEXT_UPDATE:
    case rex::xenos::PM4_WAIT_FOR_IDLE:
      reader.Advance(count);
      return true;

    case rex::xenos::PM4_INTERRUPT: {
      const uint32_t mask = reader.ReadSwap();
      reader.Advance(count - 1);
      if (interrupts_) {
        for (uint32_t cpu = 0; cpu < 6; ++cpu) {
          if (mask & (1u << cpu)) (void)interrupts_->RaiseGraphicsInterrupt(1u, cpu);
        }
      }
      return true;
    }

    case rex::xenos::PM4_XE_SWAP: {
      const uint32_t magic = reader.ReadSwap();
      const uint32_t fbAddr = reader.ReadSwap();
      const uint32_t fbWidth = reader.ReadSwap();
      const uint32_t fbHeight = reader.ReadSwap();
      reader.Advance(count > 4 ? count - 4 : 0);
      if (magic == kSwapSignatureBE || magic == kSwapSignatureLE) {
        commands_->ObserveSwapHeight(fbWidth, fbHeight);
        d3d11_->OnSwap(fbAddr, fbWidth, fbHeight);
        if (onSwap_) onSwap_();
        ++frameIndex_;
        frameStarted_ = false;
      }
      counter_.fetch_add(1u, std::memory_order_relaxed);
      return true;
    }

    case rex::xenos::PM4_INDIRECT_BUFFER:
    case rex::xenos::PM4_INDIRECT_BUFFER_PFD: {
      const uint32_t listPtr = reader.ReadSwap() & kPhysicalMask;
      const uint32_t listLen = reader.ReadSwap() & 0xFFFFFu;
      reader.Advance(count > 2 ? count - 2 : 0);
      ExecuteIndirectBuffer(listPtr, listLen, stop);
      return true;
    }

    case rex::xenos::PM4_WAIT_REG_MEM: {
      const uint32_t waitInfo = reader.ReadSwap();
      const uint32_t pollAddr = reader.ReadSwap();
      const uint32_t ref = reader.ReadSwap();
      const uint32_t mask = reader.ReadSwap();
      const uint32_t wait = reader.ReadSwap();
      reader.Advance(count > 5 ? count - 5 : 0);
      const bool isMem = (waitInfo & 0x10u) != 0;
      const uint32_t fn = waitInfo & 7u;
      uint32_t attempts = 0;
      while (!stop.stop_requested()) {
        uint32_t val = 0;
        if (isMem) {
          val = LoadPhysical32GpuSwap(pollAddr & ~3u, pollAddr & 3u);
        } else {
          if (pollAddr == static_cast<uint32_t>(rex::xenos::Register::COHER_STATUS_HOST)) {
            SyncCoherency();
          }
          val = ReadRegister(pollAddr);
        }
        bool matched = false;
        const uint32_t masked = val & mask;
        switch (fn) {
          case 0: matched = false; break;
          case 1: matched = masked < ref; break;
          case 2: matched = masked <= ref; break;
          case 3: matched = masked == ref; break;
          case 4: matched = masked != ref; break;
          case 5: matched = masked >= ref; break;
          case 6: matched = masked > ref; break;
          case 7: matched = true; break;
        }
        if (matched) break;
        if (attempts++ < 16) std::this_thread::yield();
        else std::this_thread::sleep_for(std::chrono::microseconds(wait >= 0x100 ? 500 : 100));
      }
      return true;
    }

    case rex::xenos::PM4_REG_RMW: {
      const uint32_t rmwInfo = reader.ReadSwap();
      const uint32_t andMask = reader.ReadSwap();
      const uint32_t orMask = reader.ReadSwap();
      reader.Advance(count > 3 ? count - 3 : 0);
      const uint32_t reg = rmwInfo & 0x1FFFu;
      uint32_t val = ReadRegister(reg);
      val &= ((rmwInfo >> 31) & 1u) ? ReadRegister(andMask & 0x1FFFu) : andMask;
      val |= ((rmwInfo >> 30) & 1u) ? ReadRegister(orMask & 0x1FFFu) : orMask;
      WriteRegister(reg, val);
      return true;
    }

    case rex::xenos::PM4_REG_TO_MEM: {
      const uint32_t regAddr = reader.ReadSwap();
      const uint32_t memAddr = reader.ReadSwap();
      reader.Advance(count > 2 ? count - 2 : 0);
      StorePhysical32GpuSwap(memAddr & ~3u, ReadRegister(regAddr), memAddr & 3u);
      return true;
    }

    case rex::xenos::PM4_MEM_WRITE: {
      uint32_t writeAddr = reader.ReadSwap();
      for (uint32_t i = 1; i < count; ++i) {
        const uint32_t data = reader.ReadSwap();
        StorePhysical32GpuSwap(writeAddr & ~3u, data, writeAddr & 3u);
        writeAddr += 4u;
      }
      return true;
    }

    case rex::xenos::PM4_COND_WRITE: {
      const uint32_t info = reader.ReadSwap();
      const uint32_t pollAddr = reader.ReadSwap();
      const uint32_t ref = reader.ReadSwap();
      const uint32_t mask = reader.ReadSwap();
      const uint32_t writeAddr = reader.ReadSwap();
      const uint32_t writeData = reader.ReadSwap();
      reader.Advance(count > 6 ? count - 6 : 0);
      const uint32_t val =
          (info & 0x10u) ? LoadPhysical32GpuSwap(pollAddr & ~3u, pollAddr & 3u) : ReadRegister(pollAddr);
      const uint32_t m = val & mask;
      bool ok = false;
      switch (info & 7u) {
        case 1: ok = m < ref; break;
        case 2: ok = m <= ref; break;
        case 3: ok = m == ref; break;
        case 4: ok = m != ref; break;
        case 5: ok = m >= ref; break;
        case 6: ok = m > ref; break;
        case 7: ok = true; break;
      }
      if (ok) {
        if (info & 0x100u) StorePhysical32GpuSwap(writeAddr & ~3u, writeData, writeAddr & 3u);
        else WriteRegister(writeAddr, writeData);
      }
      return true;
    }

    case rex::xenos::PM4_EVENT_WRITE: {
      const uint32_t initiator = reader.ReadSwap();
      reader.Advance(count > 1 ? count - 1 : 0);
      WriteRegister(static_cast<uint32_t>(rex::xenos::Register::VGT_EVENT_INITIATOR),
                    initiator & 0x3Fu);
      return true;
    }

    case rex::xenos::PM4_EVENT_WRITE_ZPD: {
      const uint32_t initiator = reader.ReadSwap();
      reader.Advance(count > 1 ? count - 1 : 0);
      WriteRegister(static_cast<uint32_t>(rex::xenos::Register::VGT_EVENT_INITIATOR),
                    initiator & 0x3Fu);
      const uint32_t sampleAddr =
          ReadRegister(static_cast<uint32_t>(rex::xenos::Register::RB_SAMPLE_COUNT_ADDR)) &
          kPhysicalMask;
      if (sampleAddr != 0 && memory_) {
        if (auto writable = memory_->GetWritablePhysicalSpan({sampleAddr, 32})) {
          auto* words = reinterpret_cast<uint32_t*>(writable->data());
          constexpr uint32_t kFinishedLE = 0xFFFFFEEDu;
          const uint32_t kFinishedBE = ByteSwap32(0xFFFFFEEDu);
          const bool isEndLE = (words[2] == kFinishedLE || words[3] == kFinishedLE ||
                                words[4] == kFinishedLE || words[5] == kFinishedLE);
          const bool isEndBE = (words[2] == kFinishedBE || words[3] == kFinishedBE ||
                                words[4] == kFinishedBE || words[5] == kFinishedBE);
          const uint32_t samples = (isEndLE || isEndBE)
                                       ? (isEndBE ? ByteSwap32(1000u) : 1000u)
                                       : 0u;
          words[0] = samples;
          words[1] = 0;
          words[6] = 0;
          words[7] = 0;
          std::atomic_ref<uint32_t>(words[3]).store(0u, std::memory_order_release);
          std::atomic_ref<uint32_t>(words[2]).store(0u, std::memory_order_release);
          std::atomic_ref<uint32_t>(words[5]).store(0u, std::memory_order_release);
          std::atomic_ref<uint32_t>(words[4]).store(samples, std::memory_order_release);
        }
      }
      return true;
    }

    case rex::xenos::PM4_EVENT_WRITE_SHD: {
      const uint32_t initiator = reader.ReadSwap();
      const uint32_t address = reader.ReadSwap();
      const uint32_t value = reader.ReadSwap();
      reader.Advance(count > 3 ? count - 3 : 0);
      WriteRegister(static_cast<uint32_t>(rex::xenos::Register::VGT_EVENT_INITIATOR),
                    initiator & 0x3Fu);
      const uint32_t dataVal =
          ((initiator >> 31) & 1u) ? counter_.load(std::memory_order_relaxed) : value;
      StorePhysical32GpuSwap(address & ~3u, dataVal, address & 3u);
      return true;
    }

    case rex::xenos::PM4_EVENT_WRITE_EXT: {
      const uint32_t initiator = reader.ReadSwap();
      const uint32_t address = reader.ReadSwap();
      reader.Advance(count > 2 ? count - 2 : 0);
      WriteRegister(static_cast<uint32_t>(rex::xenos::Register::VGT_EVENT_INITIATOR),
                    initiator & 0x3Fu);
      const uint32_t addr = address & kPhysicalMask & ~3u;
      if (memory_) {
        if (auto writable = memory_->GetWritablePhysicalSpan({addr, 12})) {
          const uint16_t extents[6] = {0, 8192 >> 3, 0, 8192 >> 3, 0, 1};
          auto* dst = reinterpret_cast<uint16_t*>(writable->data());
          for (int i = 0; i < 6; ++i) {
            dst[i] = static_cast<uint16_t>((extents[i] >> 8) | (extents[i] << 8));
          }
        }
      }
      return true;
    }

    case rex::xenos::PM4_VIZ_QUERY: {
      const uint32_t dword0 = reader.ReadSwap();
      reader.Advance(count > 1 ? count - 1 : 0);
      const uint32_t id = dword0 & 0x3Fu;
      if ((dword0 & 0x100u) != 0) {
        const uint32_t reg = static_cast<uint32_t>(
            id < 32 ? rex::xenos::Register::PA_SC_VIZ_QUERY_STATUS_0
                    : rex::xenos::Register::PA_SC_VIZ_QUERY_STATUS_1);
        registers_[reg] |= (1u << (id & 31u));
      }
      return true;
    }

    case rex::xenos::PM4_SET_BIN_MASK_LO:
      binMask_ = (binMask_ & 0xFFFFFFFF00000000ULL) | reader.ReadSwap();
      reader.Advance(count > 1 ? count - 1 : 0);
      return true;
    case rex::xenos::PM4_SET_BIN_MASK_HI:
      binMask_ = (binMask_ & 0x00000000FFFFFFFFULL) | (uint64_t{reader.ReadSwap()} << 32);
      reader.Advance(count > 1 ? count - 1 : 0);
      return true;
    case rex::xenos::PM4_SET_BIN_SELECT_LO:
      binSelect_ = (binSelect_ & 0xFFFFFFFF00000000ULL) | reader.ReadSwap();
      reader.Advance(count > 1 ? count - 1 : 0);
      return true;
    case rex::xenos::PM4_SET_BIN_SELECT_HI:
      binSelect_ = (binSelect_ & 0x00000000FFFFFFFFULL) | (uint64_t{reader.ReadSwap()} << 32);
      reader.Advance(count > 1 ? count - 1 : 0);
      return true;
    case rex::xenos::PM4_SET_BIN_MASK: {
      const uint32_t hi = reader.ReadSwap();
      const uint32_t lo = reader.ReadSwap();
      binMask_ = (uint64_t{hi} << 32) | lo;
      reader.Advance(count > 2 ? count - 2 : 0);
      return true;
    }
    case rex::xenos::PM4_SET_BIN_SELECT: {
      const uint32_t hi = reader.ReadSwap();
      const uint32_t lo = reader.ReadSwap();
      binSelect_ = (uint64_t{hi} << 32) | lo;
      reader.Advance(count > 2 ? count - 2 : 0);
      return true;
    }

    case rex::xenos::PM4_IM_LOAD: {
      const uint32_t addrType = reader.ReadSwap();
      const uint32_t startSize = reader.ReadSwap();
      reader.Advance(count > 2 ? count - 2 : 0);
      const bool pixel = (addrType & 3u) != 0;
      const uint32_t addr = addrType & ~3u & kPhysicalMask;
      const uint32_t dwords = startSize & 0xFFFFu;
      if (const uint8_t* ptr = resources_->PhysicalPtr(addr, size_t(dwords) * 4u)) {
        shaders_->Loaded(pixel, ptr, dwords);
      }
      return true;
    }

    case rex::xenos::PM4_IM_LOAD_IMMEDIATE: {
      const uint32_t d0 = reader.ReadSwap();
      const uint32_t d1 = reader.ReadSwap();
      const bool pixel = (d0 & 3u) != 0;
      const uint32_t dwords = d1 & 0xFFFFu;
      std::vector<uint32_t> scratch;
      if (const uint8_t* ptr = reader.ContiguousBytes(dwords, scratch)) {
        shaders_->Loaded(pixel, ptr, dwords);
      }
      reader.Advance(count > 2 ? count - 2 : 0);
      return true;
    }

    case rex::xenos::PM4_SET_CONSTANT: {
      const uint32_t offsetType = reader.ReadSwap();
      const uint32_t index = offsetType & 0x7FFu;
      const uint32_t kind = (offsetType >> 16) & 0xFFu;
      uint32_t base = 0;
      if (kind == 0) base = RenderState::VertexConstants;
      else if (kind == 1) base = RenderState::FetchConstants;
      else if (kind == 2) base = RenderState::BoolConstants;
      else if (kind == 3) base = 0x4908u;
      else if (kind == 4) base = 0x2000u;
      else {
        reader.Advance(count - 1);
        return true;
      }
      for (uint32_t i = 0; i < count - 1; ++i) WriteRegister(base + index + i, reader.ReadSwap());
      return true;
    }

    case rex::xenos::PM4_SET_CONSTANT2:
    case rex::xenos::PM4_SET_SHADER_CONSTANTS: {
      const uint32_t offsetType = reader.ReadSwap();
      const uint32_t index = offsetType & 0xFFFFu;
      for (uint32_t i = 0; i < count - 1; ++i) WriteRegister(index + i, reader.ReadSwap());
      return true;
    }

    case rex::xenos::PM4_LOAD_ALU_CONSTANT: {
      const uint32_t addr = reader.ReadSwap() & kPhysicalMask;
      const uint32_t offsetType = reader.ReadSwap();
      const uint32_t sizeDwords = reader.ReadSwap() & 0xFFFu;
      reader.Advance(count > 3 ? count - 3 : 0);
      const uint32_t index = offsetType & 0x7FFu;
      const uint32_t kind = (offsetType >> 16) & 0xFFu;
      uint32_t base = 0;
      if (kind == 0) base = RenderState::VertexConstants;
      else if (kind == 1) base = RenderState::FetchConstants;
      else if (kind == 2) base = RenderState::BoolConstants;
      else if (kind == 3) base = 0x4908u;
      else if (kind == 4) base = 0x2000u;
      else return true;
      if (const uint8_t* ptr = resources_->PhysicalPtr(addr, size_t(sizeDwords) * 4u)) {
        const auto* src = reinterpret_cast<const uint32_t*>(ptr);
        for (uint32_t i = 0; i < sizeDwords; ++i) WriteRegister(base + index + i, ByteSwap32(src[i]));
      }
      return true;
    }

    case rex::xenos::PM4_DRAW_INDX: return ExecuteDrawPacket(reader, count, true);
    case rex::xenos::PM4_DRAW_INDX_2: return ExecuteDrawPacket(reader, count, false);

    default:
      reader.Advance(count);
      return true;
  }
}

}  // namespace bs1::renderer
