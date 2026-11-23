#pragma once

#include <GLES3/gl32.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <vector>

#include <rex/guest.h>

namespace bs1::renderer::gles3 {

/**
 * Manages GLES 3.2 render targets, resolved surfaces, tiled guest textures,
 * SSBO vertex buffers, index buffers, samplers, and gamma LUT with live
 * zero-restart internal resolution scaling.
 */
class Gles3Resources final : public rex::IPhysicalWriteListener {
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
    mutable uint64_t drawGen = 0;
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
    mutable uint64_t drawGen = 0;
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
    // Set when a resolve used glCopyImageSubData with a non-unit exponent bias /
    // a clamp mode. The shader path applied these at resolve time; for the fast
    // path they are deferred to sample time via texScale/texBias/texFlags.
    int8_t expBias = 0;
    uint8_t clampMode = 0;  // 0 = none, 1 = [0,1], 2 = [-1,1]
    uint32_t lastWriteSeq = 0;
    uint64_t lastFrame = 0;
    mutable uint32_t hwSwizzle = 0x688u;
    GLuint lastSrcTex = 0;
    uint64_t lastSrcDrawGen = 0;
    int32_t lastSrcX0 = 0;
    int32_t lastSrcY0 = 0;
    uint32_t lastHostW = 0;
    uint32_t lastHostH = 0;
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
    uint64_t dynamicUntilFrame = 0;
    uint32_t stableCount = 0;
    uint32_t uploadSeq = 0;
    uint32_t rewriteStreak = 0;
    bool compressed = false;
    bool hwSrgb = false;
    bool watchArmed = false;
    uint64_t watchToken = 0;
    mutable uint32_t hwSwizzle = 0x688u;
  };

  struct VertexBuffer {
    GLuint ssbo = 0;
    GLuint tbo = 0;  // GL_TEXTURE_BUFFER texture for Mali (no VS SSBO)
    uint32_t address = 0;
    uint32_t bytes = 0;
    uint32_t cap = 0;
    uint32_t uploadedBytes = 0;
    uint64_t fingerprint = 0;
    uint64_t lastFrame = 0;
    uint64_t lastCheckedFrame = 0;
    uint64_t volatileUntilFrame = 0;
    uint32_t stableCount = 0;
    uint32_t uploadSeq = 0;
    uint32_t rewriteStreak = 0;
    bool watchArmed = false;
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
    uint64_t volatileUntilFrame = 0;
    uint32_t stableCount = 0;
    uint32_t uploadSeq = 0;
    uint32_t rewriteStreak = 0;
    bool watchArmed = false;
  };

  void Initialize(rex::IPhysicalMemory* memory, rex::IPhysicalWriteWatch* watch);
  void Shutdown();
  void SetUseVertexTextureBuffers(bool enabled) { useVertexTextureBuffers_ = enabled; }

  void RequestScale(float scale);
  float ActiveScale() const { return scale_; }
  void SetAnisotropy(uint32_t aniso);

  /** Applies pending scale change on frame boundary. Returns true if targets were rebuilt. */
  bool BeginFrame(uint64_t frame);

  bool EnsureRows(uint32_t surfaceInfo, uint32_t rows);
  void TargetSize(uint32_t surfaceInfo, uint32_t bytesPerSample, uint32_t& widthOut,
                  uint32_t& heightOut, float& scaleXOut, float& scaleYOut) const;

  const ColorTarget* ColorTargetFor(uint32_t colorInfo, uint32_t surfaceInfo);
  const ColorTarget* PrimaryColorTargetAt(uint32_t colorInfo, uint32_t pitch, uint32_t cls,
                                          int32_t clearBottom) const;
  const DepthTarget* DepthTargetFor(uint32_t depthInfo, uint32_t surfaceInfo);
  const DepthTarget* PrimaryDepthTargetAt(uint32_t depthInfo, uint32_t pitch, uint32_t cls,
                                          int32_t clearBottom) const;

  Resolved* ResolvedFor(uint32_t base, uint32_t width, uint32_t height, GLenum internalFormat,
                        bool isDepth);
  const Resolved* ResolvedAt(uint32_t base) const;
  inline bool IsResolvedDepth(uint32_t base) const {
    if (resolvedDepthCount_ == 0u) return false;
    const uint32_t addr = base & 0x1FFFF000u;
    for (uint32_t i = 0; i < resolvedDepthCount_; ++i) {
      if (resolvedDepthAddrs_[i] == addr) return true;
    }
    return false;
  }
  uint32_t CurrentWriteSeq() const { return writeSeq_.load(std::memory_order_acquire); }
  bool CheckRangeClean(uint32_t physAddr, uint32_t bytes, uint32_t& lastSeq, bool& watchArmed);
  const Resolved* LastFrontbuffer() const { return lastFrontbuffer_; }
  void SetLastFrontbuffer(const Resolved* r) { lastFrontbuffer_ = r; }

  /** Returns GL texture handle + target (2D/Cube) for guest texture fetch words[6]. */
  GLuint TextureFor(const uint32_t words[6], uint32_t& widthOut, uint32_t& heightOut,
                    bool& isCubeOut, const Resolved*& resolvedOut,
                    bool* hwSrgbOut = nullptr, uint32_t** hwSwizzlePtrOut = nullptr);

  GLuint VertexBufferFor(uint32_t address, uint32_t bytes, size_t& offsetOut,
                         size_t& rangeSizeOut, GLuint* tboOut = nullptr);
  /** Returns a cached GL_ELEMENT_ARRAY_BUFFER for static indexed draws, or 0 for dynamic/volatile. */
  GLuint StaticIndexBufferFor(uint32_t address, uint32_t count, bool wide, bool resetEnabled,
                              uint32_t resetIdx);
  GLuint SamplerFor(const uint32_t words[6]);
  const uint8_t* PhysicalPtr(uint32_t physAddr, size_t size) const;

  GLuint WhiteTexture2D() const { return whiteTex2D_; }
  GLuint WhiteTextureCube() const { return whiteTexCube_; }
  GLuint LinearClampSampler() const { return linearClampSampler_; }
  GLuint NearestClampSampler() const { return nearestClampSampler_; }
  GLuint GammaTexture() const { return gammaTex_; }
  void UpdateGammaLut(const uint32_t table[256], uint64_t version);
  bool HasGammaLut() const { return gammaValid_; }

  bool HasS3tc() const { return hasS3tc_; }
  bool HasRgtc() const { return hasRgtc_; }
  bool HasFloatLinear() const { return hasFloatLinear_; }
  uint32_t TakeTboUploads() {
    uint32_t v = tboUploads_;
    tboUploads_ = 0;
    return v;
  }
  uint32_t TakeTboNews() {
    uint32_t v = tboNews_;
    tboNews_ = 0;
    return v;
  }

 private:
  rex::PhysicalRange OnPhysicalWrite(rex::PhysicalRange range, bool exact) override;
  bool RangeDirtiedSince(uint32_t start, uint32_t end, uint32_t afterSeq) const;
  bool NeedsRefresh(uint32_t physStart, uint32_t physEnd, uint32_t& uploadSeq,
                    uint64_t prevUsedFrame, uint64_t volatileUntilFrame, bool& watchArmed,
                    bool& watchHitOut) const;
  void FinishRefresh(uint32_t addr, uint32_t size, bool changed, bool watchHit,
                     uint64_t prevUsedFrame, uint32_t& rewriteStreak,
                     uint64_t& volatileUntilFrame, bool& watchArmed);
  void ArmWatch(uint32_t start, uint32_t size);

  void DestroyScaledTargets();
  void ClearColorTextureZero(GLuint tex);
  void UploadTextureData(GuestTexture& tex, const uint32_t words[6], bool wantSrgb);
  void UploadVertexTbo(VertexBuffer& vb, const uint8_t* ptr, uint32_t exactSize);
  void RecycleVertexBufferGl(VertexBuffer& vb, bool allowPool);
  static void DeleteVertexBufferGl(VertexBuffer& vb);
  uint32_t RowsForClass(uint32_t pitch, uint32_t cls) const;
  static GLenum GlColorFormat(uint32_t xenosFmt, uint32_t& bytesPerSample);

  static constexpr uint32_t kPhysPages = (512u << 20) >> 12;
  static constexpr uint32_t kPhysChunks = (512u << 20) >> 16;

  rex::IPhysicalMemory* memory_ = nullptr;
  rex::IPhysicalWriteWatch* watch_ = nullptr;
  rex::ScopeGuard watchGuard_;
  std::unique_ptr<std::atomic<uint32_t>[]> pageSeq_;
  std::unique_ptr<std::atomic<uint32_t>[]> chunkSeq_;
  std::atomic<uint32_t> writeSeq_{1};
  const uint8_t* physBase_ = nullptr;
  size_t physSize_ = 0;

  float scale_ = 1.0f;
  float requestedScale_ = 1.0f;
  uint32_t anisotropy_ = 4;
  float maxDeviceAniso_ = 1.0f;
  bool hasS3tc_ = false;
  bool hasS3tcSrgb_ = false;
  bool hasRgtc_ = false;
  bool hasNorm16_ = false;
  bool hasBorderClamp_ = false;
  bool hasMirrorClamp_ = false;
  bool hasFloatLinear_ = false;
  bool useVertexTextureBuffers_ = false;
  uint32_t tboUploads_ = 0;
  uint32_t tboNews_ = 0;

  struct TboPair {
    GLuint buf = 0;
    GLuint tbo = 0;
  };
  struct RetiredTbo {
    uint64_t retiredFrame = 0;
    uint32_t size = 0;
    GLuint buf = 0;
    GLuint tbo = 0;
  };
  // GPU-safe retired queue: buffers wait 6 full frames before entering freeTboPool_ so Mali
  // never reads a buffer being overwritten by glBufferSubData (zero vertex flickering).
  std::vector<RetiredTbo> retiredTbos_;
  // Exact-size recycling pool of pre-attached (GL_BUFFER, GL_TEXTURE_BUFFER) pairs for Mali.
  std::unordered_map<uint32_t, std::vector<TboPair>> freeTboPool_;
  uint32_t freeTboPoolCount_ = 0;

  uint64_t frame_ = 0;
  std::unordered_map<uint64_t, uint32_t> classRows_;
  std::unordered_map<uint64_t, ColorTarget> colorTargets_;
  std::unordered_map<uint64_t, DepthTarget> depthTargets_;
  std::unordered_map<uint32_t, Resolved> resolved_;
  uint32_t resolvedDepthAddrs_[16] = {};
  uint32_t resolvedDepthCount_ = 0;
  void RebuildResolvedDepthList();
  const Resolved* lastFrontbuffer_ = nullptr;

  std::unordered_map<uint64_t, GuestTexture> textures_;
  std::unordered_map<uint64_t, VertexBuffer> vertexBuffers_;
  std::unordered_map<uint64_t, StaticIndexBuffer> indexBuffers_;
  std::unordered_map<uint64_t, GLuint> samplers_;

  // Direct-mapped inline L1 caches to avoid std::unordered_map bucket pointer-chasing in hot draw loops.
  // Always cleared whenever any entry is erased from the backing map.
  struct VbL1Entry {
    uint64_t key = 0;
    VertexBuffer* ptr = nullptr;
  };
  struct IbL1Entry {
    uint64_t key = 0;
    StaticIndexBuffer* ptr = nullptr;
  };
  VbL1Entry vbL1_[256] = {};
  IbL1Entry ibL1_[256] = {};
  uint64_t lastColorKey_ = 0;
  ColorTarget* lastColorPtr_ = nullptr;
  uint64_t lastDepthKey_ = 0;
  DepthTarget* lastDepthPtr_ = nullptr;
  void ClearVbL1() { std::memset(vbL1_, 0, sizeof(vbL1_)); }
  void ClearIbL1() { std::memset(ibL1_, 0, sizeof(ibL1_)); }
  void ClearTargetL1() {
    lastColorPtr_ = nullptr;
    lastDepthPtr_ = nullptr;
  }

  std::vector<uint8_t> texScratch_;
  std::vector<uint32_t> idxScratch_;

  GLuint whiteTex2D_ = 0;
  GLuint whiteTexCube_ = 0;
  GLuint linearClampSampler_ = 0;
  GLuint nearestClampSampler_ = 0;
  GLuint gammaTex_ = 0;
  GLuint clearFbo_ = 0;
  uint64_t gammaVersion_ = 0;
  bool gammaValid_ = false;

  // Persistent coherent mapped ring buffer for volatile/dynamic SSBO / TBO vertex buffers
  // (eliminates per-draw glBufferSubData GPU pipeline stalls on both Adreno and Mali).
  GLuint vbRing_ = 0;
  GLuint vbRingTbo_ = 0;
  uint8_t* vbRingPtr_ = nullptr;
  size_t vbRingSize_ = 16u * 1024u * 1024u;
  size_t vbRingOffset_ = 0;
  size_t ssboAlign_ = 64;
  GLsync vbHalfFence_[2] = {nullptr, nullptr};
  int vbHalf_ = 0;
};

}  // namespace bs1::renderer::gles3
