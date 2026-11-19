#include "gles3_resources.h"

#include <EGL/egl.h>
#include <android/log.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string_view>

#define GLESLOG(...) __android_log_print(ANDROID_LOG_INFO, "BS1-GLES3", __VA_ARGS__)

namespace bs1::renderer::gles3 {

namespace {

// Standard GLES extension enums (supported natively on Adreno 710).
constexpr GLenum kGlCompressedRgbaS3tcDxt1 = 0x83F1;
constexpr GLenum kGlCompressedRgbaS3tcDxt3 = 0x83F2;
constexpr GLenum kGlCompressedRgbaS3tcDxt5 = 0x83F3;
constexpr GLenum kGlCompressedRedRgtc1 = 0x8DBB;
constexpr GLenum kGlCompressedRgRgtc2 = 0x8DBD;
constexpr GLenum kGlTextureMaxAnisotropy = 0x84FE;
constexpr GLenum kGlMaxTextureMaxAnisotropy = 0x84FF;
constexpr GLenum kGlClampToBorder = 0x812D;
constexpr GLenum kGlMirrorClampToEdge = 0x8743;
constexpr GLenum kGlR16 = 0x822A;
constexpr GLenum kGlRg16 = 0x822C;
constexpr GLenum kGlRgba16 = 0x805B;

constexpr uint32_t kPhysicalMask = 0x1FFFFFFFu;

inline uint32_t ByteSwap32(uint32_t v) { return __builtin_bswap32(v); }
inline uint16_t ByteSwap16(uint16_t v) { return static_cast<uint16_t>((v << 8) | (v >> 8)); }

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

struct GlTexFmt {
  GLenum internalFormat = GL_RGBA8;
  GLenum format = GL_RGBA;
  GLenum type = GL_UNSIGNED_BYTE;
  uint32_t blockBytes = 4;
  bool compressed = false;
  bool expand565 = false;
  bool expand4444 = false;
  bool expand1555 = false;
  bool expand16ToFloat = false;
  bool supported = false;
};

GlTexFmt DescribeGlFormat(uint32_t fmt, bool hasS3tc, bool hasRgtc, bool hasNorm16) {
  GlTexFmt d{};
  switch (fmt) {
    case 2:   // k_8
    case 8:   // k_8_A
    case 9:   // k_8_B
      d.internalFormat = GL_R8;
      d.format = GL_RED;
      d.type = GL_UNSIGNED_BYTE;
      d.blockBytes = 1;
      d.supported = true;
      break;
    case 3:  // k_1_5_5_5
      d.internalFormat = GL_RGBA8;
      d.format = GL_RGBA;
      d.type = GL_UNSIGNED_BYTE;
      d.blockBytes = 2;
      d.expand1555 = true;
      d.supported = true;
      break;
    case 4:  // k_5_6_5
      d.internalFormat = GL_RGBA8;
      d.format = GL_RGBA;
      d.type = GL_UNSIGNED_BYTE;
      d.blockBytes = 2;
      d.expand565 = true;
      d.supported = true;
      break;
    case 6:   // k_8_8_8_8
    case 14:  // k_8_8_8_8_A
    case 50:
    case 62:
      d.internalFormat = GL_RGBA8;
      d.format = GL_RGBA;
      d.type = GL_UNSIGNED_BYTE;
      d.blockBytes = 4;
      d.supported = true;
      break;
    case 7:   // k_2_10_10_10
    case 54:
      d.internalFormat = GL_RGB10_A2;
      d.format = GL_RGBA;
      d.type = GL_UNSIGNED_INT_2_10_10_10_REV;
      d.blockBytes = 4;
      d.supported = true;
      break;
    case 10:  // k_8_8
      d.internalFormat = GL_RG8;
      d.format = GL_RG;
      d.type = GL_UNSIGNED_BYTE;
      d.blockBytes = 2;
      d.supported = true;
      break;
    case 15:  // k_4_4_4_4
      d.internalFormat = GL_RGBA8;
      d.format = GL_RGBA;
      d.type = GL_UNSIGNED_BYTE;
      d.blockBytes = 2;
      d.expand4444 = true;
      d.supported = true;
      break;
    case 18:  // DXT1
    case 51:
      d.internalFormat = kGlCompressedRgbaS3tcDxt1;
      d.blockBytes = 8;
      d.compressed = true;
      d.supported = hasS3tc;
      break;
    case 19:  // DXT2/3
    case 52:
      d.internalFormat = kGlCompressedRgbaS3tcDxt3;
      d.blockBytes = 16;
      d.compressed = true;
      d.supported = hasS3tc;
      break;
    case 20:  // DXT4/5
    case 53:
      d.internalFormat = kGlCompressedRgbaS3tcDxt5;
      d.blockBytes = 16;
      d.compressed = true;
      d.supported = hasS3tc;
      break;
    case 24:  // k_16
      d.internalFormat = hasNorm16 ? kGlR16 : GL_R16F;
      d.format = GL_RED;
      d.type = hasNorm16 ? GL_UNSIGNED_SHORT : GL_FLOAT;
      d.blockBytes = 2;
      d.expand16ToFloat = !hasNorm16;
      d.supported = true;
      break;
    case 25:  // k_16_16
      d.internalFormat = hasNorm16 ? kGlRg16 : GL_RG16F;
      d.format = GL_RG;
      d.type = hasNorm16 ? GL_UNSIGNED_SHORT : GL_FLOAT;
      d.blockBytes = 4;
      d.expand16ToFloat = !hasNorm16;
      d.supported = true;
      break;
    case 26:  // k_16_16_16_16
      d.internalFormat = hasNorm16 ? kGlRgba16 : GL_RGBA16F;
      d.format = GL_RGBA;
      d.type = hasNorm16 ? GL_UNSIGNED_SHORT : GL_FLOAT;
      d.blockBytes = 8;
      d.expand16ToFloat = !hasNorm16;
      d.supported = true;
      break;
    case 27:  // k_16_FLOAT
    case 30:
      d.internalFormat = GL_R16F;
      d.format = GL_RED;
      d.type = GL_HALF_FLOAT;
      d.blockBytes = 2;
      d.supported = true;
      break;
    case 28:  // k_16_16_FLOAT
    case 31:
      d.internalFormat = GL_RG16F;
      d.format = GL_RG;
      d.type = GL_HALF_FLOAT;
      d.blockBytes = 4;
      d.supported = true;
      break;
    case 29:  // k_16_16_16_16_FLOAT
    case 32:
      d.internalFormat = GL_RGBA16F;
      d.format = GL_RGBA;
      d.type = GL_HALF_FLOAT;
      d.blockBytes = 8;
      d.supported = true;
      break;
    case 36:  // k_32_FLOAT
      d.internalFormat = GL_R32F;
      d.format = GL_RED;
      d.type = GL_FLOAT;
      d.blockBytes = 4;
      d.supported = true;
      break;
    case 37:  // k_32_32_FLOAT
      d.internalFormat = GL_RG32F;
      d.format = GL_RG;
      d.type = GL_FLOAT;
      d.blockBytes = 8;
      d.supported = true;
      break;
    case 38:  // k_32_32_32_32_FLOAT
      d.internalFormat = GL_RGBA32F;
      d.format = GL_RGBA;
      d.type = GL_FLOAT;
      d.blockBytes = 16;
      d.supported = true;
      break;
    case 49:  // k_DXN (BC5)
      d.internalFormat = kGlCompressedRgRgtc2;
      d.blockBytes = 16;
      d.compressed = true;
      d.supported = hasRgtc;
      break;
    case 59:  // k_DXT5A (BC4)
      d.internalFormat = kGlCompressedRedRgtc1;
      d.blockBytes = 8;
      d.compressed = true;
      d.supported = hasRgtc;
      break;
    default:
      break;
  }
  return d;
}

bool HasExtension(const char* extList, const char* name) {
  if (!extList || !name) return false;
  const std::string_view all(extList);
  const std::string_view needle(name);
  size_t pos = 0;
  while ((pos = all.find(needle, pos)) != std::string_view::npos) {
    const bool startOk = (pos == 0 || all[pos - 1] == ' ');
    const size_t end = pos + needle.size();
    const bool endOk = (end == all.size() || all[end] == ' ');
    if (startOk && endOk) return true;
    pos = end;
  }
  return false;
}

size_t LinearizeLevel(const uint8_t* src, size_t maxBytes, const GlTexFmt& fmt, uint32_t endian,
                      uint32_t width, uint32_t height, uint32_t pitchTexels, bool tiled,
                      uint32_t x0, uint32_t y0, std::vector<uint8_t>& out) {
  if (fmt.compressed) {
    const uint32_t bw = (width + 3u) / 4u;
    const uint32_t bh = (height + 3u) / 4u;
    const uint32_t pitchBlocks = std::max(pitchTexels / 4u, 1u);
    const uint32_t pitchAligned = (pitchBlocks + 31u) & ~31u;
    const uint32_t bpbLog2 = fmt.blockBytes == 8u ? 3u : 4u;
    const uint32_t bx0 = x0 / 4u;
    const uint32_t by0 = y0 / 4u;
    const uint32_t rowBytes = bw * fmt.blockBytes;
    const size_t totalBytes = static_cast<size_t>(rowBytes) * bh;
    out.resize(totalBytes);
    uint8_t* dst = out.data();
    if (!tiled) {
      for (uint32_t by = 0; by < bh; ++by) {
        const size_t rowSrcOff = static_cast<size_t>((by + by0) * pitchBlocks + bx0) * fmt.blockBytes;
        uint8_t* rowDst = dst + static_cast<size_t>(by) * rowBytes;
        for (uint32_t bx = 0; bx < bw; ++bx) {
          const size_t off = rowSrcOff + static_cast<size_t>(bx) * fmt.blockBytes;
          uint8_t* d = rowDst + static_cast<size_t>(bx) * fmt.blockBytes;
          if (off + fmt.blockBytes > maxBytes) {
            std::memset(d, 0, fmt.blockBytes);
            continue;
          }
          const uint8_t* s = src + off;
          if (endian == 1u || endian == 2u) {
            Swap16Bytes8(d, s);
            if (fmt.blockBytes == 16u) Swap16Bytes8(d + 8, s + 8);
          } else {
            std::memcpy(d, s, fmt.blockBytes);
          }
        }
      }
      return totalBytes;
    }
    for (uint32_t by = 0; by < bh; ++by) {
      uint8_t* rowDst = dst + static_cast<size_t>(by) * rowBytes;
      for (uint32_t bx = 0; bx < bw; ++bx) {
        const size_t off = TiledOffset2D(bx + bx0, by + by0, pitchAligned, bpbLog2);
        uint8_t* d = rowDst + static_cast<size_t>(bx) * fmt.blockBytes;
        if (off + fmt.blockBytes > maxBytes) {
          std::memset(d, 0, fmt.blockBytes);
          continue;
        }
        const uint8_t* s = src + off;
        if (endian == 1u || endian == 2u) {
          Swap16Bytes8(d, s);
          if (fmt.blockBytes == 16u) Swap16Bytes8(d + 8, s + 8);
        } else {
          std::memcpy(d, s, fmt.blockBytes);
        }
      }
    }
    return totalBytes;
  }

  const bool expand32 = fmt.expand565 || fmt.expand4444 || fmt.expand1555;
  const uint32_t outBpp = expand32 ? 4u : (fmt.expand16ToFloat ? fmt.blockBytes * 2u : fmt.blockBytes);
  const uint32_t rowBytes = width * outBpp;
  const size_t totalBytes = static_cast<size_t>(rowBytes) * height;
  out.resize(totalBytes);
  uint8_t* dst = out.data();
  const uint32_t pitchAligned = (std::max(pitchTexels, 1u) + 31u) & ~31u;
  uint32_t bppLog2 = 0;
  if (fmt.blockBytes == 2u) bppLog2 = 1u;
  else if (fmt.blockBytes == 4u) bppLog2 = 2u;
  else if (fmt.blockBytes == 8u) bppLog2 = 3u;
  else if (fmt.blockBytes == 16u) bppLog2 = 4u;

  // Fast path: untiled, non-expanded, no-endian-swap (e.g. 1280x720 Bink video Y/Cr/Cb planes).
  // Uses row memcpy instead of a 921,600-iteration per-pixel scalar loop!
  if (!tiled && !expand32 && !fmt.expand16ToFloat && (fmt.blockBytes == 1u || endian == 0u)) {
    const size_t srcStride = static_cast<size_t>(pitchTexels) * fmt.blockBytes;
    const size_t srcXOff = static_cast<size_t>(x0) * fmt.blockBytes;
    for (uint32_t y = 0; y < height; ++y) {
      const size_t rowSrcOff = static_cast<size_t>(y + y0) * srcStride + srcXOff;
      uint8_t* rowDst = dst + static_cast<size_t>(y) * rowBytes;
      if (rowSrcOff + rowBytes <= maxBytes) {
        std::memcpy(rowDst, src + rowSrcOff, rowBytes);
      } else if (rowSrcOff < maxBytes) {
        const size_t avail = maxBytes - rowSrcOff;
        std::memcpy(rowDst, src + rowSrcOff, avail);
        std::memset(rowDst + avail, 0, rowBytes - avail);
      } else {
        std::memset(rowDst, 0, rowBytes);
      }
    }
    return totalBytes;
  }

  for (uint32_t y = 0; y < height; ++y) {
    uint8_t* rowDst = dst + static_cast<size_t>(y) * rowBytes;
    for (uint32_t x = 0; x < width; ++x) {
      const size_t off = tiled
                             ? TiledOffset2D(x + x0, y + y0, pitchAligned, bppLog2)
                             : static_cast<size_t>((y + y0) * pitchTexels + (x + x0)) * fmt.blockBytes;
      uint8_t* d = rowDst + static_cast<size_t>(x) * outBpp;
      if (off + fmt.blockBytes > maxBytes) {
        std::memset(d, 0, outBpp);
        continue;
      }
      const uint8_t* s = src + off;
      if (expand32) {
        uint16_t raw = 0;
        std::memcpy(&raw, s, 2);
        if (endian == 1u || endian == 2u) raw = ByteSwap16(raw);
        uint8_t rgba[4] = {0, 0, 0, 255};
        if (fmt.expand565) {
          rgba[0] = static_cast<uint8_t>((((raw >> 11) & 0x1Fu) * 255u + 15u) / 31u);
          rgba[1] = static_cast<uint8_t>((((raw >> 5) & 0x3Fu) * 255u + 31u) / 63u);
          rgba[2] = static_cast<uint8_t>(((raw & 0x1Fu) * 255u + 15u) / 31u);
        } else if (fmt.expand4444) {
          rgba[0] = static_cast<uint8_t>(((raw >> 8) & 0xFu) * 17u);
          rgba[1] = static_cast<uint8_t>(((raw >> 4) & 0xFu) * 17u);
          rgba[2] = static_cast<uint8_t>((raw & 0xFu) * 17u);
          rgba[3] = static_cast<uint8_t>(((raw >> 12) & 0xFu) * 17u);
        } else if (fmt.expand1555) {
          rgba[0] = static_cast<uint8_t>((((raw >> 10) & 0x1Fu) * 255u + 15u) / 31u);
          rgba[1] = static_cast<uint8_t>((((raw >> 5) & 0x1Fu) * 255u + 15u) / 31u);
          rgba[2] = static_cast<uint8_t>(((raw & 0x1Fu) * 255u + 15u) / 31u);
          rgba[3] = (raw & 0x8000u) ? 255u : 0u;
        }
        std::memcpy(d, rgba, 4);
      } else if (fmt.expand16ToFloat) {
        const uint32_t comps = fmt.blockBytes / 2u;
        for (uint32_t c = 0; c < comps; ++c) {
          uint16_t raw = 0;
          std::memcpy(&raw, s + c * 2u, 2);
          if (endian == 1u || endian == 2u) raw = ByteSwap16(raw);
          const float f = static_cast<float>(raw) / 65535.0f;
          std::memcpy(d + c * 4u, &f, 4);
        }
      } else if (fmt.blockBytes == 1u) {
        *d = *s;
      } else if (fmt.blockBytes == 2u) {
        uint16_t raw = 0;
        std::memcpy(&raw, s, 2);
        if (endian == 1u || endian == 2u) raw = ByteSwap16(raw);
        std::memcpy(d, &raw, 2);
      } else {
        const uint32_t words = fmt.blockBytes / 4u;
        for (uint32_t w = 0; w < words; ++w) {
          uint32_t val = 0;
          std::memcpy(&val, s + w * 4u, 4);
          val = ApplyEndian32(val, endian);
          std::memcpy(d + w * 4u, &val, 4);
        }
      }
    }
  }
  return totalBytes;
}

GLenum WrapMode(uint32_t clamp) {
  switch (clamp) {
    case 0: return GL_REPEAT;
    case 1: return GL_MIRRORED_REPEAT;
    case 2: return GL_CLAMP_TO_EDGE;
    case 3: return kGlMirrorClampToEdge;
    case 4: return GL_CLAMP_TO_EDGE;
    case 5: return kGlMirrorClampToEdge;
    default: return kGlClampToBorder;
  }
}

}  // namespace

GLenum Gles3Resources::GlColorFormat(uint32_t format, uint32_t& bytesPerSample) {
  bytesPerSample = 4;
  switch (format) {
    case 0:
    case 1: return GL_RGBA8;
    case 2:
    case 10: return GL_RGB10_A2;
    case 3:
    case 12: return GL_RGBA16F;
    case 4:
    case 6: return GL_RG16F;
    case 5:
    case 7:
      bytesPerSample = 8;
      return GL_RGBA16F;
    case 14: return GL_R32F;
    case 15:
      bytesPerSample = 8;
      return GL_RG32F;
    default: return GL_RGBA8;
  }
}

void Gles3Resources::Initialize(rex::IPhysicalMemory* memory, rex::IPhysicalWriteWatch* watch) {
  Shutdown();
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

  const char* exts = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
  hasS3tc_ = HasExtension(exts, "GL_EXT_texture_compression_s3tc");
  hasRgtc_ = HasExtension(exts, "GL_EXT_texture_compression_rgtc");
  hasNorm16_ = HasExtension(exts, "GL_EXT_texture_norm16");
  hasBorderClamp_ = HasExtension(exts, "GL_EXT_texture_border_clamp");
  hasMirrorClamp_ = HasExtension(exts, "GL_EXT_texture_mirror_clamp_to_edge");
  hasFloatLinear_ = HasExtension(exts, "GL_OES_texture_float_linear");
  if (HasExtension(exts, "GL_EXT_texture_filter_anisotropic")) {
    glGetFloatv(kGlMaxTextureMaxAnisotropy, &maxDeviceAniso_);
  } else {
    maxDeviceAniso_ = 1.0f;
  }

  // Fallback 1x1 white 2D and Cube textures.
  const uint32_t white = 0xFFFFFFFFu;
  glGenTextures(1, &whiteTex2D_);
  glBindTexture(GL_TEXTURE_2D, whiteTex2D_);
  glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 1, 1);
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, &white);

  glGenTextures(1, &whiteTexCube_);
  glBindTexture(GL_TEXTURE_CUBE_MAP, whiteTexCube_);
  glTexStorage2D(GL_TEXTURE_CUBE_MAP, 1, GL_RGBA8, 1, 1);
  for (int face = 0; face < 6; ++face) {
    glTexSubImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, 0, 0, 0, 1, 1, GL_RGBA,
                    GL_UNSIGNED_BYTE, &white);
  }
  glBindTexture(GL_TEXTURE_2D, 0);
  glBindTexture(GL_TEXTURE_CUBE_MAP, 0);

  glGenSamplers(1, &linearClampSampler_);
  glSamplerParameteri(linearClampSampler_, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glSamplerParameteri(linearClampSampler_, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glSamplerParameteri(linearClampSampler_, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glSamplerParameteri(linearClampSampler_, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  glGenSamplers(1, &nearestClampSampler_);
  glSamplerParameteri(nearestClampSampler_, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glSamplerParameteri(nearestClampSampler_, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glSamplerParameteri(nearestClampSampler_, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glSamplerParameteri(nearestClampSampler_, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  glGenTextures(1, &gammaTex_);
  glBindTexture(GL_TEXTURE_2D, gammaTex_);
  glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA16F, 256, 1);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glBindTexture(GL_TEXTURE_2D, 0);

  GLint ssboAlign = 64;
  glGetIntegerv(GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT, &ssboAlign);
  ssboAlign_ = std::max<size_t>(static_cast<size_t>(ssboAlign), 16u);
  using BufferStorageExtFn = void (*)(GLenum target, GLsizeiptr size, const void* data,
                                      GLbitfield flags);
  auto glBufferStorageExt =
      reinterpret_cast<BufferStorageExtFn>(eglGetProcAddress("glBufferStorageEXT"));
  if (glBufferStorageExt && HasExtension(exts, "GL_EXT_buffer_storage")) {
    constexpr GLbitfield kPersist = GL_MAP_WRITE_BIT | 0x0040u | 0x0080u;
    glGenBuffers(1, &vbRing_);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, vbRing_);
    glBufferStorageExt(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(vbRingSize_), nullptr,
                       kPersist);
    vbRingPtr_ = static_cast<uint8_t*>(
        glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, static_cast<GLsizeiptr>(vbRingSize_),
                         kPersist));
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    if (!vbRingPtr_) {
      glDeleteBuffers(1, &vbRing_);
      vbRing_ = 0;
    }
  }
  vbRingOffset_ = 0;
  vbHalf_ = 0;

  GLESLOG("Gles3Resources initialized (s3tc=%d rgtc=%d norm16=%d maxAniso=%.1f vbRing=%d)",
          hasS3tc_, hasRgtc_, hasNorm16_, maxDeviceAniso_, vbRingPtr_ ? 1 : 0);
}

void Gles3Resources::DestroyScaledTargets() {
  for (auto& [k, ct] : colorTargets_) {
    if (ct.tex) glDeleteTextures(1, &ct.tex);
  }
  colorTargets_.clear();
  for (auto& [k, dt] : depthTargets_) {
    if (dt.tex) glDeleteTextures(1, &dt.tex);
  }
  depthTargets_.clear();
  for (auto& [k, r] : resolved_) {
    if (r.tex) glDeleteTextures(1, &r.tex);
  }
  resolved_.clear();
  lastFrontbuffer_ = nullptr;
}

void Gles3Resources::Shutdown() {
  DestroyScaledTargets();
  for (auto& [k, t] : textures_) {
    if (t.tex) glDeleteTextures(1, &t.tex);
  }
  textures_.clear();
  for (auto& [k, vb] : vertexBuffers_) {
    if (vb.ssbo) glDeleteBuffers(1, &vb.ssbo);
  }
  vertexBuffers_.clear();
  for (auto& [k, ib] : indexBuffers_) {
    if (ib.ebo) glDeleteBuffers(1, &ib.ebo);
  }
  indexBuffers_.clear();
  for (auto& [k, s] : samplers_) {
    if (s) glDeleteSamplers(1, &s);
  }
  samplers_.clear();
  classRows_.clear();

  if (whiteTex2D_) { glDeleteTextures(1, &whiteTex2D_); whiteTex2D_ = 0; }
  if (whiteTexCube_) { glDeleteTextures(1, &whiteTexCube_); whiteTexCube_ = 0; }
  if (linearClampSampler_) { glDeleteSamplers(1, &linearClampSampler_); linearClampSampler_ = 0; }
  if (nearestClampSampler_) { glDeleteSamplers(1, &nearestClampSampler_); nearestClampSampler_ = 0; }
  if (gammaTex_) { glDeleteTextures(1, &gammaTex_); gammaTex_ = 0; }
  for (int i = 0; i < 2; ++i) {
    if (vbHalfFence_[i]) { glDeleteSync(vbHalfFence_[i]); vbHalfFence_[i] = nullptr; }
  }
  if (vbRing_) {
    if (vbRingPtr_) {
      glBindBuffer(GL_SHADER_STORAGE_BUFFER, vbRing_);
      glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
      glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
      vbRingPtr_ = nullptr;
    }
    glDeleteBuffers(1, &vbRing_);
    vbRing_ = 0;
  }
  vbRingOffset_ = 0;
  vbHalf_ = 0;
  gammaValid_ = false;
  gammaVersion_ = 0;
  memory_ = nullptr;
  watch_ = nullptr;
  physBase_ = nullptr;
  physSize_ = 0;
}

const uint8_t* Gles3Resources::PhysicalPtr(uint32_t physAddr, size_t size) const {
  const uint32_t addr = physAddr & kPhysicalMask;
  if (!physBase_ || static_cast<size_t>(addr) + size > physSize_) return nullptr;
  return physBase_ + addr;
}

void Gles3Resources::RequestScale(float scale) {
  requestedScale_ = std::clamp(scale, 0.25f, 4.0f);
}

void Gles3Resources::SetAnisotropy(uint32_t aniso) {
  const uint32_t clamped = std::clamp(aniso, 1u, 16u);
  if (clamped == anisotropy_) return;
  anisotropy_ = clamped;
  for (auto& [k, s] : samplers_) {
    if (s) glDeleteSamplers(1, &s);
  }
  samplers_.clear();
}

bool Gles3Resources::BeginFrame(uint64_t frame) {
  frame_ = frame;
  // Periodically evict stale one-shot ring vertex buffers & unused textures so we never accumulate
  // thousands of Adreno kgsl mmap VMAs (Linux vm.max_map_count = 65530).
  if ((frame_ & 15u) == 0u && vertexBuffers_.size() > 128u) {
    uint32_t evicted = 0;
    for (auto it = vertexBuffers_.begin(); it != vertexBuffers_.end() && evicted < 4u;) {
      if (frame_ > it->second.lastFrame + 45u) {
        if (it->second.ssbo) glDeleteBuffers(1, &it->second.ssbo);
        it = vertexBuffers_.erase(it);
        ++evicted;
      } else {
        ++it;
      }
    }
  }
  if ((frame_ & 31u) == 0u && indexBuffers_.size() > 128u) {
    uint32_t evicted = 0;
    for (auto it = indexBuffers_.begin(); it != indexBuffers_.end() && evicted < 4u;) {
      if (frame_ > it->second.lastFrame + 90u) {
        if (it->second.ebo) glDeleteBuffers(1, &it->second.ebo);
        it = indexBuffers_.erase(it);
        ++evicted;
      } else {
        ++it;
      }
    }
  }
  if ((frame_ & 127u) == 0u && textures_.size() > 256u) {
    uint32_t evicted = 0;
    for (auto it = textures_.begin(); it != textures_.end() && evicted < 4u;) {
      if (frame_ > it->second.lastFrame + 900u) {
        if (it->second.tex) glDeleteTextures(1, &it->second.tex);
        it = textures_.erase(it);
        ++evicted;
      } else {
        ++it;
      }
    }
  }
  if (std::fabs(requestedScale_ - scale_) > 0.01f) {
    scale_ = requestedScale_;
    DestroyScaledTargets();
    GLESLOG("live internal render scale updated -> %.3fx (no restart)", scale_);
    return true;
  }
  return false;
}

uint32_t Gles3Resources::RowsForClass(uint32_t pitch, uint32_t cls) const {
  if (pitch == 0) return 0;
  uint32_t rows = pitch >= 1024u ? 720u : std::max(pitch, 360u);
  const uint64_t key = (static_cast<uint64_t>(pitch) << 2) | (cls & 3u);
  if (auto it = classRows_.find(key); it != classRows_.end()) {
    rows = std::max(rows, it->second);
  }
  const uint32_t maxRows = (10u << 20) / (pitch * 4u);
  return std::min(rows, std::min(maxRows, 1440u));
}

bool Gles3Resources::EnsureRows(uint32_t surfaceInfo, uint32_t rows) {
  const uint32_t pitch = surfaceInfo & 0x3FFFu;
  const uint32_t cls = ((surfaceInfo >> 16) & 3u) != 0 ? 1u : 0u;
  if (pitch == 0 || rows == 0 || rows > 4096u) return false;
  rows = std::min(rows, 1440u);
  if (rows <= RowsForClass(pitch, cls)) return false;
  const uint32_t aligned = std::min((rows + 63u) & ~63u, 1440u);
  const uint64_t key = (static_cast<uint64_t>(pitch) << 2) | cls;
  auto it = classRows_.find(key);
  if (it != classRows_.end() && it->second >= aligned) return false;
  classRows_[key] = aligned;

  bool invalidated = false;
  for (auto ct = colorTargets_.begin(); ct != colorTargets_.end();) {
    if (ct->second.pitch == pitch && ((ct->second.msaa != 0 ? 1u : 0u) == cls) &&
        ct->second.rows < aligned) {
      if (ct->second.tex) glDeleteTextures(1, &ct->second.tex);
      ct = colorTargets_.erase(ct);
      invalidated = true;
    } else {
      ++ct;
    }
  }
  for (auto dt = depthTargets_.begin(); dt != depthTargets_.end();) {
    if (dt->second.pitch == pitch && ((dt->second.msaa != 0 ? 1u : 0u) == cls) &&
        dt->second.rows < aligned) {
      if (dt->second.tex) glDeleteTextures(1, &dt->second.tex);
      dt = depthTargets_.erase(dt);
      invalidated = true;
    } else {
      ++dt;
    }
  }
  return invalidated;
}

void Gles3Resources::TargetSize(uint32_t surfaceInfo, uint32_t /*bytesPerSample*/,
                                uint32_t& widthOut, uint32_t& heightOut, float& scaleXOut,
                                float& scaleYOut) const {
  const uint32_t pitch = std::max<uint32_t>(1u, surfaceInfo & 0x3FFFu);
  const uint32_t cls = ((surfaceInfo >> 16) & 3u) != 0 ? 1u : 0u;
  const uint32_t rows = RowsForClass(pitch, cls);
  widthOut = std::clamp<uint32_t>(static_cast<uint32_t>(std::lround(pitch * scale_)), 1u, 4096u);
  heightOut = std::clamp<uint32_t>(static_cast<uint32_t>(std::lround(rows * scale_)), 1u, 4096u);
  scaleXOut = static_cast<float>(widthOut) / static_cast<float>(pitch);
  scaleYOut = static_cast<float>(heightOut) / static_cast<float>(rows);
}

const Gles3Resources::ColorTarget* Gles3Resources::ColorTargetFor(uint32_t colorInfo,
                                                                  uint32_t surfaceInfo) {
  const uint32_t base = colorInfo & 0xFFFu;
  const uint32_t rawFmt = (colorInfo >> 16) & 0xFu;
  const uint32_t canonFmt =
      (rawFmt == 12u) ? 3u : (rawFmt == 10u) ? 2u : (rawFmt == 1u) ? 0u : rawFmt;
  const uint32_t pitch = surfaceInfo & 0x3FFFu;
  const uint32_t msaa = (surfaceInfo >> 16) & 3u;
  const uint32_t cls = msaa != 0 ? 1u : 0u;
  if (pitch == 0) return nullptr;

  const uint64_t key = (static_cast<uint64_t>(base) << 32) | (static_cast<uint64_t>(pitch) << 12) |
                       (static_cast<uint64_t>(canonFmt) << 4) | cls;
  auto it = colorTargets_.find(key);
  if (it != colorTargets_.end()) {
    it->second.lastFrame = frame_;
    return &it->second;
  }

  uint32_t bps = 4;
  const GLenum glFmt = GlColorFormat(canonFmt, bps);
  uint32_t w = 0, h = 0;
  float sx = 1.0f, sy = 1.0f;
  TargetSize(surfaceInfo, bps, w, h, sx, sy);

  ColorTarget ct{};
  ct.internalFormat = glFmt;
  ct.width = w;
  ct.height = h;
  ct.edramBase = base;
  ct.pitch = pitch;
  ct.rows = RowsForClass(pitch, cls);
  ct.msaa = msaa;
  ct.format = canonFmt;
  ct.lastFrame = frame_;

  glGenTextures(1, &ct.tex);
  glBindTexture(GL_TEXTURE_2D, ct.tex);
  glTexStorage2D(GL_TEXTURE_2D, 1, glFmt, static_cast<GLsizei>(w), static_cast<GLsizei>(h));
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glBindTexture(GL_TEXTURE_2D, 0);

  auto [ins, _] = colorTargets_.emplace(key, ct);
  return &ins->second;
}

const Gles3Resources::ColorTarget* Gles3Resources::PrimaryColorTargetAt(uint32_t colorInfo,
                                                                        uint32_t minPitch,
                                                                        uint32_t cls) const {
  const uint32_t base = colorInfo & 0xFFFu;
  const uint32_t rawFmt = (colorInfo >> 16) & 0xFu;
  const uint32_t canonFmt =
      (rawFmt == 12u) ? 3u : (rawFmt == 10u) ? 2u : (rawFmt == 1u) ? 0u : rawFmt;
  const ColorTarget* best = nullptr;
  uint32_t bestPitch = minPitch;
  int bestScore = -1;
  for (const auto& [k, ct] : colorTargets_) {
    if (ct.edramBase != base || !ct.tex) continue;
    if (ct.pitch <= minPitch) continue;
    const uint32_t ctCls = ct.msaa != 0 ? 1u : 0u;
    const int score = ((ctCls == cls) ? 4 : 0) + ((ct.format == canonFmt) ? 2 : 0);
    if (score > bestScore || (score == bestScore && ct.pitch > bestPitch)) {
      bestScore = score;
      bestPitch = ct.pitch;
      best = &ct;
    }
  }
  return best;
}

const Gles3Resources::DepthTarget* Gles3Resources::DepthTargetFor(uint32_t depthInfo,
                                                                  uint32_t surfaceInfo) {
  const uint32_t base = depthInfo & 0xFFFu;
  const uint32_t pitch = surfaceInfo & 0x3FFFu;
  const uint32_t msaa = (surfaceInfo >> 16) & 3u;
  const uint32_t cls = msaa != 0 ? 1u : 0u;
  if (pitch == 0) return nullptr;

  const uint64_t key = (static_cast<uint64_t>(base) << 32) | (static_cast<uint64_t>(pitch) << 12) | cls;
  auto it = depthTargets_.find(key);
  if (it != depthTargets_.end()) {
    it->second.lastFrame = frame_;
    return &it->second;
  }

  uint32_t w = 0, h = 0;
  float sx = 1.0f, sy = 1.0f;
  TargetSize(surfaceInfo, 4, w, h, sx, sy);

  DepthTarget dt{};
  dt.width = w;
  dt.height = h;
  dt.edramBase = base;
  dt.pitch = pitch;
  dt.rows = RowsForClass(pitch, cls);
  dt.msaa = msaa;
  dt.lastFrame = frame_;

  glGenTextures(1, &dt.tex);
  glBindTexture(GL_TEXTURE_2D, dt.tex);
  glTexStorage2D(GL_TEXTURE_2D, 1, GL_DEPTH24_STENCIL8, static_cast<GLsizei>(w),
                 static_cast<GLsizei>(h));
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_DEPTH_STENCIL_TEXTURE_MODE, GL_DEPTH_COMPONENT);
  glBindTexture(GL_TEXTURE_2D, 0);

  auto [ins, _] = depthTargets_.emplace(key, dt);
  return &ins->second;
}

const Gles3Resources::DepthTarget* Gles3Resources::PrimaryDepthTargetAt(uint32_t depthInfo,
                                                                        uint32_t minPitch,
                                                                        uint32_t cls) const {
  const uint32_t base = depthInfo & 0xFFFu;
  const DepthTarget* best = nullptr;
  uint32_t bestPitch = minPitch;
  int bestScore = -1;
  for (const auto& [k, dt] : depthTargets_) {
    if (dt.edramBase != base || !dt.tex) continue;
    if (dt.pitch <= minPitch) continue;
    const uint32_t dtCls = dt.msaa != 0 ? 1u : 0u;
    const int score = (dtCls == cls) ? 2 : 0;
    if (score > bestScore || (score == bestScore && dt.pitch > bestPitch)) {
      bestScore = score;
      bestPitch = dt.pitch;
      best = &dt;
    }
  }
  return best;
}

Gles3Resources::Resolved* Gles3Resources::ResolvedFor(uint32_t base, uint32_t width,
                                                      uint32_t height, GLenum internalFormat,
                                                      bool isDepth) {
  const uint32_t addr = base & 0x1FFFF000u;
  auto it = resolved_.find(addr);
  if (it != resolved_.end()) {
    Resolved& r = it->second;
    if (r.width == width && r.height == height && r.internalFormat == internalFormat &&
        r.isDepth == isDepth && r.tex != 0) {
      r.lastFrame = frame_;
      return &r;
    }
    if (r.tex) glDeleteTextures(1, &r.tex);
    if (lastFrontbuffer_ == &r) lastFrontbuffer_ = nullptr;
  }

  Resolved r{};
  r.internalFormat = internalFormat;
  r.width = width;
  r.height = height;
  r.base = addr;
  r.isDepth = isDepth;
  r.lastFrame = frame_;

  glGenTextures(1, &r.tex);
  glBindTexture(GL_TEXTURE_2D, r.tex);
  glTexStorage2D(GL_TEXTURE_2D, 1, internalFormat, static_cast<GLsizei>(width),
                 static_cast<GLsizei>(height));
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glBindTexture(GL_TEXTURE_2D, 0);

  auto [ins, _] = resolved_.insert_or_assign(addr, r);
  return &ins->second;
}

const Gles3Resources::Resolved* Gles3Resources::ResolvedAt(uint32_t base) const {
  const uint32_t addr = base & 0x1FFFF000u;
  auto it = resolved_.find(addr);
  return it != resolved_.end() ? &it->second : nullptr;
}

void Gles3Resources::UploadTextureData(GuestTexture& tex, const uint32_t words[6]) {
  const uint32_t fmtId = words[1] & 0x3Fu;
  const GlTexFmt fmt = DescribeGlFormat(fmtId, hasS3tc_, hasRgtc_, hasNorm16_);
  if (!fmt.supported) return;

  const uint32_t dimension = (words[5] >> 9) & 3u;
  const bool isCube = (dimension == 3u);
  const uint32_t endian = (words[1] >> 6) & 3u;
  const bool tiled = ((words[0] >> 31) & 1u) != 0;
  const bool packedMips = ((words[5] >> 11) & 1u) != 0;
  const uint32_t pitchTexels = std::max<uint32_t>(((words[0] >> 22) & 0x1FFu) * 32u, 32u);
  const uint32_t width = (words[2] & 0x1FFFu) + 1u;
  const uint32_t height = ((words[2] >> 13) & 0x1FFFu) + 1u;
  const uint32_t baseAddr = (words[1] & 0xFFFFF000u) & kPhysicalMask;
  const uint32_t mipAddr = (words[5] & 0xFFFFF000u) & kPhysicalMask;
  const uint32_t maxMip = std::min<uint32_t>((words[4] >> 6) & 0xFu, 12u);

  const uint8_t* physBase = physBase_;
  const size_t totalMem = physSize_;
  if (!physBase || baseAddr >= totalMem) return;

  uint32_t mipCount = 1;
  const bool pow2 = (width & (width - 1u)) == 0u && (height & (height - 1u)) == 0u;
  if (!isCube && mipAddr != 0 && maxMip > 0 && pow2) {
    uint32_t mw = width, mh = height;
    while (mipCount <= maxMip && mw > 16u && mh > 16u) {
      mw = std::max(1u, mw >> 1);
      mh = std::max(1u, mh >> 1);
      ++mipCount;
    }
  }

  const uint32_t blockDiv = fmt.compressed ? 4u : 1u;
  uint32_t mipOffsets[16] = {};
  uint32_t mipBytes[16] = {};
  uint32_t mipPitchTexels[16] = {};
  uint32_t mipOffset = 0;
  for (uint32_t level = 1; level < mipCount && level < 16u; ++level) {
    const uint32_t w = std::max(width >> level, 1u);
    const uint32_t h = std::max(height >> level, 1u);
    const uint32_t bw = (w + blockDiv - 1u) / blockDiv;
    const uint32_t bh = (h + blockDiv - 1u) / blockDiv;
    const uint32_t pb = (bw + 31u) & ~31u;
    const uint32_t rb = (bh + 31u) & ~31u;
    const uint32_t levelBytes = pb * rb * fmt.blockBytes;
    if (static_cast<size_t>(mipAddr) + mipOffset + levelBytes > totalMem) {
      mipCount = level;
      break;
    }
    mipOffsets[level] = mipOffset;
    mipBytes[level] = levelBytes;
    mipPitchTexels[level] = pb * blockDiv;
    mipOffset += levelBytes;
  }

  const uint32_t texW = fmt.compressed ? ((width + 3u) & ~3u) : width;
  const uint32_t texH = fmt.compressed ? ((height + 3u) & ~3u) : height;
  const GLenum target = isCube ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D;
  const bool needAlloc = (tex.tex == 0 || tex.target != target || tex.width != texW ||
                          tex.height != texH || tex.mipLevels != mipCount);
  glActiveTexture(GL_TEXTURE31);
  if (needAlloc) {
    if (tex.tex) glDeleteTextures(1, &tex.tex);
    glGenTextures(1, &tex.tex);
    glBindTexture(target, tex.tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexStorage2D(target, static_cast<GLsizei>(mipCount), fmt.internalFormat,
                   static_cast<GLsizei>(texW), static_cast<GLsizei>(texH));
  } else {
    glBindTexture(target, tex.tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  }

  const uint32_t faces = isCube ? 6u : 1u;
  const uint32_t baseBlocksWide = (width + blockDiv - 1u) / blockDiv;
  const uint32_t baseBlocksHigh = (height + blockDiv - 1u) / blockDiv;
  const uint32_t pitchBlocks = std::max(pitchTexels / blockDiv, 1u);
  const uint32_t rowsBlocksAligned = tiled ? ((baseBlocksHigh + 31u) & ~31u) : baseBlocksHigh;
  const size_t faceBytes =
      static_cast<size_t>(std::max(pitchBlocks, baseBlocksWide)) * rowsBlocksAligned * fmt.blockBytes;

  uint32_t x0 = 0, y0 = 0;
  PackedBaseOffset(packedMips, width, height, x0, y0);

  for (uint32_t face = 0; face < faces; ++face) {
    const GLenum uploadTarget = isCube ? (GL_TEXTURE_CUBE_MAP_POSITIVE_X + face) : GL_TEXTURE_2D;
    const size_t fOff = static_cast<size_t>(baseAddr) + face * faceBytes;
    if (fOff < totalMem) {
      const size_t bytes =
          LinearizeLevel(physBase + fOff, totalMem - fOff, fmt, endian, width, height,
                         pitchTexels, tiled, x0, y0, texScratch_);
      if (fmt.compressed) {
        glCompressedTexSubImage2D(uploadTarget, 0, 0, 0, static_cast<GLsizei>(texW),
                                  static_cast<GLsizei>(texH), fmt.internalFormat,
                                  static_cast<GLsizei>(bytes), texScratch_.data());
      } else {
        glTexSubImage2D(uploadTarget, 0, 0, 0, static_cast<GLsizei>(width),
                        static_cast<GLsizei>(height), fmt.format, fmt.type, texScratch_.data());
      }
    }
  }

  for (uint32_t level = 1; level < mipCount && level < 16u; ++level) {
    const uint32_t w = std::max(width >> level, 1u);
    const uint32_t h = std::max(height >> level, 1u);
    const uint32_t lw = fmt.compressed ? ((w + 3u) & ~3u) : w;
    const uint32_t lh = fmt.compressed ? ((h + 3u) & ~3u) : h;
    const size_t mOff = static_cast<size_t>(mipAddr) + mipOffsets[level];
    if (mOff + mipBytes[level] <= totalMem) {
      const size_t bytes =
          LinearizeLevel(physBase + mOff, mipBytes[level], fmt, endian, w, h,
                         mipPitchTexels[level], tiled, 0, 0, texScratch_);
      if (fmt.compressed) {
        glCompressedTexSubImage2D(GL_TEXTURE_2D, static_cast<GLint>(level), 0, 0,
                                  static_cast<GLsizei>(lw), static_cast<GLsizei>(lh),
                                  fmt.internalFormat, static_cast<GLsizei>(bytes),
                                  texScratch_.data());
      } else {
        glTexSubImage2D(GL_TEXTURE_2D, static_cast<GLint>(level), 0, 0,
                        static_cast<GLsizei>(w), static_cast<GLsizei>(h),
                        fmt.format, fmt.type, texScratch_.data());
      }
    }
  }

  glTexParameteri(target, GL_TEXTURE_BASE_LEVEL, 0);
  glTexParameteri(target, GL_TEXTURE_MAX_LEVEL, static_cast<GLint>(mipCount - 1));
  glBindTexture(target, 0);

  tex.target = target;
  tex.width = texW;
  tex.height = texH;
  tex.mipLevels = mipCount;
  tex.address = baseAddr;
  tex.compressed = fmt.compressed;
  tex.byteSize = static_cast<uint32_t>(std::min<size_t>(faceBytes * faces, totalMem - baseAddr));
  tex.fingerprint = Fingerprint(physBase + baseAddr, tex.byteSize);
  tex.lastCheckedFrame = frame_;
}

GLuint Gles3Resources::TextureFor(const uint32_t words[6], uint32_t& widthOut, uint32_t& heightOut,
                                  bool& isCubeOut, const Resolved*& resolvedOut) {
  resolvedOut = nullptr;
  const uint32_t fmtId = words[1] & 0x3Fu;
  const uint32_t baseAddr = (words[1] & 0xFFFFF000u) & kPhysicalMask;
  const uint32_t dimension = (words[5] >> 9) & 3u;
  isCubeOut = (dimension == 3u);
  widthOut = (words[2] & 0x1FFFu) + 1u;
  heightOut = ((words[2] >> 13) & 0x1FFFu) + 1u;

  if ((words[0] & 3u) != 2u || baseAddr == 0 || widthOut > 8192u || heightOut > 8192u ||
      dimension == 2u) {
    return isCubeOut ? whiteTexCube_ : whiteTex2D_;
  }

  if (!isCubeOut) {
    if (const Resolved* r = ResolvedAt(baseAddr)) {
      widthOut = r->guestWidth > 0 ? r->guestWidth : r->width;
      heightOut = r->guestHeight > 0 ? r->guestHeight : r->height;
      resolvedOut = r;
      return r->tex;
    }
  }

  // Match Windows texture key: (baseAddr, format | (dimension << 8), width, height)
  uint64_t key = 1469598103934665603ULL;
  const uint32_t kWords[4] = {baseAddr, fmtId | (dimension << 8), widthOut, heightOut};
  for (uint32_t kw : kWords) {
    key = (key ^ kw) * 1099511628211ULL;
  }

  auto it = textures_.find(key);
  if (it != textures_.end()) {
    GuestTexture& t = it->second;
    t.lastFrame = frame_;
    // Uncompressed textures (e.g. Bink video Y/Cr/Cb planes, dynamic UI) and recently mutated
    // textures must be checked every frame so 30fps videos never freeze or desync at 90-120 FPS.
    // Static compressed 3D world textures (DXT/BC) throttle after 16 consecutive unchanged frames.
    const bool isDynamic = !t.compressed || (frame_ <= t.dynamicUntilFrame);
    const bool needCheck =
        t.byteSize > 0 &&
        (isDynamic || t.stableCount < 16u ? (t.lastCheckedFrame != frame_)
                                          : (frame_ >= t.lastCheckedFrame + 30u));
    if (needCheck) {
      t.lastCheckedFrame = frame_;
      if (const uint8_t* ptr = PhysicalPtr(t.address, t.byteSize)) {
        const uint64_t fp = Fingerprint(ptr, t.byteSize);
        if (fp != t.fingerprint) {
          t.stableCount = 0;
          t.dynamicUntilFrame = frame_ + 300u;
          UploadTextureData(t, words);
        } else if (!isDynamic && t.stableCount < 16u) {
          ++t.stableCount;
        }
      }
    }
    if (t.tex) return t.tex;
    return isCubeOut ? whiteTexCube_ : whiteTex2D_;
  }

  GuestTexture& t = textures_[key];
  t.lastFrame = frame_;
  UploadTextureData(t, words);
  return t.tex ? t.tex : (isCubeOut ? whiteTexCube_ : whiteTex2D_);
}

GLuint Gles3Resources::VertexBufferFor(uint32_t address, uint32_t bytes, size_t& offsetOut,
                                       size_t& rangeSizeOut) {
  offsetOut = 0;
  rangeSizeOut = 0;
  const uint32_t phys = address & kPhysicalMask;
  const uint32_t size = std::max<uint32_t>((bytes + 15u) & ~15u, 16u);
  if (phys == 0) return 0;

  const uint8_t* ptr = PhysicalPtr(phys, size);
  if (!ptr) return 0;

  auto allocVbRing = [&](const uint8_t* src, uint32_t len) -> GLuint {
    if (!vbRingPtr_ || len == 0 || len > vbRingSize_ / 4u) return 0;
    vbRingOffset_ = (vbRingOffset_ + ssboAlign_ - 1u) & ~(ssboAlign_ - 1u);
    const size_t halfSize = vbRingSize_ / 2u;
    const size_t halfEnd = vbHalf_ ? vbRingSize_ : halfSize;
    if (vbRingOffset_ + len > halfEnd) {
      if (vbHalfFence_[vbHalf_]) glDeleteSync(vbHalfFence_[vbHalf_]);
      vbHalfFence_[vbHalf_] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
      vbHalf_ ^= 1;
      if (vbHalfFence_[vbHalf_]) {
        glClientWaitSync(vbHalfFence_[vbHalf_], GL_SYNC_FLUSH_COMMANDS_BIT, 500000000ULL);
        glDeleteSync(vbHalfFence_[vbHalf_]);
        vbHalfFence_[vbHalf_] = nullptr;
      }
      vbRingOffset_ = vbHalf_ ? halfSize : 0u;
    }
    const size_t off = vbRingOffset_;
    std::memcpy(vbRingPtr_ + off, src, len);
    vbRingOffset_ = off + len;
    offsetOut = off;
    rangeSizeOut = len;
    return vbRing_;
  };

  // Fast path: tiny dynamic vertex buffers (<= 8 KB: particles, UI quads, decals) stream directly
  // into the persistent coherent SSBO ring with ZERO glBufferSubData stalls and ZERO map lookups!
  if (size <= 8192u && vbRingPtr_) {
    return allocVbRing(ptr, size);
  }

  const uint64_t key = static_cast<uint64_t>(phys);
  auto it = vertexBuffers_.find(key);
  if (it != vertexBuffers_.end()) {
    VertexBuffer& vb = it->second;
    vb.lastFrame = frame_;
    const uint32_t wanted = std::max(size, vb.uploadedBytes);
    if (frame_ <= vb.volatileUntilFrame && size <= 65536u && vbRingPtr_) {
      return allocVbRing(ptr, size);
    }
    if (wanted > vb.bytes) {
      const uint32_t newCap = (wanted + wanted / 4u + 4095u) & ~4095u;
      const uint32_t uploadLen =
          static_cast<uint32_t>(std::min<size_t>(newCap, physSize_ - phys));
      vb.bytes = newCap;
      vb.uploadedBytes = wanted;
      vb.fingerprint = Fingerprint(ptr, wanted);
      vb.lastCheckedFrame = frame_;
      glBindBuffer(GL_SHADER_STORAGE_BUFFER, vb.ssbo);
      glBufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(newCap), nullptr,
                   GL_DYNAMIC_DRAW);
      glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, static_cast<GLsizeiptr>(uploadLen), ptr);
      glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
      return vb.ssbo;
    }
    // Re-check immediately when growing or for recently mutated VBs.
    // Static mesh VBs only re-verify every 16 frames to avoid CPU RAM reads.
    const bool isVolatile = (frame_ <= vb.volatileUntilFrame);
    const bool needCheck =
        (wanted > vb.uploadedBytes) || isVolatile ||
        (vb.stableCount < 16u ? (vb.lastCheckedFrame != frame_)
                              : (frame_ >= vb.lastCheckedFrame + 16u));
    if (needCheck) {
      vb.lastCheckedFrame = frame_;
      const uint64_t fp = Fingerprint(ptr, wanted);
      if (wanted > vb.uploadedBytes || fp != vb.fingerprint) {
        if (fp != vb.fingerprint && wanted <= vb.uploadedBytes) {
          vb.volatileUntilFrame = frame_ + 180u;
          vb.stableCount = 0;
          if (size <= 65536u && vbRingPtr_) {
            return allocVbRing(ptr, size);
          }
        }
        vb.uploadedBytes = wanted;
        vb.fingerprint = fp;
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, vb.ssbo);
        // Orphan before upload so Adreno never stalls waiting on in-flight draws
        glBufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(vb.bytes), nullptr,
                     GL_DYNAMIC_DRAW);
        glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, static_cast<GLsizeiptr>(wanted), ptr);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
      } else if (vb.stableCount < 16u) {
        ++vb.stableCount;
      }
    }
    return vb.ssbo;
  }

  // Hard cap live SSBO count so even rapid ring-buffer sub-allocations within a frame never
  // exhaust Adreno kgsl mmap slots.
  if (vertexBuffers_.size() >= 1024u) {
    for (auto vit = vertexBuffers_.begin(); vit != vertexBuffers_.end();) {
      if (vit->second.lastFrame < frame_) {
        if (vit->second.ssbo) glDeleteBuffers(1, &vit->second.ssbo);
        vit = vertexBuffers_.erase(vit);
      } else {
        ++vit;
      }
    }
  }

  const uint32_t allocCap = (size + size / 8u + 4095u) & ~4095u;
  const uint32_t uploadLen =
      static_cast<uint32_t>(std::min<size_t>(allocCap, physSize_ - phys));
  VertexBuffer& vb = vertexBuffers_[key];
  vb.address = phys;
  vb.bytes = allocCap;
  vb.uploadedBytes = size;
  vb.fingerprint = Fingerprint(ptr, size);
  vb.lastFrame = frame_;
  vb.lastCheckedFrame = frame_;
  glGenBuffers(1, &vb.ssbo);
  glBindBuffer(GL_SHADER_STORAGE_BUFFER, vb.ssbo);
  glBufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(allocCap), nullptr,
               size <= 262144u ? GL_DYNAMIC_DRAW : GL_STATIC_DRAW);
  glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, static_cast<GLsizeiptr>(uploadLen), ptr);
  glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
  return vb.ssbo;
}

GLuint Gles3Resources::StaticIndexBufferFor(uint32_t address, uint32_t count, bool wide,
                                            bool resetEnabled, uint32_t resetIdx) {
  const uint32_t phys = address & kPhysicalMask;
  if (phys == 0 || count < 64u) return 0;
  const uint32_t elemBytes = wide ? 4u : 2u;
  const uint32_t rawBytes = count * elemBytes;
  const uint8_t* ptr = PhysicalPtr(phys, rawBytes);
  if (!ptr) return 0;

  uint64_t key = 1469598103934665603ULL;
  const uint32_t kWords[4] = {phys, count, (wide ? 1u : 0u) | (resetEnabled ? 2u : 0u),
                              resetEnabled ? resetIdx : 0u};
  for (uint32_t kw : kWords) key = (key ^ kw) * 1099511628211ULL;

  auto it = indexBuffers_.find(key);
  if (it != indexBuffers_.end()) {
    StaticIndexBuffer& ib = it->second;
    ib.lastFrame = frame_;
    if (ib.ebo) {
      if (frame_ >= ib.lastCheckedFrame + 16u) {
        ib.lastCheckedFrame = frame_;
        const uint64_t fp = Fingerprint(ptr, rawBytes);
        if (fp != ib.fingerprint) {
          glDeleteBuffers(1, &ib.ebo);
          ib.ebo = 0;
          ib.fingerprint = fp;
          ib.stableCount = 0;
          return 0;
        }
      }
      return ib.ebo;
    }
    if (ib.lastCheckedFrame != frame_) {
      ib.lastCheckedFrame = frame_;
      const uint64_t fp = Fingerprint(ptr, rawBytes);
      if (fp != ib.fingerprint) {
        ib.fingerprint = fp;
        ib.stableCount = 0;
        return 0;
      }
      if (++ib.stableCount < 2u) return 0;

      idxScratch_.resize(count);
      if (wide) {
        const auto* src = reinterpret_cast<const uint32_t*>(ptr);
        const uint32_t r = resetIdx & 0xFFFFFFu;
        for (uint32_t i = 0; i < count; ++i) {
          const uint32_t idx = ByteSwap32(src[i]) & 0xFFFFFFu;
          idxScratch_[i] = (resetEnabled && idx == r) ? 0xFFFFFFFFu : idx;
        }
      } else {
        const auto* src = reinterpret_cast<const uint16_t*>(ptr);
        const uint32_t r = resetIdx & 0xFFFFu;
        for (uint32_t i = 0; i < count; ++i) {
          const uint32_t idx = ByteSwap16(src[i]);
          idxScratch_[i] = (resetEnabled && idx == r) ? 0xFFFFFFFFu : idx;
        }
      }
      glGenBuffers(1, &ib.ebo);
      glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ib.ebo);
      glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(count) * 4,
                   idxScratch_.data(), GL_STATIC_DRAW);
      return ib.ebo;
    }
    return 0;
  }

  if (indexBuffers_.size() >= 1024u) {
    for (auto iit = indexBuffers_.begin(); iit != indexBuffers_.end();) {
      if (iit->second.lastFrame < frame_) {
        if (iit->second.ebo) glDeleteBuffers(1, &iit->second.ebo);
        iit = indexBuffers_.erase(iit);
      } else {
        ++iit;
      }
    }
  }

  StaticIndexBuffer& ib = indexBuffers_[key];
  ib.address = phys;
  ib.count = count;
  ib.rawBytes = rawBytes;
  ib.fingerprint = Fingerprint(ptr, rawBytes);
  ib.lastFrame = frame_;
  ib.lastCheckedFrame = frame_;
  ib.stableCount = 1u;
  return 0;
}

GLuint Gles3Resources::SamplerFor(const uint32_t words[6]) {
  const uint32_t clampX = (words[0] >> 10) & 7u;
  const uint32_t clampY = (words[0] >> 13) & 7u;
  const uint32_t clampZ = (words[0] >> 16) & 7u;
  uint32_t magFilter = (words[3] >> 19) & 3u;
  uint32_t minFilter = (words[3] >> 21) & 3u;
  uint32_t mipFilter = (words[3] >> 23) & 3u;
  if (magFilter == 3u) magFilter = 1u;
  if (minFilter == 3u) minFilter = 1u;
  if (mipFilter == 3u) mipFilter = 1u;
  const uint32_t borderColor = words[5] & 3u;
  const uint64_t key = static_cast<uint64_t>(clampX) | (static_cast<uint64_t>(clampY) << 3) |
                       (static_cast<uint64_t>(clampZ) << 6) | (static_cast<uint64_t>(magFilter) << 9) |
                       (static_cast<uint64_t>(minFilter) << 11) | (static_cast<uint64_t>(mipFilter) << 13) |
                       (static_cast<uint64_t>(borderColor) << 15) | (static_cast<uint64_t>(anisotropy_) << 20);
  if (auto it = samplers_.find(key); it != samplers_.end()) return it->second;

  GLuint s = 0;
  glGenSamplers(1, &s);
  auto wrap = [&](uint32_t c) -> GLenum {
    GLenum m = WrapMode(c);
    if (m == kGlClampToBorder && !hasBorderClamp_) m = GL_CLAMP_TO_EDGE;
    if (m == kGlMirrorClampToEdge && !hasMirrorClamp_) m = GL_CLAMP_TO_EDGE;
    return m;
  };
  glSamplerParameteri(s, GL_TEXTURE_WRAP_S, static_cast<GLint>(wrap(clampX)));
  glSamplerParameteri(s, GL_TEXTURE_WRAP_T, static_cast<GLint>(wrap(clampY)));
  glSamplerParameteri(s, GL_TEXTURE_WRAP_R, static_cast<GLint>(wrap(clampZ)));

  const bool noMips = (mipFilter == 2u);
  const bool magLin = (magFilter != 0u);
  const bool minLin = (minFilter != 0u);
  const GLenum glMag = magLin ? GL_LINEAR : GL_NEAREST;
  GLenum glMin = minLin ? GL_LINEAR : GL_NEAREST;
  if (!noMips) {
    if (mipFilter == 0u) {
      glMin = minLin ? GL_LINEAR_MIPMAP_NEAREST : GL_NEAREST_MIPMAP_NEAREST;
    } else if (mipFilter == 1u) {
      glMin = minLin ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_LINEAR;
    }
  }
  glSamplerParameteri(s, GL_TEXTURE_MAG_FILTER, static_cast<GLint>(glMag));
  glSamplerParameteri(s, GL_TEXTURE_MIN_FILTER, static_cast<GLint>(glMin));
  glSamplerParameterf(s, GL_TEXTURE_MIN_LOD, 0.0f);
  glSamplerParameterf(s, GL_TEXTURE_MAX_LOD, noMips ? 0.0f : 1000.0f);
  if (!noMips && maxDeviceAniso_ > 1.0f && anisotropy_ > 1u && minLin) {
    glSamplerParameterf(s, kGlTextureMaxAnisotropy,
                        std::min(maxDeviceAniso_, static_cast<float>(anisotropy_)));
  }
  if (hasBorderClamp_) {
    const float white = (borderColor == 1u) ? 1.0f : 0.0f;
    float bc[4] = {white, white, white, white};
    glSamplerParameterfv(s, 0x1004 /* GL_TEXTURE_BORDER_COLOR */, bc);
  }
  samplers_[key] = s;
  return s;
}

void Gles3Resources::UpdateGammaLut(const uint32_t table[256], uint64_t version) {
  if (version == gammaVersion_ && gammaValid_) return;
  float rgba[256 * 4];
  for (int i = 0; i < 256; ++i) {
    const uint32_t v = table[i];
    rgba[i * 4 + 0] = static_cast<float>((v >> 20) & 0x3FFu) / 1023.0f;
    rgba[i * 4 + 1] = static_cast<float>((v >> 10) & 0x3FFu) / 1023.0f;
    rgba[i * 4 + 2] = static_cast<float>(v & 0x3FFu) / 1023.0f;
    rgba[i * 4 + 3] = 1.0f;
  }
  glBindTexture(GL_TEXTURE_2D, gammaTex_);
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 1, GL_RGBA, GL_FLOAT, rgba);
  glBindTexture(GL_TEXTURE_2D, 0);
  gammaVersion_ = version;
  gammaValid_ = true;
}

}  // namespace bs1::renderer::gles3
