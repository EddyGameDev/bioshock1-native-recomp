#pragma once

#include <GLES3/gl32.h>

#include <cstdint>
#include <unordered_map>
#include <vector>

#include <rex/guest.h>

namespace bs1::renderer::gles3 {

/**
 * Manages GLES 3.2 render targets, resolved surfaces, tiled guest textures,
 * SSBO vertex buffers, index buffers, samplers, and gamma LUT with live
 * zero-restart internal resolution scaling.
 */
class Gles3Resources {
 public:
  using Handle = uint32_t;

  struct ColorTarget {
    GLuint tex = 0;
    GLenum internalFormat = GL_RGBA8;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t edramBase = 0;
    uint32_t pitch = 0;
    uint32_t rows = 0;
    uint32_t msaa = 0;
    uint32_t format = 0;
    uint64_t lastFrame = 0;
  };

  struct DepthTarget {
    GLuint tex = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t edramBase = 0;
    uint32_t pitch = 0;
    uint32_t rows = 0;
    uint32_t msaa = 0;
    uint64_t lastFrame = 0;
  };

  struct Resolved {
    GLuint tex = 0;
    GLenum internalFormat = GL_RGBA8;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t guestWidth = 0;
    uint32_t guestHeight = 0;
    uint32_t base = 0;
    bool isDepth = false;
    bool rbSwapped = false;
    uint64_t lastFrame = 0;
  };

  struct GuestTexture {
    GLuint tex = 0;
    GLenum target = GL_TEXTURE_2D;
    uint32_t width = 1;
    uint32_t height = 1;
    uint32_t mipLevels = 1;
    uint32_t address = 0;
    uint32_t byteSize = 0;
    uint64_t fingerprint = 0;
    uint64_t lastFrame = 0;
    uint64_t lastCheckedFrame = 0;
    uint32_t stableCount = 0;
    uint64_t watchToken = 0;
  };

  struct VertexBuffer {
    GLuint ssbo = 0;
    uint32_t address = 0;
    uint32_t bytes = 0;
    uint32_t uploadedBytes = 0;
    uint64_t fingerprint = 0;
    uint64_t lastFrame = 0;
    uint64_t lastCheckedFrame = 0;
    uint64_t volatileUntilFrame = 0;
    uint32_t stableCount = 0;
    uint64_t watchToken = 0;
  };

  struct StaticIndexBuffer {
    GLuint ebo = 0;
    uint32_t address = 0;
    uint32_t count = 0;
    uint32_t rawBytes = 0;
    uint64_t fingerprint = 0;
    uint64_t lastFrame = 0;
    uint64_t lastCheckedFrame = 0;
    uint32_t stableCount = 0;
  };

  void Initialize(rex::IPhysicalMemory* memory, rex::IPhysicalWriteWatch* watch);
  void Shutdown();

  void RequestScale(float scale);
  float ActiveScale() const { return scale_; }
  void SetAnisotropy(uint32_t aniso);

  /** Applies pending scale change on frame boundary. Returns true if targets were rebuilt. */
  bool BeginFrame(uint64_t frame);

  bool EnsureRows(uint32_t surfaceInfo, uint32_t rows);
  void TargetSize(uint32_t surfaceInfo, uint32_t bytesPerSample, uint32_t& widthOut,
                  uint32_t& heightOut, float& scaleXOut, float& scaleYOut) const;

  const ColorTarget* ColorTargetFor(uint32_t colorInfo, uint32_t surfaceInfo);
  const ColorTarget* PrimaryColorTargetAt(uint32_t colorInfo, uint32_t pitch, uint32_t cls) const;
  const DepthTarget* DepthTargetFor(uint32_t depthInfo, uint32_t surfaceInfo);
  const DepthTarget* PrimaryDepthTargetAt(uint32_t depthInfo, uint32_t pitch, uint32_t cls) const;

  Resolved* ResolvedFor(uint32_t base, uint32_t width, uint32_t height, GLenum internalFormat,
                        bool isDepth);
  const Resolved* ResolvedAt(uint32_t base) const;
  const Resolved* LastFrontbuffer() const { return lastFrontbuffer_; }
  void SetLastFrontbuffer(const Resolved* r) { lastFrontbuffer_ = r; }

  /** Returns GL texture handle + target (2D/Cube) for guest texture fetch words[6]. */
  GLuint TextureFor(const uint32_t words[6], uint32_t& widthOut, uint32_t& heightOut,
                    bool& isCubeOut, const Resolved*& resolvedOut);

  GLuint VertexBufferFor(uint32_t address, uint32_t bytes, size_t& offsetOut,
                         size_t& rangeSizeOut);
  /** Returns a cached GL_ELEMENT_ARRAY_BUFFER for static indexed draws, or 0 for dynamic/volatile. */
  GLuint StaticIndexBufferFor(uint32_t address, uint32_t count, bool wide, bool resetEnabled,
                              uint32_t resetIdx);
  GLuint SamplerFor(const uint32_t words[6]);
  const uint8_t* PhysicalPtr(uint32_t physAddr, size_t size) const;

  GLuint WhiteTexture2D() const { return whiteTex2D_; }
  GLuint WhiteTextureCube() const { return whiteTexCube_; }
  GLuint LinearClampSampler() const { return linearClampSampler_; }
  GLuint GammaTexture() const { return gammaTex_; }
  void UpdateGammaLut(const uint32_t table[256], uint64_t version);
  bool HasGammaLut() const { return gammaValid_; }

  bool HasS3tc() const { return hasS3tc_; }
  bool HasRgtc() const { return hasRgtc_; }

 private:
  void DestroyScaledTargets();
  void UploadTextureData(GuestTexture& tex, const uint32_t words[6]);
  uint32_t RowsForClass(uint32_t pitch, uint32_t cls) const;
  static GLenum GlColorFormat(uint32_t xenosFmt, uint32_t& bytesPerSample);

  rex::IPhysicalMemory* memory_ = nullptr;
  rex::IPhysicalWriteWatch* watch_ = nullptr;
  const uint8_t* physBase_ = nullptr;
  size_t physSize_ = 0;

  float scale_ = 1.0f;
  float requestedScale_ = 1.0f;
  uint32_t anisotropy_ = 4;
  float maxDeviceAniso_ = 1.0f;
  bool hasS3tc_ = false;
  bool hasRgtc_ = false;
  bool hasNorm16_ = false;
  bool hasBorderClamp_ = false;
  bool hasMirrorClamp_ = false;

  uint64_t frame_ = 0;
  std::unordered_map<uint64_t, uint32_t> classRows_;
  std::unordered_map<uint64_t, ColorTarget> colorTargets_;
  std::unordered_map<uint64_t, DepthTarget> depthTargets_;
  std::unordered_map<uint32_t, Resolved> resolved_;
  const Resolved* lastFrontbuffer_ = nullptr;

  std::unordered_map<uint64_t, GuestTexture> textures_;
  std::unordered_map<uint64_t, VertexBuffer> vertexBuffers_;
  std::unordered_map<uint64_t, StaticIndexBuffer> indexBuffers_;
  std::unordered_map<uint64_t, GLuint> samplers_;

  std::vector<uint8_t> texScratch_;
  std::vector<uint32_t> idxScratch_;

  GLuint whiteTex2D_ = 0;
  GLuint whiteTexCube_ = 0;
  GLuint linearClampSampler_ = 0;
  GLuint gammaTex_ = 0;
  uint64_t gammaVersion_ = 0;
  bool gammaValid_ = false;

  // Persistent coherent mapped ring buffer for volatile/dynamic SSBO vertex buffers
  // (eliminates per-draw glBufferSubData GPU pipeline stalls on Adreno).
  GLuint vbRing_ = 0;
  uint8_t* vbRingPtr_ = nullptr;
  size_t vbRingSize_ = 16u * 1024u * 1024u;
  size_t vbRingOffset_ = 0;
  size_t ssboAlign_ = 64;
  GLsync vbHalfFence_[2] = {nullptr, nullptr};
  int vbHalf_ = 0;
};

}  // namespace bs1::renderer::gles3
