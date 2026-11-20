#include "android_custom_driver.h"

#include <android/log.h>
#include <rex/logging/log_category.h>
#include <rex/settings_registry.h>
#include <rex/xenos/format.h>
#include <rex/xenos/packet.h>
#include <rex/xenos/register.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <thread>

#include <sys/resource.h>
#include <unistd.h>

#include "bs1_filelog.h"
#define GLESLOG(...) BS1_GLES_LOG_IMPL(ANDROID_LOG_INFO, __VA_ARGS__)
#define GLESERR(...) BS1_GLES_LOG_IMPL(ANDROID_LOG_ERROR, __VA_ARGS__)

namespace bs1::renderer {

namespace {

constexpr uint32_t kPhysicalMask = 0x1FFFFFFFu;
constexpr uint32_t kSwapSignatureBE = 0x53574150u;
constexpr uint32_t kSwapSignatureLE = 0x50415753u;

double g_profDrawMs = 0.0;
double g_profResolveMs = 0.0;
double g_profPresentMs = 0.0;
double g_profWaitMs = 0.0;
uint32_t g_profDraws = 0;
uint32_t g_profResolves = 0;
uint32_t g_profWaits = 0;
uint32_t g_lastWaitAddr = 0;
uint32_t g_lastWaitVal = 0;
uint32_t g_lastWaitRef = 0;

inline uint32_t ByteSwap32(uint32_t v) { return __builtin_bswap32(v); }

inline uint32_t SwapGpuEndian(uint32_t v, uint32_t endian) {
  return rex::xenos::GPUSwap(v, static_cast<rex::xenos::Endian>(endian & 3u));
}

}  // namespace

uint32_t CustomXenosGraphicsDriver::DwordReader::ReadSwap() {
  if (!base || remainingDwords == 0 || capacityDwords == 0) return 0;
  uint32_t raw = 0;
  std::memcpy(&raw, base + static_cast<size_t>(readIndex) * 4u, 4);
  readIndex = (readIndex + 1u) % capacityDwords;
  --remainingDwords;
  return ByteSwap32(raw);
}

void CustomXenosGraphicsDriver::DwordReader::Advance(uint32_t dwords) {
  const uint32_t skip = std::min(dwords, remainingDwords);
  if (capacityDwords > 0) readIndex = (readIndex + skip) % capacityDwords;
  remainingDwords -= skip;
}

const uint8_t* CustomXenosGraphicsDriver::DwordReader::ContiguousBytes(
    uint32_t dwords, std::vector<uint32_t>& scratch) const {
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

rex::Result<void> CustomXenosGraphicsDriver::Initialize(rex::IDriverContext& context) {
  settingsRegistry_ = &context.GetSettingsRegistry();
  if (auto reg = settingsRegistry_->Register(settings_); !reg) {
    return reg;
  }

  std::filesystem::path cacheDir;
  if (auto found = settingsRegistry_->Find("app.cache_root")) {
    const std::string root = (*found)->GetText();
    if (!root.empty()) {
      cacheDir = std::filesystem::path(root) / "custom-gles3-shaders";
    }
  }
  backend_.SetCacheDir(cacheDir);

  GLESLOG("initialized Android Custom GLES 3.2 Xenos driver (internal_resolution=%s, filter=%s)",
          settings_.internal_resolution.Get().c_str(), settings_.present_filter.Get().c_str());
  return {};
}

void CustomXenosGraphicsDriver::Shutdown() {
  DetachTitle();
  if (settingsRegistry_) {
    settingsRegistry_->Unregister(settings_);
    settingsRegistry_ = nullptr;
  }
}

rex::Result<void> CustomXenosGraphicsDriver::SetSurface(const rex::NativeWindow& window) {
  if (window.type != rex::NativeWindow::Type::kAndroid || !window.window) {
    return rex::Err(rex::ErrorCategory::kRuntime,
                    "Android Custom GLES3 renderer requires an ANativeWindow surface");
  }
  backend_.SetSurfaceWindow(static_cast<ANativeWindow*>(window.window));
  return {};
}

rex::Result<void> CustomXenosGraphicsDriver::AttachTitle(const rex::TitleInfo& info,
                                                         rex::IPhysicalMemory& memory,
                                                         rex::IPhysicalWriteWatch& watch,
                                                         rex::IGuestInterrupts& interrupts) {
  DetachTitle();
  videoMode_ = info.video_mode;
  memory_ = &memory;
  watch_ = &watch;
  interrupts_ = &interrupts;

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
  lastAppliedScale_ = -1.0f;
  lutWritten_ = false;
  lutVersion_ = 0;

  if (auto worker = interrupts.StartHostThread(
          "GPU Commands (Custom GLES3)", [this](std::stop_token stop) { WorkerLoop(stop); })) {
    workerThread_ = std::move(*worker);
  } else {
    return rex::Err(worker.error());
  }

  if (auto vsync = interrupts.StartHostThread(
          "VBlank (Custom GLES3)", [this](std::stop_token stop) { VsyncLoop(stop); })) {
    vsyncThread_ = std::move(*vsync);
  } else {
    workerThread_ = rex::ScopeGuard();
    return rex::Err(vsync.error());
  }

  titleAttached_ = true;
  GLESLOG("attached title %08X (%ux%u @ %.1fHz)", info.title_id, info.video_mode.width,
          info.video_mode.height, info.video_mode.refresh_rate);
  return {};
}

void CustomXenosGraphicsDriver::DetachTitle() {
  if (!titleAttached_) return;
  titleAttached_ = false;
  vsyncThread_ = rex::ScopeGuard();
  {
    std::lock_guard lock(ringMutex_);
    stopping_ = true;
  }
  ringCv_.notify_all();
  workerThread_ = rex::ScopeGuard();
  memory_ = nullptr;
  watch_ = nullptr;
  interrupts_ = nullptr;
}

rex::gpu::FrameStatus CustomXenosGraphicsDriver::WaitForFrame(rex::u32 timeoutMs) {
  // Presentation happens directly on the GPU worker thread on PM4_XE_SWAP.
  if (timeoutMs > 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(std::min<uint32_t>(timeoutMs, 16u)));
  }
  rex::gpu::FrameStatus status{};
  status.new_frame = false;
  return status;
}

void CustomXenosGraphicsDriver::ResizeSurface(rex::u32 width, rex::u32 height) {
  backend_.SetSurfaceSize(width, height);
}

bool CustomXenosGraphicsDriver::BeginPresent() { return false; }
void CustomXenosGraphicsDriver::RenderOverlays(ImDrawData&) {}
void CustomXenosGraphicsDriver::EndPresent() {}

void CustomXenosGraphicsDriver::InitializeRingBuffer(rex::u32 address, rex::u32 sizeLog2) {
  std::lock_guard lock(ringMutex_);
  ringAddress_ = address & kPhysicalMask;
  ringSizeBytes_ = 1u << (sizeLog2 + 3u);
  readIndex_ = 0;
  writeIndex_.store(0, std::memory_order_relaxed);
  ringCv_.notify_all();
}

void CustomXenosGraphicsDriver::EnableReadPointerWriteBack(rex::u32 address, rex::u32) {
  std::lock_guard lock(ringMutex_);
  writebackAddress_ = address & kPhysicalMask;
}

rex::u32 CustomXenosGraphicsDriver::ReadRegister(rex::u32 index) {
  if (const auto fixed = rex::xenos::ReadFixedRegister(index, videoMode_)) {
    return *fixed;
  }
  if (index == static_cast<uint32_t>(rex::xenos::Register::COHER_STATUS_HOST)) {
    SyncCoherency();
  }
  if (index == static_cast<uint32_t>(rex::xenos::Register::CP_RB_RPTR)) {
    return readIndex_;
  }
  if (index == static_cast<uint32_t>(rex::xenos::Register::CP_RB_WPTR)) {
    const uint32_t w = writeIndex_.load(std::memory_order_acquire);
    return w == 0xBAADF00Du ? 0u : w;
  }
  if (index < registers_.size()) return registers_[index];
  return 0;
}

void CustomXenosGraphicsDriver::WriteRegister(rex::u32 index, rex::u32 value) {
  using Reg = rex::xenos::Register;
  if (index == static_cast<uint32_t>(Reg::CP_RB_WPTR)) {
    writeIndex_.store(value, std::memory_order_release);
    ringCv_.notify_all();
    return;
  }
  if (index >= registers_.size()) return;
  registers_[index] = value;

  if (index >= static_cast<uint32_t>(Reg::SCRATCH_REG0) &&
      index <= static_cast<uint32_t>(Reg::SCRATCH_REG7)) {
    const uint32_t slot = index - static_cast<uint32_t>(Reg::SCRATCH_REG0);
    if ((registers_[static_cast<uint32_t>(Reg::SCRATCH_UMSK)] & (1u << slot)) != 0) {
      const uint32_t addr = registers_[static_cast<uint32_t>(Reg::SCRATCH_ADDR)] + slot * 4u;
      StorePhysical32Host(addr, value);
    }
  } else if (index == static_cast<uint32_t>(Reg::COHER_STATUS_HOST)) {
    registers_[index] |= 0x80000000u;
  } else {
    HandleLutWrite(index, value);
  }
}

void CustomXenosGraphicsDriver::ApplyLiveSettings() {
  const uint32_t baseH = backend_.ObservedMainHeight();
  const float scale = ComputeEffectiveScale(settings_, baseH);
  backend_.Resources().RequestScale(scale);
  const uint32_t aniso = static_cast<uint32_t>(std::clamp(settings_.anisotropy.Get(), 1, 16));
  backend_.Resources().SetAnisotropy(aniso);

  if (std::fabs(scale - lastAppliedScale_) > 0.01f) {
    lastAppliedScale_ = scale;
    GLESLOG("live internal resolution -> preset=%s scale=%.2fx (~%up over %up guest)",
            settings_.internal_resolution.Get().c_str(), scale,
            static_cast<uint32_t>(std::lround(baseH * scale)), baseH);
  }
}

void CustomXenosGraphicsDriver::VsyncLoop(std::stop_token stop) {
  const double hz =
      (videoMode_.refresh_rate >= 24.0f && videoMode_.refresh_rate <= 240.0f)
          ? static_cast<double>(videoMode_.refresh_rate)
          : 60.0;
  const auto period = std::chrono::microseconds(static_cast<int64_t>(1000000.0 / hz));
  auto next = std::chrono::steady_clock::now();
  while (!stop.stop_requested()) {
    next += period;
    const auto now = std::chrono::steady_clock::now();
    if (next > now) {
      std::this_thread::sleep_until(next);
    } else {
      next = now;
    }
    if (stop.stop_requested()) break;
    counter_.fetch_add(1u, std::memory_order_relaxed);
    if (interrupts_) {
      (void)interrupts_->RaiseGraphicsInterrupt(0u, 2u);
    }
  }
}

void CustomXenosGraphicsDriver::HandleLutWrite(uint32_t index, uint32_t value) {
  using Reg = rex::xenos::Register;
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

void CustomXenosGraphicsDriver::SyncCoherency() {
  registers_[static_cast<uint32_t>(rex::xenos::Register::COHER_STATUS_HOST)] = 0;
}

void CustomXenosGraphicsDriver::StorePhysical32Host(uint32_t physAddr, uint32_t value) {
  if (!memory_) return;
  (void)memory_->StoreRelease32(physAddr & kPhysicalMask, value);
}

void CustomXenosGraphicsDriver::StorePhysical32GpuSwap(uint32_t physAddr, uint32_t value,
                                                       uint32_t endian) {
  StorePhysical32Host(physAddr, ByteSwap32(SwapGpuEndian(value, endian)));
}

uint32_t CustomXenosGraphicsDriver::LoadPhysical32GpuSwap(uint32_t physAddr, uint32_t endian) {
  if (!memory_) return 0;
  auto loaded = memory_->LoadAcquire32(physAddr & kPhysicalMask);
  if (!loaded) return 0;
  return SwapGpuEndian(ByteSwap32(*loaded), endian);
}

void CustomXenosGraphicsDriver::LoadShaderMicrocode(bool pixel, const uint8_t* bytes,
                                                    uint32_t dwords) {
  if (!bytes || dwords == 0 || dwords > 65536u) return;
  shaderWordScratch_.resize(dwords);
  for (uint32_t i = 0; i < dwords; ++i) {
    shaderWordScratch_[i] = (static_cast<uint32_t>(bytes[i * 4 + 0]) << 24) |
                            (static_cast<uint32_t>(bytes[i * 4 + 1]) << 16) |
                            (static_cast<uint32_t>(bytes[i * 4 + 2]) << 8) | bytes[i * 4 + 3];
  }
  const auto h = backend_.Shaders().Intern(shaderWordScratch_.data(), dwords, pixel);
  backend_.Shaders().Select(h, pixel);
}

void CustomXenosGraphicsDriver::WorkerLoop(std::stop_token stop) {
  if (!backend_.EnsureInitialized(memory_, watch_)) {
    GLESERR("Gles3Backend failed to initialize on GPU worker thread!");
    return;
  }

  auto lastReport = std::chrono::steady_clock::now();
  uint64_t lastFrameIdx = 0;
  double accExecMs = 0.0;
  uint32_t accBatches = 0;

  while (!stop.stop_requested()) {
    uint32_t wptr = 0;
    uint32_t ringSize = 0;
    {
      std::unique_lock lock(ringMutex_);
      ringCv_.wait_for(lock, std::chrono::milliseconds(8), [&]() {
        if (stopping_ || stop.stop_requested()) return true;
        const uint32_t w = writeIndex_.load(std::memory_order_acquire);
        return ringSizeBytes_ > 0 && w != 0xBAADF00Du && w != readIndex_;
      });
      if (stopping_ || stop.stop_requested()) break;
      wptr = writeIndex_.load(std::memory_order_acquire);
      ringSize = ringSizeBytes_;
    }

    backend_.UpdatePendingSurface();
    if (ringSize != 0 && wptr != 0xBAADF00Du && wptr != readIndex_) {
      const auto t0 = std::chrono::steady_clock::now();
      readIndex_ = ExecutePrimaryBuffer(readIndex_, wptr, stop);
      registers_[static_cast<uint32_t>(rex::xenos::Register::CP_RB_RPTR)] = readIndex_;
      if (writebackAddress_ != 0) {
        StorePhysical32Host(writebackAddress_, readIndex_);
      }
      const auto t1 = std::chrono::steady_clock::now();
      accExecMs += std::chrono::duration<double, std::milli>(t1 - t0).count();
      ++accBatches;
    }

    const auto now = std::chrono::steady_clock::now();
    if (now - lastReport >= std::chrono::seconds(10)) {
      const uint64_t curFrame = frameIndex_;
      const auto rs = backend_.GetResolveStats();
      GLESLOG(
          "PERF(10s): swaps=%llu batches=%u exec=%.1fms (draw=%.1f/%u res=%.1f/%u pres=%.1f wait=%.1f/%u lastWait=0x%X:%X/%X) "
          "resolves=(color=%u depth=%u direct=%u skip=%u rej[s=%u c=%u b=%u] px=%llu) shaders=%u/%u",
          static_cast<unsigned long long>(curFrame - lastFrameIdx), accBatches, accExecMs,
          g_profDrawMs, g_profDraws, g_profResolveMs, g_profResolves, g_profPresentMs,
          g_profWaitMs, g_profWaits, g_lastWaitAddr, g_lastWaitVal, g_lastWaitRef,
          rs.color, rs.depth, rs.directCopy, rs.skipped, rs.rejectScale, rs.rejectClamp,
          rs.rejectBounds, static_cast<unsigned long long>(rs.pixels),
          backend_.Shaders().CompiledProgramCount(), backend_.Shaders().BinaryCacheHits());
      backend_.ResetResolveStats();
      lastFrameIdx = curFrame;
      accExecMs = 0.0;
      accBatches = 0;
      g_profDrawMs = g_profResolveMs = g_profPresentMs = g_profWaitMs = 0.0;
      g_profDraws = g_profResolves = g_profWaits = 0;
      lastReport = now;
    }
  }

  backend_.ShutdownOnGpuThread();
}

uint32_t CustomXenosGraphicsDriver::ExecutePrimaryBuffer(uint32_t readIndex, uint32_t writeIndex,
                                                         std::stop_token stop) {
  if (!memory_ || ringSizeBytes_ == 0) return writeIndex;
  auto span = memory_->GetReadOnlyPhysicalSpan({ringAddress_, ringSizeBytes_});
  if (!span) return writeIndex;
  const uint32_t cap = ringSizeBytes_ / 4u;
  const uint32_t r = readIndex % cap;
  const uint32_t w = writeIndex % cap;
  const uint32_t count = (w >= r) ? (w - r) : (cap - r + w);
  DwordReader reader;
  reader.base = reinterpret_cast<const uint8_t*>(span->data());
  reader.capacityDwords = cap;
  reader.readIndex = r;
  reader.remainingDwords = count;
  while (!reader.Empty() && !stop.stop_requested()) {
    if (!ExecutePacket(reader, stop)) break;
  }
  return writeIndex;
}

void CustomXenosGraphicsDriver::ExecuteIndirectBuffer(uint32_t physAddr, uint32_t dwordCount,
                                                      std::stop_token stop) {
  if (!memory_ || dwordCount == 0) return;
  auto span = memory_->GetReadOnlyPhysicalSpan({physAddr & kPhysicalMask, dwordCount * 4u});
  if (!span) return;
  DwordReader reader;
  reader.base = reinterpret_cast<const uint8_t*>(span->data());
  reader.capacityDwords = dwordCount;
  reader.readIndex = 0;
  reader.remainingDwords = dwordCount;
  while (!reader.Empty() && !stop.stop_requested()) {
    if (!ExecutePacket(reader, stop)) break;
  }
}

bool CustomXenosGraphicsDriver::ExecutePacket(DwordReader& reader, std::stop_token stop) {
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

bool CustomXenosGraphicsDriver::ExecuteType0(DwordReader& reader, uint32_t packet) {
  const uint32_t count = ((packet >> 16) & 0x3FFFu) + 1u;
  if (reader.remainingDwords < count) return false;
  const uint32_t baseIndex = packet & 0x7FFFu;
  const bool oneReg = ((packet >> 15) & 1u) != 0;
  for (uint32_t i = 0; i < count; ++i) {
    WriteRegister(oneReg ? baseIndex : (baseIndex + i), reader.ReadSwap());
  }
  return true;
}

bool CustomXenosGraphicsDriver::ExecuteType1(DwordReader& reader, uint32_t packet) {
  if (reader.remainingDwords < 2) return false;
  const uint32_t r1 = packet & 0x7FFu;
  const uint32_t r2 = (packet >> 11) & 0x7FFu;
  const uint32_t d1 = reader.ReadSwap();
  const uint32_t d2 = reader.ReadSwap();
  WriteRegister(r1, d1);
  WriteRegister(r2, d2);
  return true;
}

bool CustomXenosGraphicsDriver::ExecuteDrawPacket(DwordReader& reader, uint32_t countRemaining,
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
  if ((viz & 1u) != 0 && (viz & 0x100u) != 0) return true;

  if (!frameStarted_) {
    frameStarted_ = true;
    ApplyLiveSettings();
    backend_.BeginFrame(frameIndex_);
  }

  const uint32_t mode = registers_[0x2208u] & 7u;
  if (mode == 6u) {
    const auto t0 = std::chrono::steady_clock::now();
    backend_.ExecuteResolve(registers_.data());
    const auto t1 = std::chrono::steady_clock::now();
    g_profResolveMs += std::chrono::duration<double, std::milli>(t1 - t0).count();
    ++g_profResolves;
  } else if (mode != 0u) {
    const auto t0 = std::chrono::steady_clock::now();
    backend_.ExecuteDraw(registers_.data(), initiator, indexBase, memory_);
    const auto t1 = std::chrono::steady_clock::now();
    g_profDrawMs += std::chrono::duration<double, std::milli>(t1 - t0).count();
    ++g_profDraws;
  }
  return true;
}

bool CustomXenosGraphicsDriver::ExecuteType3(DwordReader& reader, uint32_t packet,
                                             std::stop_token stop) {
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
        const auto t0 = std::chrono::steady_clock::now();
        backend_.ObserveSwapHeight(fbWidth, fbHeight);
        const bool useLanczos = (settings_.present_filter.Get() == "lanczos");
        backend_.ExecutePresent(fbAddr, fbWidth, fbHeight, useLanczos,
                                settings_.letterbox.Get(), settings_.vsync.Get(),
                                gammaLut_, lutVersion_, lutWritten_);
        const auto t1 = std::chrono::steady_clock::now();
        g_profPresentMs += std::chrono::duration<double, std::milli>(t1 - t0).count();
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
      const auto t0 = std::chrono::steady_clock::now();
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
        uint32_t val = isMem ? LoadPhysical32GpuSwap(pollAddr & ~3u, pollAddr & 3u)
                             : ReadRegister(pollAddr);
        const uint32_t m = val & mask;
        bool matched = false;
        switch (fn) {
          case 1: matched = m < ref; break;
          case 2: matched = m <= ref; break;
          case 3: matched = m == ref; break;
          case 4: matched = m != ref; break;
          case 5: matched = m >= ref; break;
          case 6: matched = m > ref; break;
          case 7: matched = true; break;
        }
        if (matched) break;
        if (attempts == 32) {
          g_lastWaitAddr = pollAddr | (isMem ? 0x80000000u : 0u);
          g_lastWaitVal = val;
          g_lastWaitRef = ref;
        }
        if (attempts++ < 16) std::this_thread::yield();
        else std::this_thread::sleep_for(std::chrono::microseconds(wait >= 0x100 ? 500 : 100));
      }
      const auto t1 = std::chrono::steady_clock::now();
      g_profWaitMs += std::chrono::duration<double, std::milli>(t1 - t0).count();
      ++g_profWaits;
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
        StorePhysical32GpuSwap(writeAddr & ~3u, reader.ReadSwap(), writeAddr & 3u);
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
      const uint32_t val = (info & 0x10u) ? LoadPhysical32GpuSwap(pollAddr & ~3u, pollAddr & 3u)
                                          : ReadRegister(pollAddr);
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
          for (int i = 0; i < 6; ++i) dst[i] = static_cast<uint16_t>((extents[i] >> 8) | (extents[i] << 8));
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
      if (memory_ && dwords > 0) {
        if (auto span = memory_->GetReadOnlyPhysicalSpan({addr, dwords * 4u})) {
          LoadShaderMicrocode(pixel, reinterpret_cast<const uint8_t*>(span->data()), dwords);
        }
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
        LoadShaderMicrocode(pixel, ptr, dwords);
      }
      reader.Advance(count > 2 ? count - 2 : 0);
      return true;
    }

    case rex::xenos::PM4_SET_CONSTANT: {
      const uint32_t offsetType = reader.ReadSwap();
      const uint32_t index = offsetType & 0x7FFu;
      const uint32_t kind = (offsetType >> 16) & 0xFFu;
      uint32_t base = 0;
      if (kind == 0) base = 0x4000u;
      else if (kind == 1) base = 0x4800u;
      else if (kind == 2) base = 0x4900u;
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
      if (kind == 0) base = 0x4000u;
      else if (kind == 1) base = 0x4800u;
      else if (kind == 2) base = 0x4900u;
      else if (kind == 3) base = 0x4908u;
      else if (kind == 4) base = 0x2000u;
      else return true;
      if (memory_ && sizeDwords > 0) {
        if (auto span = memory_->GetReadOnlyPhysicalSpan({addr, sizeDwords * 4u})) {
          const auto* src = reinterpret_cast<const uint32_t*>(span->data());
          for (uint32_t i = 0; i < sizeDwords; ++i) WriteRegister(base + index + i, ByteSwap32(src[i]));
        }
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
