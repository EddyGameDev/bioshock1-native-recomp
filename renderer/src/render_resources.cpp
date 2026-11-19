#include "render_resources.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <rex/bit.h>
#include <rex/logging/log_category.h>
#include <rex/xenos/format.h>
#include <rex/xenos/texture.h>

namespace bs1::renderer {

namespace {

constexpr uint64_t kUnusedFrames = 1800;

inline uint32_t ByteSwap32(uint32_t v) {
#if defined(__GNUC__) || defined(__clang__)
  return __builtin_bswap32(v);
#else
  return _byteswap_ulong(v);
#endif
}

inline uint16_t ByteSwap16(uint16_t v) {
  return static_cast<uint16_t>((v << 8) | (v >> 8));
}

inline uint32_t ApplyEndian32(uint32_t word, uint32_t endian) {
  switch (endian) {
    case 1: return ((word & 0xFF00FF00u) >> 8) | ((word & 0x00FF00FFu) << 8);
    case 2: return ByteSwap32(word);
    case 3: return (word >> 16) | (word << 16);
    default: return word;
  }
}

uint64_t Fingerprint(const uint8_t* data, size_t bytes) {
  if (!data || bytes == 0) return 0;
  constexpr uint64_t kFnv = 1099511628211ULL;
  uint64_t hash = 1469598103934665603ULL ^ bytes;
  if (bytes <= 16384) {
    size_t at = 0;
    for (; at + 8 <= bytes; at += 8) {
      uint64_t w = 0;
      std::memcpy(&w, data + at, 8);
      hash = (hash ^ w) * kFnv;
    }
    if (at < bytes) {
      uint64_t w = 0;
      std::memcpy(&w, data + at, bytes - at);
      hash = (hash ^ w) * kFnv;
    }
    return hash;
  }
  const size_t step = std::max<size_t>(bytes / 4096, 16);
  for (size_t at = 0; at + 8 <= bytes; at += step) {
    uint64_t w = 0;
    std::memcpy(&w, data + at, 8);
    hash = (hash ^ w) * kFnv;
  }
  uint64_t tail = 0;
  std::memcpy(&tail, data + bytes - 8, 8);
  return (hash ^ tail) * kFnv;
}

DXGI_FORMAT ColorFormat(uint32_t format, uint32_t& bytesPerSample) {
  bytesPerSample = 4;
  switch (format) {
    case 0:
    case 1: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case 2:
    case 10: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case 3:
    case 12:
      // k_2_10_10_10_FLOAT uses 32 bits per sample in EDRAM (not 64bpp).
      return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case 4: return DXGI_FORMAT_R16G16_UNORM;
    case 5:
      bytesPerSample = 8;
      return DXGI_FORMAT_R16G16B16A16_UNORM;
    case 6: return DXGI_FORMAT_R16G16_FLOAT;
    case 7:
      bytesPerSample = 8;
      return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case 14: return DXGI_FORMAT_R32_FLOAT;
    case 15:
      bytesPerSample = 8;
      return DXGI_FORMAT_R32G32_FLOAT;
    default: return DXGI_FORMAT_R8G8B8A8_UNORM;
  }
}

uint32_t PitchOf(uint32_t surfaceInfo) { return surfaceInfo & 0x3FFF; }
uint32_t ClassOf(uint32_t surfaceInfo) { return ((surfaceInfo >> 16) & 3) != 0 ? 1u : 0u; }

struct TexFormatDesc {
  DXGI_FORMAT dxgi = DXGI_FORMAT_UNKNOWN;
  uint32_t blockBytes = 0;
  uint32_t blockTexels = 1;
  bool compressed = false;
  bool expand16To32 = false;
  bool expand565To32 = false;
  bool expand4444To32 = false;
  bool expand1555To32 = false;
  bool supported = false;
};

TexFormatDesc DescribeTextureFormat(uint32_t format) {
  TexFormatDesc d{};
  switch (format) {
    case 2:   // k_8
    case 8:   // k_8_A
    case 9:   // k_8_B
      d.dxgi = DXGI_FORMAT_R8_UNORM;
      d.blockBytes = 1;
      d.supported = true;
      break;
    case 3:  // k_1_5_5_5
      d.dxgi = DXGI_FORMAT_R8G8B8A8_UNORM;
      d.blockBytes = 2;
      d.expand1555To32 = true;
      d.supported = true;
      break;
    case 4:  // k_5_6_5
      d.dxgi = DXGI_FORMAT_R8G8B8A8_UNORM;
      d.blockBytes = 2;
      d.expand565To32 = true;
      d.supported = true;
      break;
    case 6:   // k_8_8_8_8
    case 14:  // k_8_8_8_8_A
    case 50:  // k_8_8_8_8_AS_16_16_16_16
    case 62:  // k_8_8_8_8_GAMMA_EDRAM
      d.dxgi = DXGI_FORMAT_R8G8B8A8_UNORM;
      d.blockBytes = 4;
      d.supported = true;
      break;
    case 7:   // k_2_10_10_10
    case 54:  // k_2_10_10_10_AS_16_16_16_16
      d.dxgi = DXGI_FORMAT_R10G10B10A2_UNORM;
      d.blockBytes = 4;
      d.supported = true;
      break;
    case 10:  // k_8_8
      d.dxgi = DXGI_FORMAT_R8G8_UNORM;
      d.blockBytes = 2;
      d.supported = true;
      break;
    case 15:  // k_4_4_4_4
      d.dxgi = DXGI_FORMAT_R8G8B8A8_UNORM;
      d.blockBytes = 2;
      d.expand4444To32 = true;
      d.supported = true;
      break;
    case 18:  // k_DXT1
    case 51:  // k_DXT1_AS_16_16_16_16
      d.dxgi = DXGI_FORMAT_BC1_UNORM;
      d.blockBytes = 8;
      d.blockTexels = 4;
      d.compressed = true;
      d.supported = true;
      break;
    case 19:  // k_DXT2_3
    case 52:  // k_DXT2_3_AS_16_16_16_16
      d.dxgi = DXGI_FORMAT_BC2_UNORM;
      d.blockBytes = 16;
      d.blockTexels = 4;
      d.compressed = true;
      d.supported = true;
      break;
    case 20:  // k_DXT4_5
    case 53:  // k_DXT4_5_AS_16_16_16_16
      d.dxgi = DXGI_FORMAT_BC3_UNORM;
      d.blockBytes = 16;
      d.blockTexels = 4;
      d.compressed = true;
      d.supported = true;
      break;
    case 24:  // k_16
      d.dxgi = DXGI_FORMAT_R16_UNORM;
      d.blockBytes = 2;
      d.supported = true;
      break;
    case 25:  // k_16_16
      d.dxgi = DXGI_FORMAT_R16G16_UNORM;
      d.blockBytes = 4;
      d.supported = true;
      break;
    case 26:  // k_16_16_16_16
      d.dxgi = DXGI_FORMAT_R16G16B16A16_UNORM;
      d.blockBytes = 8;
      d.supported = true;
      break;
    case 29:  // k_16_16_16_16_EXPAND
    case 32:  // k_16_16_16_16_FLOAT
      d.dxgi = DXGI_FORMAT_R16G16B16A16_FLOAT;
      d.blockBytes = 8;
      d.supported = true;
      break;
    case 28:  // k_16_16_EXPAND
    case 31:  // k_16_16_FLOAT
      d.dxgi = DXGI_FORMAT_R16G16_FLOAT;
      d.blockBytes = 4;
      d.supported = true;
      break;
    case 27:  // k_16_EXPAND
    case 30:  // k_16_FLOAT
      d.dxgi = DXGI_FORMAT_R16_FLOAT;
      d.blockBytes = 2;
      d.supported = true;
      break;
    case 36:  // k_32_FLOAT
      d.dxgi = DXGI_FORMAT_R32_FLOAT;
      d.blockBytes = 4;
      d.supported = true;
      break;
    case 37:  // k_32_32_FLOAT
      d.dxgi = DXGI_FORMAT_R32G32_FLOAT;
      d.blockBytes = 8;
      d.supported = true;
      break;
    case 38:  // k_32_32_32_32_FLOAT
      d.dxgi = DXGI_FORMAT_R32G32B32A32_FLOAT;
      d.blockBytes = 16;
      d.supported = true;
      break;
    case 49:  // k_DXN (BC5)
      d.dxgi = DXGI_FORMAT_BC5_UNORM;
      d.blockBytes = 16;
      d.blockTexels = 4;
      d.compressed = true;
      d.supported = true;
      break;
    case 59:  // k_DXT5A (BC4)
      d.dxgi = DXGI_FORMAT_BC4_UNORM;
      d.blockBytes = 8;
      d.blockTexels = 4;
      d.compressed = true;
      d.supported = true;
      break;
    default:
      break;
  }
  return d;
}

void PackedBaseOffset(bool packed, uint32_t width, uint32_t height, uint32_t& x, uint32_t& y) {
  x = 0;
  y = 0;
  if (!packed) return;
  uint32_t log2W = 0;
  uint32_t log2H = 0;
  while ((1u << log2W) < width) ++log2W;
  while ((1u << log2H) < height) ++log2H;
  if (std::min(log2W, log2H) > 4) return;
  if (log2W > log2H) {
    y = 16;
  } else {
    x = 16;
  }
}

inline uint32_t TiledOffset2D(uint32_t x, uint32_t y, uint32_t pitchAligned, uint32_t bpbLog2) {
  const uint32_t macro = ((x >> 5) + (y >> 5) * (pitchAligned >> 5)) << (bpbLog2 + 7);
  const uint32_t micro = ((x & 7u) + ((y & 0xEu) << 2)) << bpbLog2;
  const uint32_t offset = macro + ((micro & ~0xFu) << 1) + (micro & 0xFu) + ((y & 1u) << 4);
  return ((offset & ~0x1FFu) << 3) + ((y & 16u) << 7) + ((offset & 0x1C0u) << 2) +
         (((((y & 8u) >> 2) + (x >> 3)) & 3u) << 6) + (offset & 0x3Fu);
}

inline void Swap16Bytes8(uint8_t* dst, const uint8_t* src) {
  uint64_t v = 0;
  std::memcpy(&v, src, 8);
  v = ((v & 0x00FF00FF00FF00FFULL) << 8) | ((v >> 8) & 0x00FF00FF00FF00FFULL);
  std::memcpy(dst, &v, 8);
}

size_t LinearLevelSize(const TexFormatDesc& fmt, uint32_t width, uint32_t height,
                       uint32_t& rowBytes) {
  if (fmt.compressed) {
    const uint32_t bw = (width + 3u) / 4u;
    const uint32_t bh = (height + 3u) / 4u;
    rowBytes = bw * fmt.blockBytes;
    return static_cast<size_t>(rowBytes) * bh;
  }
  const bool expand32 = fmt.expand565To32 || fmt.expand4444To32 || fmt.expand1555To32;
  const uint32_t outBpp = expand32 ? 4u : fmt.blockBytes;
  rowBytes = width * outBpp;
  return static_cast<size_t>(rowBytes) * height;
}

void LinearLevelInto(const uint8_t* data, size_t maxBytes, const TexFormatDesc& fmt,
                     uint32_t endian, uint32_t width, uint32_t height, uint32_t pitchTexels,
                     bool tiled, uint32_t x0, uint32_t y0, uint8_t* outPtr, uint32_t& rowBytes) {
  if (fmt.compressed) {
    const uint32_t bw = (width + 3u) / 4u;
    const uint32_t bh = (height + 3u) / 4u;
    const uint32_t pitchBlocks = std::max(pitchTexels / 4u, 1u);
    const uint32_t pitchAligned = (pitchBlocks + 31u) & ~31u;
    const uint32_t bpbLog2 = fmt.blockBytes == 8u ? 3u : 4u;
    const uint32_t bx0 = x0 / 4u;
    const uint32_t by0 = y0 / 4u;
    rowBytes = bw * fmt.blockBytes;
    const size_t totalBytes = static_cast<size_t>(rowBytes) * bh;
    if (!tiled) {
      for (uint32_t by = 0; by < bh; ++by) {
        const size_t rowSrcOff =
            static_cast<size_t>((by + by0) * pitchBlocks + bx0) * fmt.blockBytes;
        uint8_t* dstRow = outPtr + static_cast<size_t>(by) * rowBytes;
        if (rowSrcOff + rowBytes > maxBytes) {
          std::memset(dstRow, 0, rowBytes);
          for (uint32_t bx = 0; bx < bw; ++bx) {
            const size_t off = rowSrcOff + static_cast<size_t>(bx) * fmt.blockBytes;
            if (off + fmt.blockBytes > maxBytes) break;
            uint8_t* dst = dstRow + static_cast<size_t>(bx) * fmt.blockBytes;
            const uint8_t* src = data + off;
            Swap16Bytes8(dst, src);
            if (fmt.blockBytes == 16u) Swap16Bytes8(dst + 8, src + 8);
          }
          continue;
        }
        const uint8_t* src = data + rowSrcOff;
        for (uint32_t i = 0; i < rowBytes; i += 8u) {
          Swap16Bytes8(dstRow + i, src + i);
        }
      }
      return;
    }

    // Fast tiled path with inlined Xenos 2D tile address calculation and 64-bit word swaps.
    std::memset(outPtr, 0, totalBytes);
    for (uint32_t by = 0; by < bh; ++by) {
      const uint32_t ty = by + by0;
      uint8_t* dstRow = outPtr + static_cast<size_t>(by) * rowBytes;
      for (uint32_t bx = 0; bx < bw; ++bx) {
        const uint32_t off = TiledOffset2D(bx + bx0, ty, pitchAligned, bpbLog2);
        if (static_cast<size_t>(off) + fmt.blockBytes > maxBytes) continue;
        uint8_t* dst = dstRow + static_cast<size_t>(bx) * fmt.blockBytes;
        const uint8_t* src = data + off;
        Swap16Bytes8(dst, src);
        if (fmt.blockBytes == 16u) Swap16Bytes8(dst + 8, src + 8);
      }
    }
    return;
  }

  const uint32_t bpp = fmt.blockBytes;
  const uint32_t bppLog2 = bpp == 16 ? 4u : bpp == 8 ? 3u : bpp == 4 ? 2u : bpp == 2 ? 1u : 0u;
  const bool expand32 = fmt.expand565To32 || fmt.expand4444To32 || fmt.expand1555To32;
  const uint32_t outBpp = expand32 ? 4u : bpp;
  rowBytes = width * outBpp;
  std::memset(outPtr, 0, static_cast<size_t>(rowBytes) * height);
  const uint32_t pitchAligned = (std::max(pitchTexels, 1u) + 31u) & ~31u;

  for (uint32_t y = 0; y < height; ++y) {
    const uint32_t ty = y + y0;
    uint8_t* dstRow = outPtr + static_cast<size_t>(y) * rowBytes;
    for (uint32_t x = 0; x < width; ++x) {
      const uint32_t off = tiled ? TiledOffset2D(x + x0, ty, pitchAligned, bppLog2)
                                 : (ty * pitchTexels + x + x0) * bpp;
      if (static_cast<size_t>(off) + bpp > maxBytes) continue;
      uint8_t* dst = dstRow + static_cast<size_t>(x) * outBpp;
      const uint8_t* src = data + off;

      if (bpp == 1) {
        dst[0] = src[0];
      } else if (bpp == 2) {
        uint16_t raw = 0;
        std::memcpy(&raw, src, 2);
        if (endian == 1 || endian == 2) raw = ByteSwap16(raw);
        if (fmt.expand565To32) {
          const uint8_t b = static_cast<uint8_t>(((raw & 0x1F) * 255 + 15) / 31);
          const uint8_t g = static_cast<uint8_t>((((raw >> 5) & 0x3F) * 255 + 31) / 63);
          const uint8_t r = static_cast<uint8_t>((((raw >> 11) & 0x1F) * 255 + 15) / 31);
          dst[0] = r;
          dst[1] = g;
          dst[2] = b;
          dst[3] = 255;
        } else if (fmt.expand4444To32) {
          dst[0] = static_cast<uint8_t>(((raw >> 8) & 0xF) * 17);
          dst[1] = static_cast<uint8_t>(((raw >> 4) & 0xF) * 17);
          dst[2] = static_cast<uint8_t>((raw & 0xF) * 17);
          dst[3] = static_cast<uint8_t>(((raw >> 12) & 0xF) * 17);
        } else if (fmt.expand1555To32) {
          dst[0] = static_cast<uint8_t>((((raw >> 10) & 0x1F) * 255 + 15) / 31);
          dst[1] = static_cast<uint8_t>((((raw >> 5) & 0x1F) * 255 + 15) / 31);
          dst[2] = static_cast<uint8_t>(((raw & 0x1F) * 255 + 15) / 31);
          dst[3] = (raw & 0x8000) ? 255 : 0;
        } else {
          std::memcpy(dst, &raw, 2);
        }
      } else if (bpp == 4) {
        uint32_t w = 0;
        std::memcpy(&w, src, 4);
        w = ApplyEndian32(w, endian);
        std::memcpy(dst, &w, 4);
      } else {
        for (uint32_t i = 0; i < bpp; i += 4) {
          uint32_t w = 0;
          std::memcpy(&w, src + i, 4);
          w = ApplyEndian32(w, endian);
          std::memcpy(dst + i, &w, 4);
        }
      }
    }
  }
}

}  // namespace

bool RenderResources::Ring::Make(ID3D11Device* dev, uint32_t size, UINT bind) {
  D3D11_BUFFER_DESC desc{};
  desc.ByteWidth = size;
  desc.Usage = D3D11_USAGE_DYNAMIC;
  desc.BindFlags = bind;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  if (FAILED(dev->CreateBuffer(&desc, nullptr, &buffer))) return false;
  bytes = size;
  offset = 0;
  return true;
}

uint8_t* RenderResources::Ring::Map(ID3D11DeviceContext* ctx, uint32_t size, uint32_t alignment,
                                    uint32_t& outOffset) {
  if (!buffer || size > bytes) return nullptr;
  const uint32_t aligned = (offset + alignment - 1) & ~(alignment - 1);
  D3D11_MAP mode = D3D11_MAP_WRITE_NO_OVERWRITE;
  if (aligned + size > bytes) {
    offset = 0;
    mode = D3D11_MAP_WRITE_DISCARD;
  } else {
    offset = aligned;
  }
  D3D11_MAPPED_SUBRESOURCE mapped{};
  if (FAILED(ctx->Map(buffer.Get(), 0, mode, 0, &mapped))) return nullptr;
  outOffset = offset;
  offset += size;
  return static_cast<uint8_t*>(mapped.pData) + outOffset;
}

void RenderResources::Ring::Unmap(ID3D11DeviceContext* ctx) {
  if (buffer) ctx->Unmap(buffer.Get(), 0);
}

uint32_t RenderResources::Ring::Append(ID3D11DeviceContext* ctx, const void* data, uint32_t size,
                                       uint32_t alignment) {
  uint32_t outOffset = 0;
  uint8_t* dst = Map(ctx, size, alignment, outOffset);
  if (!dst) return 0;
  std::memcpy(dst, data, size);
  Unmap(ctx);
  return outOffset;
}

void RenderResources::Initialize(ID3D11Device* device, ID3D11DeviceContext* context,
                                 rex::IPhysicalMemory* memory, rex::IPhysicalWriteWatch* watch) {
  Shutdown();
  device_ = device;
  context_ = context;
  memory_ = memory;
  watch_ = watch;
  physBase_ = nullptr;
  physSize_ = 0;

  if (memory_) {
    if (auto span = memory_->GetReadOnlyPhysicalSpan({0, 512u << 20})) {
      physBase_ = reinterpret_cast<const uint8_t*>(span->data());
      physSize_ = span->size();
    }
  }
  pageSeq_ = std::make_unique<std::atomic<uint32_t>[]>(kPhysPages);
  chunkSeq_ = std::make_unique<std::atomic<uint32_t>[]>(kPhysChunks);
  writeSeq_.store(1u, std::memory_order_relaxed);
  if (watch_) {
    watchGuard_ = watch_->AddWriteListener(*this);
  }

  std::memset(rowsAsked_, 0, sizeof(rowsAsked_));
  indexRing_.Make(device_, 8u << 20, D3D11_BIND_INDEX_BUFFER);
  constantRing_.Make(device_, 8u << 20, D3D11_BIND_CONSTANT_BUFFER);

  // Handle 1 = default 1x1 white fallback texture.
  textures_.emplace_back();
  TextureEntry& white = textures_.back();
  white.live = true;
  white.width = 1;
  white.height = 1;
  const uint32_t whitePixel = 0xFFFFFFFFu;
  D3D11_TEXTURE2D_DESC td{};
  td.Width = 1;
  td.Height = 1;
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_IMMUTABLE;
  td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  const D3D11_SUBRESOURCE_DATA init{&whitePixel, 4, 0};
  if (SUCCEEDED(device_->CreateTexture2D(&td, &init, &white.texture))) {
    device_->CreateShaderResourceView(white.texture.Get(), nullptr, &white.resource);
  }

  td.ArraySize = 6;
  td.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;
  const D3D11_SUBRESOURCE_DATA cubeInit[6] = {
      {&whitePixel, 4, 0}, {&whitePixel, 4, 0}, {&whitePixel, 4, 0},
      {&whitePixel, 4, 0}, {&whitePixel, 4, 0}, {&whitePixel, 4, 0},
  };
  if (SUCCEEDED(device_->CreateTexture2D(&td, cubeInit, &whiteCube_))) {
    device_->CreateShaderResourceView(whiteCube_.Get(), nullptr, &whiteCubeView_);
  }
}

void RenderResources::Shutdown() {
  watchGuard_ = rex::ScopeGuard();
  watch_ = nullptr;
  memory_ = nullptr;
  physBase_ = nullptr;
  physSize_ = 0;

  colorTargets_.clear();
  depthTargets_.clear();
  colorKeys_.Clear();
  depthKeys_.Clear();
  resolved_.clear();
  resolvedKeys_.Clear();
  textures_.clear();
  textureKeys_.Clear();
  freeTextures_.clear();
  whiteCube_.Reset();
  whiteCubeView_.Reset();
  buffers_.clear();
  bufferKeys_.Clear();
  freeBuffers_.clear();
  indexEntries_.clear();
  indexKeys_.Clear();
  freeIndexEntries_.clear();
  indexRing_.Reset();
  constantRing_.Reset();

  pageSeq_.reset();
  chunkSeq_.reset();
  device_ = nullptr;
  context_ = nullptr;
}

const uint8_t* RenderResources::PhysicalPtr(uint32_t physical, size_t size) const {
  const uint32_t addr = physical & 0x1FFFFFFFu;
  if (!physBase_ || static_cast<size_t>(addr) + size > physSize_) return nullptr;
  return physBase_ + addr;
}

rex::PhysicalRange RenderResources::OnPhysicalWrite(rex::PhysicalRange range, bool /*exact*/) {
  const uint32_t seq = writeSeq_.fetch_add(1u, std::memory_order_acq_rel) + 1u;
  if (pageSeq_ && chunkSeq_ && range.length > 0 && range.address < (512u << 20)) {
    const uint32_t start = range.address;
    const uint32_t end =
        std::min<uint32_t>(start + std::min<uint32_t>(range.length, (512u << 20) - start),
                           512u << 20);
    if (end > start) {
      const uint32_t p0 = start >> 12;
      const uint32_t p1 = (end - 1u) >> 12;
      for (uint32_t p = p0; p <= p1 && p < kPhysPages; ++p) {
        uint32_t prev = pageSeq_[p].load(std::memory_order_relaxed);
        while (seq > prev && !pageSeq_[p].compare_exchange_weak(prev, seq, std::memory_order_release,
                                                                 std::memory_order_relaxed)) {
        }
      }
      const uint32_t c0 = start >> 16;
      const uint32_t c1 = (end - 1u) >> 16;
      for (uint32_t c = c0; c <= c1 && c < kPhysChunks; ++c) {
        uint32_t prev = chunkSeq_[c].load(std::memory_order_relaxed);
        while (seq > prev && !chunkSeq_[c].compare_exchange_weak(prev, seq, std::memory_order_release,
                                                                 std::memory_order_relaxed)) {
        }
      }
    }
  }
  return range;
}

bool RenderResources::RangeDirtiedSince(uint32_t start, uint32_t end, uint32_t afterSeq) const {
  if (!pageSeq_ || !chunkSeq_ || end <= start || start >= (512u << 20)) return false;
  const uint32_t clampedEnd = std::min<uint32_t>(end, 512u << 20);
  const uint32_t c0 = start >> 16;
  const uint32_t c1 = (clampedEnd - 1u) >> 16;
  for (uint32_t c = c0; c <= c1; ++c) {
    if (chunkSeq_[c].load(std::memory_order_acquire) <= afterSeq) continue;
    const uint32_t p0 = std::max(start, c << 16) >> 12;
    const uint32_t p1 = (std::min(clampedEnd, (c + 1u) << 16) - 1u) >> 12;
    for (uint32_t p = p0; p <= p1; ++p) {
      if (pageSeq_[p].load(std::memory_order_acquire) > afterSeq) return true;
    }
  }
  return false;
}

bool RenderResources::NeedsRefresh(uint32_t physStart, uint32_t physEnd, uint32_t& uploadSeq,
                                 uint64_t prevUsedFrame, uint64_t volatileUntilFrame,
                                 bool& watchArmed, bool& watchHitOut) const {
  const uint32_t curSeq = writeSeq_.load(std::memory_order_acquire);
  watchHitOut = (uploadSeq != curSeq) && RangeDirtiedSince(physStart, physEnd, uploadSeq);
  if (watchHitOut) {
    watchArmed = false;
    uploadSeq = curSeq;
    return true;
  }
  uploadSeq = curSeq;
  if (!watchArmed) {
    // Re-check once per frame while volatile, or once after volatileUntilFrame expires to re-arm.
    return prevUsedFrame != frame_;
  }
  (void)volatileUntilFrame;
  return false;
}

void RenderResources::FinishRefresh(uint32_t addr, uint32_t size, bool changed, bool watchHit,
                                    uint64_t prevUsedFrame, uint32_t& rewriteStreak,
                                    uint64_t& volatileUntilFrame, bool& watchArmed) {
  if (!changed) {
    if (watchHit || (!watchArmed && frame_ >= volatileUntilFrame && prevUsedFrame != frame_)) {
      rewriteStreak = 0;
      ArmWatch(addr, size);
      watchArmed = true;
    }
    return;
  }
  if (rewriteStreak < 100) ++rewriteStreak;
  if (rewriteStreak >= 3) {
    volatileUntilFrame = frame_ + 60;
    watchArmed = false;
  } else {
    ArmWatch(addr, size);
    watchArmed = true;
  }
}

void RenderResources::ArmWatch(uint32_t start, uint32_t size) {
  if (watch_ && size > 0) {
    watch_->WatchPhysicalWrites({start & 0x1FFFFFFFu, size});
  }
}

void RenderResources::RequestScale(float scale) {
  requestedScale_ = std::clamp(scale, 0.25f, 8.0f);
}

void RenderResources::RequestMultisample(uint32_t samples) {
  requestedSamples_ = SupportedSamples(samples);
}

uint32_t RenderResources::SupportedSamples(uint32_t wanted) const {
  if (!device_) return 1;
  for (uint32_t s = wanted; s > 1; s /= 2) {
    UINT c = 0;
    UINT d = 0;
    if (SUCCEEDED(device_->CheckMultisampleQualityLevels(DXGI_FORMAT_R8G8B8A8_UNORM, s, &c)) &&
        c > 0 &&
        SUCCEEDED(device_->CheckMultisampleQualityLevels(DXGI_FORMAT_D32_FLOAT_S8X24_UINT, s, &d)) &&
        d > 0) {
      return s;
    }
  }
  return 1;
}

bool RenderResources::BeginFrame(uint64_t frame) {
  frame_ = frame;
  bool remade = false;
  if (requestedSamples_ != samples_) {
    remade = true;
    samples_ = requestedSamples_;
    colorTargets_.clear();
    depthTargets_.clear();
    colorKeys_.Clear();
    depthKeys_.Clear();
  }
  if (std::abs(requestedScale_ - scale_) > 0.001f) {
    remade = true;
    scale_ = requestedScale_;
    colorTargets_.clear();
    depthTargets_.clear();
    colorKeys_.Clear();
    depthKeys_.Clear();
    REXGPU_INFO("custom d3d11: internal render scale updated live -> {:.3f}x (no restart)", scale_);
  }

  if (frame % 300 != 0) return remade;
  for (uint32_t i = 1; i < textures_.size(); ++i) {
    TextureEntry& e = textures_[i];
    if (!e.live || frame - e.usedFrame < kUnusedFrames) continue;
    textureKeys_.Erase(e.key);
    e.texture.Reset();
    e.resource.Reset();
    e.face.Reset();
    e.live = false;
    freeTextures_.push_back(i + 1);
  }
  for (uint32_t i = 0; i < buffers_.size(); ++i) {
    BufferEntry& e = buffers_[i];
    if (!e.live || frame - e.usedFrame < kUnusedFrames) continue;
    bufferKeys_.Erase(e.key);
    e.buffer.Reset();
    e.resource.Reset();
    e.live = false;
    freeBuffers_.push_back(i + 1);
  }
  for (uint32_t i = 0; i < indexEntries_.size(); ++i) {
    IndexEntry& e = indexEntries_[i];
    if (!e.live || frame - e.usedFrame < kUnusedFrames) continue;
    indexKeys_.Erase(e.key);
    e.buffer.Reset();
    e.live = false;
    freeIndexEntries_.push_back(i + 1);
  }
  return remade;
}

uint32_t RenderResources::TargetRows(uint32_t pitch, uint32_t cls, uint32_t bytesPerSample) const {
  if (pitch == 0) return 0;
  const uint32_t edram = (10u << 20) / std::max(1u, pitch * bytesPerSample);
  uint32_t rows = pitch >= 1024 ? 720u : std::max(pitch, 360u);
  if (pitch / 32 < 513) {
    rows = std::max(rows, rowsAsked_[cls][pitch / 32]);
  }
  return std::min(std::min(rows, edram), 1440u);
}

void RenderResources::HostSize(uint32_t pitch, uint32_t rows, uint32_t /*cls*/, uint32_t& width,
                               uint32_t& height) const {
  width = std::clamp(static_cast<uint32_t>(std::lround(static_cast<double>(pitch) * scale_)), 1u,
                     16384u);
  height = std::clamp(static_cast<uint32_t>(std::lround(static_cast<double>(rows) * scale_)), 1u,
                      16384u);
}

void RenderResources::TargetSize(uint32_t surfaceInfo, uint32_t bytesPerSample, uint32_t& width,
                                 uint32_t& height, float& scaleX, float& scaleY) const {
  const uint32_t pitch = PitchOf(surfaceInfo);
  const uint32_t cls = ClassOf(surfaceInfo);
  if (pitch == 0) {
    width = height = 0;
    scaleX = scaleY = scale_;
    return;
  }
  const uint32_t rows = TargetRows(pitch, cls, bytesPerSample);
  HostSize(pitch, rows, cls, width, height);
  scaleX = static_cast<float>(width) / static_cast<float>(pitch);
  scaleY = rows > 0 ? static_cast<float>(height) / static_cast<float>(rows) : scale_;
}

bool RenderResources::EnsureRows(uint32_t surfaceInfo, uint32_t rows) {
  const uint32_t pitch = PitchOf(surfaceInfo);
  const uint32_t cls = ClassOf(surfaceInfo);
  if (pitch == 0 || pitch / 32 >= 513) return false;
  rows = std::min(rows, 1440u);
  if (rows <= rowsAsked_[cls][pitch / 32] || rows <= TargetRows(pitch, cls, 4)) return false;
  rowsAsked_[cls][pitch / 32] = std::min((rows + 63u) & ~63u, 1440u);
  for (ColorEntry& e : colorTargets_) {
    if (e.cls == cls && e.target.pitch == pitch && e.target.view) {
      colorKeys_.Erase(e.key);
      e.texture.Reset();
      e.view.Reset();
      e.resource.Reset();
      e.target.texture = nullptr;
      e.target.view = nullptr;
      e.target.resource = nullptr;
    }
  }
  for (DepthEntry& e : depthTargets_) {
    if (e.cls == cls && e.target.pitch == pitch && e.target.view) {
      depthKeys_.Erase(e.key);
      e.texture.Reset();
      e.view.Reset();
      e.resource.Reset();
      e.target.texture = nullptr;
      e.target.view = nullptr;
      e.target.resource = nullptr;
    }
  }
  return true;
}

RenderResources::Handle RenderResources::ColorTargetFor(uint32_t colorInfo, uint32_t surfaceInfo) {
  const uint32_t pitch = PitchOf(surfaceInfo);
  const uint32_t cls = ClassOf(surfaceInfo);
  const uint32_t rawFormat = (colorInfo >> 16) & 0xF;
  const uint32_t canonFormat =
      (rawFormat == 12u) ? 3u : (rawFormat == 10u) ? 2u : (rawFormat == 1u) ? 0u : rawFormat;
  const uint32_t key[2] = {(colorInfo & 0x00000FFFu) | (canonFormat << 16), pitch | (cls << 16)};
  if (const Handle found = colorKeys_.Find(key)) return found;

  colorTargets_.emplace_back();
  ColorEntry& entry = colorTargets_.back();
  const Handle handle = static_cast<Handle>(colorTargets_.size());
  uint32_t bytesPerSample = 4;
  entry.target.format = ColorFormat(canonFormat, bytesPerSample);
  entry.target.pitch = pitch;
  entry.target.rows = TargetRows(pitch, cls, bytesPerSample);
  entry.target.msaa = (surfaceInfo >> 16) & 3u;
  entry.cls = cls;
  HostSize(pitch, entry.target.rows, cls, entry.target.width, entry.target.height);
  std::memcpy(entry.key, key, sizeof(entry.key));
  colorKeys_.Insert(key, handle);
  if (pitch == 0 || entry.target.rows == 0 || !device_) return handle;

  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = entry.target.width;
  desc.Height = entry.target.height;
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = entry.target.format;
  const uint32_t samples = cls != 0 ? samples_ : 1u;
  desc.SampleDesc.Count = samples;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  if (FAILED(device_->CreateTexture2D(&desc, nullptr, &entry.texture))) return handle;
  device_->CreateRenderTargetView(entry.texture.Get(), nullptr, &entry.view);
  device_->CreateShaderResourceView(entry.texture.Get(), nullptr, &entry.resource);
  entry.target.texture = entry.texture.Get();
  entry.target.view = entry.view.Get();
  entry.target.resource = entry.resource.Get();
  entry.target.samples = samples;
  return handle;
}

RenderResources::Handle RenderResources::DepthTargetFor(uint32_t depthInfo, uint32_t surfaceInfo) {
  const uint32_t pitch = PitchOf(surfaceInfo);
  const uint32_t cls = ClassOf(surfaceInfo);
  const uint32_t key[2] = {depthInfo & 0xFFF, pitch | (cls << 16)};
  if (const Handle found = depthKeys_.Find(key)) return found;

  depthTargets_.emplace_back();
  DepthEntry& entry = depthTargets_.back();
  const Handle handle = static_cast<Handle>(depthTargets_.size());
  entry.target.pitch = pitch;
  entry.target.rows = TargetRows(pitch, cls, 4);
  entry.target.msaa = (surfaceInfo >> 16) & 3u;
  entry.cls = cls;
  HostSize(pitch, entry.target.rows, cls, entry.target.width, entry.target.height);
  std::memcpy(entry.key, key, sizeof(entry.key));
  depthKeys_.Insert(key, handle);
  if (pitch == 0 || entry.target.rows == 0 || !device_) return handle;

  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = entry.target.width;
  desc.Height = entry.target.height;
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_R32G8X24_TYPELESS;
  const uint32_t samples = cls != 0 ? samples_ : 1u;
  desc.SampleDesc.Count = samples;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
  if (FAILED(device_->CreateTexture2D(&desc, nullptr, &entry.texture))) return handle;

  D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
  dsvDesc.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
  dsvDesc.ViewDimension =
      samples > 1 ? D3D11_DSV_DIMENSION_TEXTURE2DMS : D3D11_DSV_DIMENSION_TEXTURE2D;
  device_->CreateDepthStencilView(entry.texture.Get(), &dsvDesc, &entry.view);

  D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
  srvDesc.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
  srvDesc.ViewDimension =
      samples > 1 ? D3D11_SRV_DIMENSION_TEXTURE2DMS : D3D11_SRV_DIMENSION_TEXTURE2D;
  srvDesc.Texture2D.MipLevels = 1;
  device_->CreateShaderResourceView(entry.texture.Get(), &srvDesc, &entry.resource);

  entry.target.texture = entry.texture.Get();
  entry.target.view = entry.view.Get();
  entry.target.resource = entry.resource.Get();
  entry.target.samples = samples;
  return handle;
}

const RenderResources::ColorTarget* RenderResources::ColorTargetOf(Handle handle) const {
  return (handle != 0 && handle <= colorTargets_.size()) ? &colorTargets_[handle - 1].target
                                                          : nullptr;
}

const RenderResources::DepthTarget* RenderResources::DepthTargetOf(Handle handle) const {
  return (handle != 0 && handle <= depthTargets_.size()) ? &depthTargets_[handle - 1].target
                                                          : nullptr;
}

RenderResources::Handle RenderResources::PrimaryColorTargetAt(uint32_t base, uint32_t minPitch,
                                                                uint32_t cls) const {
  const uint32_t rawFmt = (base >> 16) & 0xFu;
  const uint32_t canonFmt =
      (rawFmt == 12u) ? 3u : (rawFmt == 10u) ? 2u : (rawFmt == 1u) ? 0u : rawFmt;
  const uint32_t reqSamples = cls != 0 ? samples_ : 1u;
  Handle best = 0;
  uint32_t bestPitch = minPitch;
  int bestScore = -1;
  for (size_t i = 0; i < colorTargets_.size(); ++i) {
    const ColorEntry& e = colorTargets_[i];
    if ((e.key[0] & 0xFFFu) != (base & 0xFFFu) || !e.target.view) continue;
    if (e.target.pitch <= minPitch) continue;
    if (e.target.samples != reqSamples && samples_ > 1) continue;
    const int score =
        ((e.cls == cls) ? 4 : 0) + (((e.key[0] >> 16) & 0xFu) == canonFmt ? 2 : 0);
    if (score > bestScore || (score == bestScore && e.target.pitch > bestPitch)) {
      bestScore = score;
      bestPitch = e.target.pitch;
      best = static_cast<Handle>(i + 1);
    }
  }
  return best;
}

RenderResources::Handle RenderResources::PrimaryDepthTargetAt(uint32_t base, uint32_t minPitch,
                                                                uint32_t cls) const {
  const uint32_t reqSamples = cls != 0 ? samples_ : 1u;
  Handle best = 0;
  uint32_t bestPitch = minPitch;
  int bestScore = -1;
  for (size_t i = 0; i < depthTargets_.size(); ++i) {
    const DepthEntry& e = depthTargets_[i];
    if ((e.key[0] & 0xFFFu) != (base & 0xFFFu) || !e.target.view) continue;
    if (e.target.pitch <= minPitch) continue;
    if (e.target.samples != reqSamples && samples_ > 1) continue;
    const int score = (e.cls == cls) ? 2 : 0;
    if (score > bestScore || (score == bestScore && e.target.pitch > bestPitch)) {
      bestScore = score;
      bestPitch = e.target.pitch;
      best = static_cast<Handle>(i + 1);
    }
  }
  return best;
}

RenderResources::Resolved* RenderResources::ResolvedFor(uint32_t physical, uint32_t width,
                                                        uint32_t height, DXGI_FORMAT format,
                                                        bool depth) {
  if (!device_ || width == 0 || height == 0) return nullptr;
  const uint32_t addr = physical & 0x1FFFFFFFu;
  const uint32_t key[1] = {addr};
  Handle handle = resolvedKeys_.Find(key);
  if (handle == 0) {
    resolved_.emplace_back();
    handle = static_cast<Handle>(resolved_.size());
    resolved_[handle - 1].physical = addr;
    resolvedKeys_.Insert(key, handle);
  }
  ResolvedEntry& entry = resolved_[handle - 1];
  Resolved& out = entry.surface;
  if (!entry.texture || out.width != width || out.height != height || out.format != format) {
    entry.texture.Reset();
    entry.resource.Reset();
    entry.view.Reset();
    out.texture = nullptr;
    out.resource = nullptr;
    out.view = nullptr;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device_->CreateTexture2D(&desc, nullptr, &entry.texture))) return nullptr;
    device_->CreateShaderResourceView(entry.texture.Get(), nullptr, &entry.resource);
    device_->CreateRenderTargetView(entry.texture.Get(), nullptr, &entry.view);
    out.texture = entry.texture.Get();
    out.resource = entry.resource.Get();
    out.view = entry.view.Get();
    out.width = width;
    out.height = height;
    out.format = format;
  }
  out.depth = depth;
  return &out;
}

const RenderResources::Resolved* RenderResources::ResolvedAt(uint32_t physical) const {
  const uint32_t key[1] = {physical & 0x1FFFFFFFu};
  const Handle handle = resolvedKeys_.Find(key);
  if (handle == 0) return nullptr;
  const Resolved& out = resolved_[handle - 1].surface;
  return out.resource ? &out : nullptr;
}

const RenderResources::Resolved* RenderResources::UploadCpuFrontBuffer(uint32_t physical,
                                                                       uint32_t width,
                                                                       uint32_t height) {
  if (const Resolved* existing = ResolvedAt(physical)) return existing;
  if (!context_ || width == 0 || height == 0 || width > 4096 || height > 4096) return nullptr;
  const uint32_t pitch = (width + 31u) & ~31u;
  const uint8_t* src = PhysicalPtr(physical, static_cast<size_t>(pitch) * height * 4);
  if (!src) return nullptr;

  Resolved* dst = ResolvedFor(physical, width, height, DXGI_FORMAT_R8G8B8A8_UNORM, false);
  if (!dst || !dst->texture) return nullptr;
  dst->guestWidth = width;
  dst->guestHeight = height;

  std::vector<uint8_t> rgba(static_cast<size_t>(width) * height * 4);
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      const uint32_t off =
          static_cast<uint32_t>(rex::xenos::GetTiledOffset2D(static_cast<int32_t>(x),
                                                             static_cast<int32_t>(y), pitch, 2));
      uint32_t w = 0;
      std::memcpy(&w, src + off, 4);
      w = ByteSwap32(w);
      std::memcpy(rgba.data() + (static_cast<size_t>(y) * width + x) * 4, &w, 4);
    }
  }
  context_->UpdateSubresource(dst->texture, 0, nullptr, rgba.data(), width * 4, 0);
  return dst;
}

bool RenderResources::UploadTexture(TextureEntry& entry, const uint32_t fetch[6]) {
  const uint32_t format = fetch[1] & 0x3F;
  const uint32_t endian = (fetch[1] >> 6) & 3;
  const uint32_t pitch = ((fetch[0] >> 22) & 0x1FF) * 32;
  const bool tiled = ((fetch[0] >> 31) & 1) != 0;
  const uint32_t width = (fetch[2] & 0x1FFF) + 1;
  const uint32_t height = ((fetch[2] >> 13) & 0x1FFF) + 1;
  const bool packedMips = ((fetch[5] >> 11) & 1) != 0;
  const uint32_t mipAddress = fetch[5] & 0xFFFFF000u;
  const uint32_t mipMax = (fetch[4] >> 6) & 0xF;
  const uint32_t dimension = (fetch[5] >> 9) & 3;
  const bool cube = dimension == 3;
  const uint32_t faces = cube ? 6u : 1u;

  const TexFormatDesc fmt = DescribeTextureFormat(format);
  if (!fmt.supported) return false;

  const uint32_t basePhys = fetch[1] & 0xFFFFF000u;
  const uint32_t baseBlocksWide = (width + fmt.blockTexels - 1) / fmt.blockTexels;
  const uint32_t baseBlocksHigh = (height + fmt.blockTexels - 1) / fmt.blockTexels;
  const uint32_t pitchBlocks = std::max(pitch / fmt.blockTexels, 1u);
  const uint32_t rowsBlocksAligned = tiled ? ((baseBlocksHigh + 31) & ~31u) : baseBlocksHigh;
  const size_t faceBytes =
      static_cast<size_t>(std::max(pitchBlocks, baseBlocksWide)) * rowsBlocksAligned * fmt.blockBytes;
  const size_t totalBaseBytes = faceBytes * faces;
  const uint8_t* data = PhysicalPtr(basePhys, std::min<size_t>(totalBaseBytes, 4096));
  if (!data) return false;
  const size_t availBase = physSize_ - (basePhys & 0x1FFFFFFFu);

  uint32_t levels = 1;
  const bool pow2 = (width & (width - 1)) == 0 && (height & (height - 1)) == 0;
  if (!cube && mipAddress != 0 && mipMax > 0 && pow2) {
    uint32_t w = width;
    uint32_t h = height;
    while (levels <= mipMax && w > 16 && h > 16) {
      w = std::max(w >> 1, 1u);
      h = std::max(h >> 1, 1u);
      ++levels;
    }
  }

  subresourceScratch_.resize(levels * faces);
  uint32_t packedX = 0;
  uint32_t packedY = 0;
  PackedBaseOffset(packedMips, width, height, packedX, packedY);

  // Pre-compute flat scratch layout (zero per-texture heap churn after warmup).
  uint32_t baseRowBytes = 0;
  const size_t baseSliceBytes = LinearLevelSize(fmt, width, height, baseRowBytes);
  size_t totalScratch = baseSliceBytes * faces;
  uint32_t actualLevels = levels;
  uint32_t mipOffsets[16] = {};
  uint32_t mipBytes[16] = {};
  uint32_t mipPitchTexels[16] = {};
  size_t mipSliceBytes[16] = {};
  uint32_t mipRowBytes[16] = {};
  uint32_t mipOffset = 0;
  for (uint32_t level = 1; level < levels && level < 16u; ++level) {
    const uint32_t w = std::max(width >> level, 1u);
    const uint32_t h = std::max(height >> level, 1u);
    const uint32_t bw = (w + fmt.blockTexels - 1) / fmt.blockTexels;
    const uint32_t bh = (h + fmt.blockTexels - 1) / fmt.blockTexels;
    const uint32_t pb = (bw + 31) & ~31u;
    const uint32_t rb = (bh + 31) & ~31u;
    const uint32_t levelBytes = pb * rb * fmt.blockBytes;
    if (!PhysicalPtr(mipAddress + mipOffset, levelBytes)) {
      actualLevels = level;
      break;
    }
    mipOffsets[level] = mipOffset;
    mipBytes[level] = levelBytes;
    mipPitchTexels[level] = pb * fmt.blockTexels;
    mipSliceBytes[level] = LinearLevelSize(fmt, w, h, mipRowBytes[level]);
    totalScratch += mipSliceBytes[level];
    mipOffset += levelBytes;
  }
  levels = actualLevels;
  if (texScratch_.size() < totalScratch) {
    texScratch_.resize(totalScratch);
  }
  subresourceScratch_.resize(levels * faces);

  size_t cursor = 0;
  uint32_t rowBytes = 0;
  for (uint32_t f = 0; f < faces; ++f) {
    const size_t fOff = static_cast<size_t>(f) * faceBytes;
    if (fOff >= availBase) return false;
    uint8_t* dst = texScratch_.data() + cursor;
    LinearLevelInto(data + fOff, availBase - fOff, fmt, endian, width, height, pitch, tiled,
                    packedX, packedY, dst, rowBytes);
    subresourceScratch_[f * levels] = {dst, rowBytes, 0};
    cursor += baseSliceBytes;
  }

  for (uint32_t level = 1; level < levels && level < 16u; ++level) {
    const uint32_t w = std::max(width >> level, 1u);
    const uint32_t h = std::max(height >> level, 1u);
    const uint8_t* mipData = PhysicalPtr(mipAddress + mipOffsets[level], mipBytes[level]);
    uint8_t* dst = texScratch_.data() + cursor;
    LinearLevelInto(mipData, mipBytes[level], fmt, endian, w, h, mipPitchTexels[level], tiled, 0,
                    0, dst, rowBytes);
    subresourceScratch_[level] = {dst, rowBytes, 0};
    cursor += mipSliceBytes[level];
  }

  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = fmt.compressed ? ((width + 3) & ~3u) : width;
  desc.Height = fmt.compressed ? ((height + 3) & ~3u) : height;
  desc.MipLevels = levels;
  desc.ArraySize = faces;
  desc.Format = fmt.dxgi;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_IMMUTABLE;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  desc.MiscFlags = cube ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0;

  entry.texture.Reset();
  entry.resource.Reset();
  entry.face.Reset();
  if (FAILED(device_->CreateTexture2D(&desc, subresourceScratch_.data(), &entry.texture))) {
    return false;
  }
  if (FAILED(device_->CreateShaderResourceView(entry.texture.Get(), nullptr, &entry.resource))) {
    entry.texture.Reset();
    return false;
  }
  entry.cube = cube;
  if (cube) {
    D3D11_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = fmt.dxgi;
    view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    view.Texture2DArray.MipLevels = 1;
    view.Texture2DArray.ArraySize = 1;
    device_->CreateShaderResourceView(entry.texture.Get(), &view, &entry.face);
  }
  entry.width = width;
  entry.height = height;
  entry.bytes = totalBaseBytes;
  return true;
}

RenderResources::Handle RenderResources::TextureFor(const uint32_t fetch[6], uint32_t& width,
                                                    uint32_t& height, bool& resolved) {
  const uint32_t format = fetch[1] & 0x3F;
  const uint32_t base = fetch[1] & 0xFFFFF000u;
  width = (fetch[2] & 0x1FFF) + 1;
  height = ((fetch[2] >> 13) & 0x1FFF) + 1;
  resolved = false;

  if ((fetch[0] & 3) != 2 || base == 0 || width > 8192 || height > 8192) return 1;

  if (const Resolved* surface = ResolvedAt(base)) {
    width = surface->guestWidth;
    height = surface->guestHeight;
    resolved = true;
    return 0;
  }

  const uint32_t dimension = (fetch[5] >> 9) & 3;
  if (dimension == 2) {
    static std::atomic<bool> warnedDim2{false};
    if (!warnedDim2.exchange(true)) {
      REXLOG_WARN("custom-renderer: 3D texture fallback used (fmt={} {}x{})", format, width, height);
    }
    return 1;
  }
  const TexFormatDesc fmt = DescribeTextureFormat(format);
  if (!fmt.supported) {
    static std::atomic<uint64_t> warnedFmt{0};
    const uint64_t bit = 1ULL << (format & 63u);
    if ((warnedFmt.fetch_or(bit) & bit) == 0) {
      REXLOG_WARN("custom-renderer: unsupported texture fmt={} ({}x{})", format, width, height);
    }
    return 1;
  }

  const uint32_t key[4] = {base & 0x1FFFFFFFu, format | (dimension << 8), width, height};
  Handle handle = textureKeys_.Find(key);

  const uint32_t pitch = ((fetch[0] >> 22) & 0x1FF) * 32;
  const uint32_t srcBytes = static_cast<uint32_t>(std::max<size_t>(
      4096, static_cast<size_t>(std::max(pitch / fmt.blockTexels, 1u)) *
                ((height + fmt.blockTexels - 1) / fmt.blockTexels) * fmt.blockBytes));
  const uint8_t* ptr = PhysicalPtr(base, 4);
  if (!ptr) return 1;
  const size_t fpBytes = std::min<size_t>(srcBytes, physSize_ - (base & 0x1FFFFFFFu));

  if (handle != 0) {
    TextureEntry& entry = textures_[handle - 1];
    const uint64_t prevUsedFrame = entry.usedFrame;
    entry.usedFrame = frame_;
    bool watchHit = false;
    const bool refresh = NeedsRefresh(entry.physStart, entry.physEnd, entry.uploadSeq, prevUsedFrame,
                                      entry.volatileUntilFrame, entry.watchArmed, watchHit);
    if (!refresh) {
      return handle;
    }
    const uint32_t seqBeforeRead = writeSeq_.load(std::memory_order_acquire);
    const uint64_t fp = Fingerprint(ptr, fpBytes);
    const bool changed = (fp != entry.fingerprint);
    FinishRefresh(base, static_cast<uint32_t>(fpBytes), changed, watchHit, prevUsedFrame,
                  entry.rewriteStreak, entry.volatileUntilFrame, entry.watchArmed);
    entry.uploadSeq = seqBeforeRead;
    if (!changed) {
      return handle;
    }
    entry.fingerprint = fp;
    if (UploadTexture(entry, fetch)) return handle;
    return 1;
  }

  if (!freeTextures_.empty()) {
    handle = freeTextures_.back();
    freeTextures_.pop_back();
  } else {
    textures_.emplace_back();
    handle = static_cast<Handle>(textures_.size());
  }
  TextureEntry& entry = textures_[handle - 1];
  entry = TextureEntry();
  std::memcpy(entry.key, key, sizeof(entry.key));
  entry.physStart = base & 0x1FFFFFFFu;
  entry.physEnd = entry.physStart + static_cast<uint32_t>(fpBytes);
  entry.uploadSeq = writeSeq_.load(std::memory_order_acquire);
  entry.fingerprint = Fingerprint(ptr, fpBytes);
  entry.usedFrame = frame_;
  ArmWatch(entry.physStart, static_cast<uint32_t>(fpBytes));
  entry.watchArmed = true;
  if (!UploadTexture(entry, fetch)) {
    freeTextures_.push_back(handle);
    return 1;
  }
  entry.live = true;
  textureKeys_.Insert(key, handle);
  return handle;
}

ID3D11ShaderResourceView* RenderResources::TextureView(Handle handle, bool cube,
                                                       bool force2D) const {
  if (handle == 0 || handle > textures_.size()) return nullptr;
  const TextureEntry& e = textures_[handle - 1];
  if (handle == 1) {
    return cube ? whiteCubeView_.Get() : e.resource.Get();
  }
  if (e.cube && force2D) return e.face ? e.face.Get() : whiteCubeView_.Get();
  if (cube && !e.cube) return whiteCubeView_.Get();
  return e.resource.Get();
}

RenderResources::Handle RenderResources::VertexBufferFor(uint32_t physical, uint32_t bytes) {
  if (!device_ || !context_ || physical == 0 || bytes == 0) return 0;
  const uint32_t addr = physical & 0x1FFFFFFFu;
  const uint32_t key[2] = {addr, 0};
  Handle handle = bufferKeys_.Find(key);

  const uint8_t* data = PhysicalPtr(addr, bytes);
  if (!data) return 0;

  if (handle != 0) {
    BufferEntry& entry = buffers_[handle - 1];
    const uint64_t prevUsedFrame = entry.usedFrame;
    entry.usedFrame = frame_;
    const uint32_t wanted = std::max(bytes, entry.seen);
    bool watchHit = false;
    bool refresh = NeedsRefresh(entry.physStart, entry.physEnd, entry.uploadSeq, prevUsedFrame,
                                entry.volatileUntilFrame, entry.watchArmed, watchHit);
    // Volatile dynamic VBs (e.g. dynamic ring/cloth buffers) can be rewritten multiple times in
    // the same frame while write watches are disarmed.
    if (!refresh && !entry.watchArmed && wanted <= 262144u) {
      refresh = true;
    }
    if (!refresh && entry.bytes >= wanted) {
      return handle;
    }
    const uint32_t seqBeforeRead = writeSeq_.load(std::memory_order_acquire);
    const uint64_t fp = Fingerprint(data, wanted);
    const bool changed = (fp != entry.fingerprint);
    FinishRefresh(addr, wanted, changed, watchHit, prevUsedFrame, entry.rewriteStreak,
                  entry.volatileUntilFrame, entry.watchArmed);
    entry.uploadSeq = seqBeforeRead;
    if (!changed && entry.bytes >= wanted) {
      return handle;
    }
    entry.fingerprint = fp;
    entry.seen = wanted;
    if (entry.bytes >= wanted) {
      D3D11_BOX box{0, 0, 0, wanted, 1, 1};
      context_->UpdateSubresource(entry.buffer.Get(), 0, &box, data, 0, 0);
      return handle;
    }
    bufferKeys_.Erase(entry.key);
    entry.buffer.Reset();
    entry.resource.Reset();
    entry.live = false;
    freeBuffers_.push_back(handle);
    handle = 0;
  }

  if (!freeBuffers_.empty()) {
    handle = freeBuffers_.back();
    freeBuffers_.pop_back();
  } else {
    buffers_.emplace_back();
    handle = static_cast<Handle>(buffers_.size());
  }
  BufferEntry& entry = buffers_[handle - 1];
  entry = BufferEntry();
  std::memcpy(entry.key, key, sizeof(entry.key));
  entry.physStart = addr;
  entry.physEnd = addr + bytes;
  entry.uploadSeq = writeSeq_.load(std::memory_order_acquire);
  entry.fingerprint = Fingerprint(data, bytes);
  entry.usedFrame = frame_;
  entry.seen = bytes;
  entry.bytes = (bytes + bytes / 8 + 4095u) & ~4095u;

  D3D11_BUFFER_DESC desc{};
  desc.ByteWidth = entry.bytes;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
  if (FAILED(device_->CreateBuffer(&desc, nullptr, &entry.buffer))) {
    freeBuffers_.push_back(handle);
    return 0;
  }
  D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
  srv.Format = DXGI_FORMAT_R32_TYPELESS;
  srv.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
  srv.BufferEx.FirstElement = 0;
  srv.BufferEx.NumElements = entry.bytes / 4;
  srv.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
  if (FAILED(device_->CreateShaderResourceView(entry.buffer.Get(), &srv, &entry.resource))) {
    entry.buffer.Reset();
    freeBuffers_.push_back(handle);
    return 0;
  }
  D3D11_BOX box{0, 0, 0, bytes, 1, 1};
  context_->UpdateSubresource(entry.buffer.Get(), 0, &box, data, 0, 0);
  entry.live = true;
  bufferKeys_.Insert(key, handle);
  ArmWatch(addr, bytes);
  entry.watchArmed = true;
  return handle;
}

ID3D11ShaderResourceView* RenderResources::VertexBufferView(Handle handle) const {
  return (handle != 0 && handle <= buffers_.size()) ? buffers_[handle - 1].resource.Get()
                                                    : nullptr;
}

uint32_t RenderResources::IndexAppend(const void* data, uint32_t bytes) {
  return indexRing_.Append(context_, data, bytes, 4);
}

uint8_t* RenderResources::IndexMap(uint32_t bytes, uint32_t& offset) {
  return indexRing_.Map(context_, bytes, 4, offset);
}

void RenderResources::IndexUnmap() { indexRing_.Unmap(context_); }

ID3D11Buffer* RenderResources::StaticIndicesFor(uint32_t physical, uint32_t count, bool wide,
                                                uint32_t resetIndex) {
  if (!device_ || physical == 0 || count < 128) return nullptr;
  const uint32_t addr = physical & 0x1FFFFFFFu;
  const uint32_t rawBytes = count * (wide ? 4u : 2u);
  const uint8_t* source = PhysicalPtr(addr, rawBytes);
  if (!source) return nullptr;

  const uint32_t key[4] = {addr, count, wide ? 1u : 0u, resetIndex};
  Handle handle = indexKeys_.Find(key);
  if (handle != 0) {
    IndexEntry& e = indexEntries_[handle - 1];
    const uint64_t prevUsedFrame = e.usedFrame;
    e.usedFrame = frame_;
    bool watchHit = false;
    const bool needsCheck =
        !e.buffer || NeedsRefresh(e.physStart, e.physEnd, e.uploadSeq, prevUsedFrame,
                                  e.volatileUntilFrame, e.watchArmed, watchHit);
    if (e.buffer && !needsCheck) return e.buffer.Get();
    const uint32_t seqBeforeRead = writeSeq_.load(std::memory_order_acquire);
    const uint64_t fp = Fingerprint(source, rawBytes);
    const bool changed = (fp != e.fingerprint);
    FinishRefresh(addr, rawBytes, changed, watchHit, prevUsedFrame, e.rewriteStreak,
                  e.volatileUntilFrame, e.watchArmed);
    e.uploadSeq = seqBeforeRead;
    if (changed) {
      e.fingerprint = fp;
      e.stable = 0;
      if (e.buffer) {
        indexKeys_.Erase(e.key);
        e.buffer.Reset();
        e.live = false;
        freeIndexEntries_.push_back(handle);
      }
      return nullptr;
    }
    if (e.buffer) return e.buffer.Get();
    if (++e.stable < 4) return nullptr;

    const uint32_t hostBytes = count * (wide ? 4u : 2u);
    indexScratch_.resize(hostBytes);
    if (wide) {
      const auto* s = reinterpret_cast<const uint32_t*>(source);
      auto* d = reinterpret_cast<uint32_t*>(indexScratch_.data());
      for (uint32_t i = 0; i < count; ++i) d[i] = ByteSwap32(s[i]) & 0xFFFFFFu;
    } else {
      const auto* s = reinterpret_cast<const uint16_t*>(source);
      auto* d = reinterpret_cast<uint16_t*>(indexScratch_.data());
      for (uint32_t i = 0; i < count; ++i) d[i] = ByteSwap16(s[i]);
    }
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = hostBytes;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
    const D3D11_SUBRESOURCE_DATA init{indexScratch_.data(), 0, 0};
    if (SUCCEEDED(device_->CreateBuffer(&desc, &init, &e.buffer))) {
      return e.buffer.Get();
    }
    return nullptr;
  }

  if (!freeIndexEntries_.empty()) {
    handle = freeIndexEntries_.back();
    freeIndexEntries_.pop_back();
  } else {
    indexEntries_.emplace_back();
    handle = static_cast<Handle>(indexEntries_.size());
  }
  IndexEntry& e = indexEntries_[handle - 1];
  e = IndexEntry();
  std::memcpy(e.key, key, sizeof(e.key));
  e.physStart = addr;
  e.physEnd = addr + rawBytes;
  e.uploadSeq = writeSeq_.load(std::memory_order_acquire);
  e.fingerprint = Fingerprint(source, rawBytes);
  e.usedFrame = frame_;
  e.stable = 1;
  e.live = true;
  indexKeys_.Insert(key, handle);
  ArmWatch(addr, rawBytes);
  e.watchArmed = true;
  return nullptr;
}

uint8_t* RenderResources::ConstantMap(uint32_t bytes, uint32_t& offset) {
  return constantRing_.Map(context_, bytes, 256, offset);
}

bool RenderResources::ConstantWouldWrap(uint32_t bytes) const {
  const uint32_t aligned = (constantRing_.offset + 255u) & ~255u;
  return aligned + bytes > constantRing_.bytes;
}

void RenderResources::ConstantUnmap() { constantRing_.Unmap(context_); }

}  // namespace bs1::renderer
