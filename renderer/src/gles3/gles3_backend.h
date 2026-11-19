#pragma once

#include "gles3_resources.h"
#include "gles3_shaders.h"

#include <EGL/egl.h>
#include <GLES3/gl32.h>
#include <android/native_window.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <vector>

namespace bs1::renderer::gles3 {

/**
 * Single-threaded EGL + OpenGL ES 3.2 execution backend for Android.
 * Lives entirely on the PM4 GPU worker thread so there is zero cross-thread
 * eglMakeCurrent thrashing. Presents directly to the ANativeWindow surface
 * on PM4_XE_SWAP with Lanczos2 or Bilinear upscaling and Xenos gamma LUT.
 */
class Gles3Backend {
 public:
  void SetSurfaceWindow(ANativeWindow* window);
  void SetSurfaceSize(uint32_t width, uint32_t height);
  void SetCacheDir(const std::filesystem::path& cacheDir) { cacheDir_ = cacheDir; }

  // Called on the GPU worker thread:
  bool EnsureInitialized(rex::IPhysicalMemory* memory, rex::IPhysicalWriteWatch* watch);
  void ShutdownOnGpuThread();

  void UpdatePendingSurface();
  bool HasWindowSurface() const { return surface_ != EGL_NO_SURFACE; }

  Gles3Shaders& Shaders() { return shaders_; }
  Gles3Resources& Resources() { return resources_; }

  bool BeginFrame(uint64_t frame);
  bool ExecuteDraw(const uint32_t* regs, uint32_t initiator, uint32_t indexBase,
                   rex::IPhysicalMemory* memory);
  bool ExecuteResolve(const uint32_t* regs);
  void ExecutePresent(uint32_t fbAddr, uint32_t fbWidth, uint32_t fbHeight, bool useLanczos,
                      bool letterbox, bool vsync, const uint32_t* gammaLut, uint64_t gammaVersion,
                      bool hasGamma);

  uint32_t ObservedMainHeight() const { return observedMainHeight_; }
  void ObserveSwapHeight(uint32_t w, uint32_t h);

  // Per-window resolve breakdown, consumed by the Android PERF(10s) log.
  struct ResolveStats {
    uint32_t depth = 0;
    uint32_t color = 0;
    uint32_t directCopy = 0;
    uint32_t skipped = 0;
    uint32_t rejectScale = 0;
    uint32_t rejectClamp = 0;
    uint32_t rejectBounds = 0;
    uint64_t pixels = 0;
  };
  ResolveStats GetResolveStats() const { return resolveStats_; }
  void ResetResolveStats() { resolveStats_ = ResolveStats{}; }

 private:
  struct DrawConstantsCpu {
    float viewportScale[4];
    float viewportOffset[4];
    float targetSize[4];
    uint32_t flags[4];
    float texSize[32][4];
    uint32_t textureAdjustment[32][4];
    float pixelGen[4];
    float colorExpBias[4];
  };

  struct PresentParamsCpu {
    float uvRect[4];
    float params[4];
  };

  bool InitEgl();
  void DestroySurface();
  void UnbindTexturesMatching(GLuint tex);
  void ApplyBlendState(const uint32_t blendControl[4], uint32_t colorMask,
                       const float blendFactor[4]);
  void ApplyDepthStencilState(uint32_t depthControl, uint32_t stencilRefMask,
                              uint32_t stencilRefMaskBF, bool hasDepth, bool polygonal,
                              bool cullFront, bool cullBack);
  void ApplyRasterizerState(uint32_t suScModeControl, uint32_t clipControl, bool rectangles,
                            float slopeBias, float depthBiasUnits);

  std::mutex surfaceMutex_;
  ANativeWindow* pendingWindow_ = nullptr;
  bool windowDirty_ = false;
  std::atomic<uint32_t> surfaceWidth_{1280};
  std::atomic<uint32_t> surfaceHeight_{720};

  EGLDisplay display_ = EGL_NO_DISPLAY;
  EGLConfig config_ = nullptr;
  EGLContext context_ = EGL_NO_CONTEXT;
  EGLSurface surface_ = EGL_NO_SURFACE;
  EGLSurface pbuffer_ = EGL_NO_SURFACE;
  ANativeWindow* boundWindow_ = nullptr;
  bool eglReady_ = false;
  bool currentVsync_ = true;

  using ClipControlFn = void (*)(GLenum origin, GLenum depth);
  using BufferStorageExtFn = void (*)(GLenum target, GLsizeiptr size, const void* data,
                                      GLbitfield flags);
  ClipControlFn glClipControlExt_ = nullptr;
  BufferStorageExtFn glBufferStorageExt_ = nullptr;
  bool hasDepthClamp_ = false;
  bool hasBufferStorage_ = false;

  std::filesystem::path cacheDir_;
  Gles3Shaders shaders_;
  Gles3Resources resources_;

  GLuint emptyVao_ = 0;
  GLuint drawFbo_ = 0;
  GLuint resolveFbo_ = 0;
  GLuint uboRing_ = 0;
  uint8_t* uboRingPtr_ = nullptr;
  size_t uboRingSize_ = 16u * 1024u * 1024u;
  size_t uboRingOffset_ = 0;
  size_t uboAlign_ = 256;
  GLsync uboHalfFence_[2] = {nullptr, nullptr};
  int uboHalf_ = 0;
  GLuint uboPresent_ = 0;
  GLuint indexBuf_ = 0;
  uint8_t* indexBufPtr_ = nullptr;
  size_t indexBufCap_ = 4u * 1024u * 1024u;
  size_t indexRingOffset_ = 0;
  GLsync indexHalfFence_[2] = {nullptr, nullptr};
  int indexHalf_ = 0;

  void UploadUboPresent(const PresentParamsCpu& pp);

  GLuint boundTex_[24] = {};
  GLenum boundTexTarget_[24] = {};
  GLuint boundSampler_[24] = {};
  GLuint boundSsbo_[16] = {};
  size_t boundSsboOff_[16] = {};
  size_t boundSsboSize_[16] = {};
  Gles3Shaders::Handle lastVsH_ = 0;
  Gles3Shaders::Handle lastPsH_ = 0;
  bool lastRectProg_ = false;
  bool lastAlphaProg_ = false;
  GLuint lastLinkedProg_ = 0;
  GLuint lastFboColor_[4] = {};
  GLuint lastFboDepth_ = 0;
  bool drawFboBound_ = false;
  GLuint lastProg_ = 0;
  GLuint lastEbo_ = 0;
  bool stateDirty_ = true;
  uint32_t lastBlendCtrl_[4] = {};
  uint32_t lastColorMask_ = 0;
  float lastBlendFactor_[4] = {};
  uint32_t lastDepthCtrl_ = 0;
  uint32_t lastStencilRef_ = 0;
  uint32_t lastStencilRefBF_ = 0;
  bool lastHasDepth_ = false;
  bool lastDepthPoly_ = false;
  bool lastCullFront_ = false;
  bool lastCullBack_ = false;
  uint32_t lastSuSc_ = 0;
  uint32_t lastClipCtrl_ = 0;
  bool lastRects_ = false;
  float lastSlopeBias_ = 0.0f;
  float lastDepthBias_ = 0.0f;
  bool lastPrimRestart_ = false;
  uint32_t lastVpW_ = 0;
  uint32_t lastVpH_ = 0;
  int32_t lastScissor_[4] = {0, 0, 0, 0};
  bool lastScissorEnabled_ = false;
  bool boolsValid_ = false;
  uint32_t lastBools_[40] = {};
  bool drawConstValid_ = false;
  DrawConstantsCpu lastDrawConst_{};
  bool vsFloatsValid_ = false;
  size_t lastVsFloatBytes_ = 0;
  float lastVsFloats_[256 * 4] = {};
  bool psFloatsValid_ = false;
  size_t lastPsFloatBytes_ = 0;
  float lastPsFloats_[256 * 4] = {};

  uint32_t observedMainHeight_ = 720;
  DrawConstantsCpu drawConst_{};
  std::vector<uint32_t> scratchIdx_;
  std::vector<uint32_t> expandedIdx_;
  float packedVsFloats_[256 * 4] = {};
  float packedPsFloats_[256 * 4] = {};

  ResolveStats resolveStats_{};

  // Consecutive identical EDRAM->host resolves with no intervening draw are redundant.
  struct ResolveKey {
    uint32_t control = 0;
    uint32_t destBase = 0;
    uint32_t destInfo = 0;
    uint32_t surfaceInfo = 0;
    uint32_t tl = 0;
    uint32_t br = 0;
    uint32_t colorInfo[4] = {};
    uint32_t depthInfo = 0;
    bool frontbufferCandidate = false;
    bool valid = false;
    bool operator==(const ResolveKey& o) const {
      return valid && o.valid && control == o.control && destBase == o.destBase &&
             destInfo == o.destInfo && surfaceInfo == o.surfaceInfo && tl == o.tl && br == o.br &&
             depthInfo == o.depthInfo && frontbufferCandidate == o.frontbufferCandidate &&
             std::memcmp(colorInfo, o.colorInfo, sizeof(colorInfo)) == 0;
    }
  };
  ResolveKey lastResolveKey_{};
  uint32_t drawsSinceResolve_ = 0;
};

}  // namespace bs1::renderer::gles3
