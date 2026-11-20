#include "gles3_backend.h"

#include <android/log.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#define GLESLOG(...) __android_log_print(ANDROID_LOG_INFO, "BS1-GLES3", __VA_ARGS__)
#define GLESERR(...) __android_log_print(ANDROID_LOG_ERROR, "BS1-GLES3", __VA_ARGS__)

namespace {
std::atomic<uint64_t> g_android_custom_present_count{0};
}

extern "C" __attribute__((visibility("default"))) uint64_t rex_android_get_custom_present_count() {
  return g_android_custom_present_count.load(std::memory_order_relaxed);
}

namespace bs1::renderer::gles3 {

namespace {

constexpr uint32_t kPhysicalMask = 0x1FFFFFFFu;
constexpr GLenum kGlLowerLeftExt = 0x8CA1;
constexpr GLenum kGlZeroToOneExt = 0x935F;
constexpr GLenum kGlDepthClampExt = 0x864F;
constexpr GLbitfield kGlMapPersistentBitExt = 0x0040u;
constexpr GLbitfield kGlMapCoherentBitExt = 0x0080u;

// Xenos register indices matching render_state.h:
constexpr uint32_t kRegSurfaceInfo = 0x2000;
constexpr uint32_t kRegColorInfo0 = 0x2001;
constexpr uint32_t kRegDepthInfo = 0x2002;
constexpr uint32_t kRegColorInfo1 = 0x2003;
constexpr uint32_t kRegColorInfo2 = 0x2004;
constexpr uint32_t kRegColorInfo3 = 0x2005;
constexpr uint32_t kRegWindowOffset = 0x2080;
constexpr uint32_t kRegScissorTopLeft = 0x2081;
constexpr uint32_t kRegScissorBottomRight = 0x2082;
constexpr uint32_t kRegIndexOffset = 0x2102;
constexpr uint32_t kRegResetIndex = 0x2103;
constexpr uint32_t kRegColorMask = 0x2104;
constexpr uint32_t kRegBlendRed = 0x2105;
constexpr uint32_t kRegStencilRefMaskBF = 0x210C;
constexpr uint32_t kRegStencilRefMask = 0x210D;
constexpr uint32_t kRegAlphaReference = 0x210E;
constexpr uint32_t kRegViewportXScale = 0x210F;
constexpr uint32_t kRegProgramControl = 0x2180;
constexpr uint32_t kRegContextMisc = 0x2181;
constexpr uint32_t kRegDepthControl = 0x2200;
constexpr uint32_t kRegBlendControl0 = 0x2201;
constexpr uint32_t kRegColorControl = 0x2202;
constexpr uint32_t kRegClipControl = 0x2204;
constexpr uint32_t kRegSuScModeControl = 0x2205;
constexpr uint32_t kRegVteControl = 0x2206;
constexpr uint32_t kRegModeControl = 0x2208;
constexpr uint32_t kRegBlendControl1 = 0x2209;
constexpr uint32_t kRegBlendControl2 = 0x220A;
constexpr uint32_t kRegBlendControl3 = 0x220B;
constexpr uint32_t kRegVertexControl = 0x2302;
constexpr uint32_t kRegCopyControl = 0x2318;
constexpr uint32_t kRegCopyDestBase = 0x2319;
constexpr uint32_t kRegCopyDestInfo = 0x231B;
constexpr uint32_t kRegCopyDepthClear = 0x231D;
constexpr uint32_t kRegCopyColorClear = 0x231E;
constexpr uint32_t kRegPolyOffsetFrontScale = 0x2380;
constexpr uint32_t kRegVertexConstants = 0x4000;
constexpr uint32_t kRegPixelConstants = 0x4400;
constexpr uint32_t kRegFetchConstants = 0x4800;
constexpr uint32_t kRegBoolConstants = 0x4900;

inline float RegFloat(const uint32_t* regs, uint32_t idx) {
  float f = 0.0f;
  std::memcpy(&f, &regs[idx], 4);
  return f;
}

inline uint16_t ByteSwap16(uint16_t v) { return static_cast<uint16_t>((v << 8) | (v >> 8)); }
inline uint32_t ByteSwap32(uint32_t v) { return __builtin_bswap32(v); }

GLenum GlCompare(uint32_t f) {
  switch (f & 7u) {
    case 0: return GL_NEVER;
    case 1: return GL_LESS;
    case 2: return GL_EQUAL;
    case 3: return GL_LEQUAL;
    case 4: return GL_GREATER;
    case 5: return GL_NOTEQUAL;
    case 6: return GL_GEQUAL;
    default: return GL_ALWAYS;
  }
}

GLenum GlStencilOp(uint32_t op) {
  switch (op & 7u) {
    case 0: return GL_KEEP;
    case 1: return GL_ZERO;
    case 2: return GL_REPLACE;
    case 3: return GL_INCR;
    case 4: return GL_DECR;
    case 5: return GL_INVERT;
    case 6: return GL_INCR_WRAP;
    case 7: return GL_DECR_WRAP;
    default: return GL_KEEP;
  }
}

GLenum GlBlendFactor(uint32_t f, bool isAlpha) {
  switch (f & 0x1Fu) {
    case 0: return GL_ZERO;
    case 1: return GL_ONE;
    case 4: return GL_SRC_COLOR;
    case 5: return GL_ONE_MINUS_SRC_COLOR;
    case 6: return GL_SRC_ALPHA;
    case 7: return GL_ONE_MINUS_SRC_ALPHA;
    case 8: return GL_DST_COLOR;
    case 9: return GL_ONE_MINUS_DST_COLOR;
    case 10: return GL_DST_ALPHA;
    case 11: return GL_ONE_MINUS_DST_ALPHA;
    case 12: return GL_CONSTANT_COLOR;
    case 13: return GL_ONE_MINUS_CONSTANT_COLOR;
    case 14: return GL_CONSTANT_ALPHA;
    case 15: return GL_ONE_MINUS_CONSTANT_ALPHA;
    case 16: return isAlpha ? GL_ONE : GL_SRC_ALPHA_SATURATE;
    default: return GL_ONE;
  }
}

GLenum GlBlendOp(uint32_t op) {
  switch (op & 7u) {
    case 0: return GL_FUNC_ADD;
    case 1: return GL_FUNC_SUBTRACT;
    case 2: return GL_MIN;
    case 3: return GL_MAX;
    case 4: return GL_FUNC_REVERSE_SUBTRACT;
    default: return GL_FUNC_ADD;
  }
}

}  // namespace

void Gles3Backend::SetSurfaceWindow(ANativeWindow* window) {
  std::lock_guard lock(surfaceMutex_);
  if (pendingWindow_) {
    ANativeWindow_release(pendingWindow_);
    pendingWindow_ = nullptr;
  }
  if (window) {
    ANativeWindow_acquire(window);
    pendingWindow_ = window;
    const int w = ANativeWindow_getWidth(window);
    const int h = ANativeWindow_getHeight(window);
    if (w > 0 && h > 0) {
      surfaceWidth_.store(static_cast<uint32_t>(w), std::memory_order_relaxed);
      surfaceHeight_.store(static_cast<uint32_t>(h), std::memory_order_relaxed);
    }
  }
  windowDirty_ = true;
}

void Gles3Backend::SetSurfaceSize(uint32_t width, uint32_t height) {
  if (width > 0 && height > 0) {
    surfaceWidth_.store(width, std::memory_order_relaxed);
    surfaceHeight_.store(height, std::memory_order_relaxed);
  }
}

void Gles3Backend::ObserveSwapHeight(uint32_t w, uint32_t h) {
  if (w >= 480 && h >= 240 && h <= 1080) {
    observedMainHeight_ = h;
  }
}

bool Gles3Backend::InitEgl() {
  if (eglReady_) return true;

  display_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (display_ == EGL_NO_DISPLAY) {
    GLESERR("eglGetDisplay failed");
    return false;
  }
  EGLint major = 0, minor = 0;
  if (!eglInitialize(display_, &major, &minor)) {
    GLESERR("eglInitialize failed (0x%x)", eglGetError());
    return false;
  }

  const EGLint cfgAttribs[] = {
      EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
      EGL_SURFACE_TYPE, EGL_WINDOW_BIT | EGL_PBUFFER_BIT,
      EGL_RED_SIZE, 8,
      EGL_GREEN_SIZE, 8,
      EGL_BLUE_SIZE, 8,
      EGL_ALPHA_SIZE, 8,
      EGL_NONE,
  };
  EGLint numConfigs = 0;
  if (!eglChooseConfig(display_, cfgAttribs, &config_, 1, &numConfigs) || numConfigs < 1) {
    GLESERR("eglChooseConfig failed (0x%x)", eglGetError());
    return false;
  }

  const EGLint ctxAttribs[] = {
      EGL_CONTEXT_MAJOR_VERSION, 3,
      EGL_CONTEXT_MINOR_VERSION, 2,
      EGL_NONE,
  };
  context_ = eglCreateContext(display_, config_, EGL_NO_CONTEXT, ctxAttribs);
  if (context_ == EGL_NO_CONTEXT) {
    GLESERR("eglCreateContext(GLES 3.2) failed (0x%x)", eglGetError());
    return false;
  }

  const EGLint pbufAttribs[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
  pbuffer_ = eglCreatePbufferSurface(display_, config_, pbufAttribs);
  if (pbuffer_ == EGL_NO_SURFACE) {
    GLESERR("eglCreatePbufferSurface failed (0x%x)", eglGetError());
    return false;
  }

  if (!eglMakeCurrent(display_, pbuffer_, pbuffer_, context_)) {
    GLESERR("eglMakeCurrent(pbuffer) failed (0x%x)", eglGetError());
    return false;
  }

  glClipControlExt_ = reinterpret_cast<ClipControlFn>(eglGetProcAddress("glClipControlEXT"));
  glBufferStorageExt_ =
      reinterpret_cast<BufferStorageExtFn>(eglGetProcAddress("glBufferStorageEXT"));
  const char* exts = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
  hasDepthClamp_ = exts && std::strstr(exts, "GL_EXT_depth_clamp") != nullptr;
  hasBufferStorage_ = glBufferStorageExt_ && exts &&
                      std::strstr(exts, "GL_EXT_buffer_storage") != nullptr;

  // Mali GPUs expose zero vertex shader storage blocks, but do support texture
  // buffers (GL_EXT_texture_buffer). Use samplerBuffer for guest vertex fetch.
  GLint maxVertexSSBOs = 1;
  glGetIntegerv(GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS, &maxVertexSSBOs);
  const bool useVertexTextureBuffers = maxVertexSSBOs <= 0;
  XenosGlsl::SetVertexTextureBuffers(useVertexTextureBuffers);

  if (glClipControlExt_) {
    glClipControlExt_(kGlLowerLeftExt, kGlZeroToOneExt);
  }
  GLint maxCombined = 0, maxVertTex = 0, maxFragTex = 0;
  glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &maxCombined);
  glGetIntegerv(GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS, &maxVertTex);
  glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &maxFragTex);
  GLESLOG("EGL %d.%d + GLES '%s' on '%s' (clipControl=%d depthClamp=%d bufStorage=%d "
          "texUnits=%d/%d/%d maxVSSSBO=%d)",
          major, minor,
          reinterpret_cast<const char*>(glGetString(GL_VERSION)),
          reinterpret_cast<const char*>(glGetString(GL_RENDERER)),
          glClipControlExt_ ? 1 : 0, hasDepthClamp_ ? 1 : 0, hasBufferStorage_ ? 1 : 0,
          maxCombined, maxVertTex, maxFragTex, maxVertexSSBOs);

  glGenVertexArrays(1, &emptyVao_);
  glBindVertexArray(emptyVao_);
  glGenFramebuffers(1, &drawFbo_);
  glGenFramebuffers(1, &resolveFbo_);

  GLint uboAlign = 256;
  glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &uboAlign);
  uboAlign_ = std::max<size_t>(static_cast<size_t>(uboAlign), 64u);

  const GLbitfield kPersistFlags =
      GL_MAP_WRITE_BIT | kGlMapPersistentBitExt | kGlMapCoherentBitExt;

  glGenBuffers(1, &uboRing_);
  glBindBuffer(GL_UNIFORM_BUFFER, uboRing_);
  uboRingPtr_ = nullptr;
  if (hasBufferStorage_) {
    glBufferStorageExt_(GL_UNIFORM_BUFFER, static_cast<GLsizeiptr>(uboRingSize_), nullptr,
                        kPersistFlags);
    uboRingPtr_ = static_cast<uint8_t*>(
        glMapBufferRange(GL_UNIFORM_BUFFER, 0, static_cast<GLsizeiptr>(uboRingSize_),
                         kPersistFlags));
  }
  if (!uboRingPtr_) {
    glBufferData(GL_UNIFORM_BUFFER, static_cast<GLsizeiptr>(uboRingSize_), nullptr, GL_STREAM_DRAW);
  }
  uboRingOffset_ = 0;
  uboHalf_ = 0;

  glGenBuffers(1, &uboPresent_);
  glBindBuffer(GL_UNIFORM_BUFFER, uboPresent_);
  glBufferData(GL_UNIFORM_BUFFER, sizeof(PresentParamsCpu), nullptr, GL_DYNAMIC_DRAW);
  glBindBuffer(GL_UNIFORM_BUFFER, 0);

  glGenBuffers(1, &indexBuf_);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, indexBuf_);
  indexBufPtr_ = nullptr;
  if (hasBufferStorage_) {
    glBufferStorageExt_(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(indexBufCap_), nullptr,
                        kPersistFlags);
    indexBufPtr_ = static_cast<uint8_t*>(
        glMapBufferRange(GL_ELEMENT_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(indexBufCap_),
                         kPersistFlags));
  }
  if (!indexBufPtr_) {
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(indexBufCap_), nullptr,
                 GL_STREAM_DRAW);
  }
  indexRingOffset_ = 0;
  indexHalf_ = 0;
  lastEbo_ = indexBuf_;
  GLESLOG("persistent ring maps: ubo=%d idx=%d", uboRingPtr_ ? 1 : 0, indexBufPtr_ ? 1 : 0);
  eglReady_ = true;
  return true;
}

bool Gles3Backend::EnsureInitialized(rex::IPhysicalMemory* memory,
                                     rex::IPhysicalWriteWatch* watch) {
  if (!InitEgl()) return false;
  if (shaders_.PresentLanczosProgram() == 0) {
    shaders_.Initialize(cacheDir_);
    resources_.Initialize(memory, watch);
  }
  UpdatePendingSurface();
  return true;
}

void Gles3Backend::DestroySurface() {
  if (surface_ != EGL_NO_SURFACE) {
    if (display_ != EGL_NO_DISPLAY && pbuffer_ != EGL_NO_SURFACE && context_ != EGL_NO_CONTEXT) {
      eglMakeCurrent(display_, pbuffer_, pbuffer_, context_);
    }
    eglDestroySurface(display_, surface_);
    surface_ = EGL_NO_SURFACE;
  }
  if (boundWindow_) {
    ANativeWindow_release(boundWindow_);
    boundWindow_ = nullptr;
  }
}

void Gles3Backend::UpdatePendingSurface() {
  if (!eglReady_) return;
  ANativeWindow* nextWin = nullptr;
  bool dirty = false;
  {
    std::lock_guard lock(surfaceMutex_);
    if (windowDirty_) {
      dirty = true;
      windowDirty_ = false;
      nextWin = pendingWindow_;
      pendingWindow_ = nullptr;
    }
  }
  if (!dirty) return;

  DestroySurface();
  if (nextWin) {
    surface_ = eglCreateWindowSurface(display_, config_, nextWin, nullptr);
    if (surface_ == EGL_NO_SURFACE) {
      GLESERR("eglCreateWindowSurface failed (0x%x)", eglGetError());
      ANativeWindow_release(nextWin);
      return;
    }
    boundWindow_ = nextWin;
    if (!eglMakeCurrent(display_, surface_, surface_, context_)) {
      GLESERR("eglMakeCurrent(window) failed (0x%x)", eglGetError());
      DestroySurface();
      return;
    }
    eglSwapInterval(display_, currentVsync_ ? 1 : 0);
    if (glClipControlExt_) glClipControlExt_(kGlLowerLeftExt, kGlZeroToOneExt);
    glBindVertexArray(emptyVao_);
    EGLint w = 0, h = 0;
    eglQuerySurface(display_, surface_, EGL_WIDTH, &w);
    eglQuerySurface(display_, surface_, EGL_HEIGHT, &h);
    if (w > 0 && h > 0) {
      surfaceWidth_.store(static_cast<uint32_t>(w), std::memory_order_relaxed);
      surfaceHeight_.store(static_cast<uint32_t>(h), std::memory_order_relaxed);
    }
    GLESLOG("attached ANativeWindow EGLSurface %dx%d", w, h);
  }
}

void Gles3Backend::ShutdownOnGpuThread() {
  if (!eglReady_) return;
  resources_.Shutdown();
  shaders_.Shutdown();
  if (emptyVao_) { glDeleteVertexArrays(1, &emptyVao_); emptyVao_ = 0; }
  if (drawFbo_) { glDeleteFramebuffers(1, &drawFbo_); drawFbo_ = 0; }
  if (resolveFbo_) { glDeleteFramebuffers(1, &resolveFbo_); resolveFbo_ = 0; }
  auto delFence = [](GLsync& f) {
    if (f) { glDeleteSync(f); f = nullptr; }
  };
  delFence(uboHalfFence_[0]);
  delFence(uboHalfFence_[1]);
  delFence(indexHalfFence_[0]);
  delFence(indexHalfFence_[1]);
  if (uboRing_ && uboRingPtr_) {
    glBindBuffer(GL_UNIFORM_BUFFER, uboRing_);
    glUnmapBuffer(GL_UNIFORM_BUFFER);
    uboRingPtr_ = nullptr;
  }
  if (indexBuf_ && indexBufPtr_) {
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, indexBuf_);
    glUnmapBuffer(GL_ELEMENT_ARRAY_BUFFER);
    indexBufPtr_ = nullptr;
  }
  auto delBuf = [](GLuint& b) { if (b) { glDeleteBuffers(1, &b); b = 0; } };
  delBuf(uboRing_);
  delBuf(uboPresent_);
  delBuf(indexBuf_);
  uboRingOffset_ = 0;
  indexRingOffset_ = 0;

  DestroySurface();
  if (display_ != EGL_NO_DISPLAY) {
    eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (pbuffer_ != EGL_NO_SURFACE) { eglDestroySurface(display_, pbuffer_); pbuffer_ = EGL_NO_SURFACE; }
    if (context_ != EGL_NO_CONTEXT) { eglDestroyContext(display_, context_); context_ = EGL_NO_CONTEXT; }
    eglTerminate(display_);
    display_ = EGL_NO_DISPLAY;
  }
  eglReady_ = false;
  std::lock_guard lock(surfaceMutex_);
  if (pendingWindow_) { ANativeWindow_release(pendingWindow_); pendingWindow_ = nullptr; }
  windowDirty_ = false;
}

bool Gles3Backend::BeginFrame(uint64_t frame) {
  UpdatePendingSurface();
  lastResolveKey_ = ResolveKey{};
  drawsSinceResolve_ = 0;
  if (resources_.BeginFrame(frame)) {
    for (uint32_t i = 0; i < 4; ++i) lastFboColor_[i] = 0;
    lastFboDepth_ = 0;
    drawFboBound_ = false;
    return true;
  }
  return false;
}

void Gles3Backend::UnbindTexturesMatching(GLuint tex) {
  if (!tex) return;
  for (uint32_t u = 0; u < 24; ++u) {
    if (boundTex_[u] == tex) {
      glActiveTexture(GL_TEXTURE0 + u);
      glBindTexture(boundTexTarget_[u], 0);
      glBindSampler(u, 0);
      boundTex_[u] = 0;
      boundSampler_[u] = 0;
    }
  }
}

void Gles3Backend::ApplyBlendState(const uint32_t blendControl[4], uint32_t colorMask,
                                   const float blendFactor[4]) {
  glBlendColor(blendFactor[0], blendFactor[1], blendFactor[2], blendFactor[3]);
  for (uint32_t i = 0; i < 4; ++i) {
    const uint32_t cm = (colorMask >> (i * 4)) & 0xFu;
    glColorMaski(i, (cm & 1u) != 0, (cm & 2u) != 0, (cm & 4u) != 0, (cm & 8u) != 0);

    const uint32_t bc = blendControl[i];
    const uint32_t srcRgb = bc & 0x1Fu;
    const uint32_t opRgb = (bc >> 5) & 7u;
    const uint32_t dstRgb = (bc >> 8) & 0x1Fu;
    const uint32_t srcA = (bc >> 16) & 0x1Fu;
    const uint32_t opA = (bc >> 21) & 7u;
    const uint32_t dstA = (bc >> 24) & 0x1Fu;
    const bool enabled = !(srcRgb == 1u && dstRgb == 0u && opRgb == 0u &&
                           srcA == 1u && dstA == 0u && opA == 0u);
    if (enabled) {
      glEnablei(GL_BLEND, i);
      glBlendEquationSeparatei(i, GlBlendOp(opRgb), GlBlendOp(opA));
      glBlendFuncSeparatei(i, GlBlendFactor(srcRgb, false), GlBlendFactor(dstRgb, false),
                           GlBlendFactor(srcA, true), GlBlendFactor(dstA, true));
    } else {
      glDisablei(GL_BLEND, i);
    }
  }
}

void Gles3Backend::ApplyDepthStencilState(uint32_t depthControl, uint32_t stencilRefMask,
                                          uint32_t stencilRefMaskBF, bool hasDepth,
                                          bool polygonal, bool cullFront, bool cullBack) {
  if (!hasDepth) {
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_STENCIL_TEST);
    return;
  }

  const bool stencilEn = (depthControl & 1u) != 0;
  const bool depthTest = (depthControl & 2u) != 0;
  const bool depthWrite = (depthControl & 4u) != 0;
  const uint32_t zFunc = (depthControl >> 4) & 7u;
  const bool backStencilEn = (depthControl & 0x80u) != 0;

  if (depthTest) {
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GlCompare(zFunc));
    glDepthMask(depthWrite ? GL_TRUE : GL_FALSE);
  } else {
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
  }

  if (stencilEn) {
    glEnable(GL_STENCIL_TEST);
    const bool useBf = polygonal && backStencilEn && cullFront && !cullBack;
    const uint32_t fRefMask = useBf ? stencilRefMaskBF : stencilRefMask;
    const uint32_t bRefMask = backStencilEn ? stencilRefMaskBF : stencilRefMask;

    const GLint fRef = static_cast<GLint>(fRefMask & 0xFFu);
    const GLuint fRead = (fRefMask >> 8) & 0xFFu;
    const GLuint fWrite = (fRefMask >> 16) & 0xFFu;
    glStencilFuncSeparate(GL_FRONT, GlCompare((depthControl >> 8) & 7u), fRef, fRead);
    glStencilOpSeparate(GL_FRONT, GlStencilOp((depthControl >> 11) & 7u),
                        GlStencilOp((depthControl >> 17) & 7u),
                        GlStencilOp((depthControl >> 14) & 7u));
    glStencilMaskSeparate(GL_FRONT, fWrite);

    if (backStencilEn) {
      const GLint bRef = static_cast<GLint>(bRefMask & 0xFFu);
      const GLuint bRead = (bRefMask >> 8) & 0xFFu;
      const GLuint bWrite = (bRefMask >> 16) & 0xFFu;
      glStencilFuncSeparate(GL_BACK, GlCompare((depthControl >> 20) & 7u), bRef, bRead);
      glStencilOpSeparate(GL_BACK, GlStencilOp((depthControl >> 23) & 7u),
                          GlStencilOp((depthControl >> 29) & 7u),
                          GlStencilOp((depthControl >> 26) & 7u));
      glStencilMaskSeparate(GL_BACK, bWrite);
    } else {
      glStencilFuncSeparate(GL_BACK, GlCompare((depthControl >> 8) & 7u), fRef, fRead);
      glStencilOpSeparate(GL_BACK, GlStencilOp((depthControl >> 11) & 7u),
                          GlStencilOp((depthControl >> 17) & 7u),
                          GlStencilOp((depthControl >> 14) & 7u));
      glStencilMaskSeparate(GL_BACK, fWrite);
    }
  } else {
    glDisable(GL_STENCIL_TEST);
  }
}

void Gles3Backend::ApplyRasterizerState(uint32_t suScModeControl, uint32_t clipControl,
                                        bool rectangles, float slopeBias, float depthBiasUnits) {
  // Xenos PA_SU_SC_MODE_CNTL bit 2: 0 = CCW front in screen space (Y-down), 1 = CW front in
  // screen space (Y-down). In GL_LOWER_LEFT with Y-down coordinates mapped directly to GL window Y
  // (no vertical flip), screen-space CW corresponds to GL_CCW and screen-space CCW to GL_CW.
  const bool guestCw = ((suScModeControl >> 2) & 1u) != 0;
  glFrontFace(guestCw ? GL_CCW : GL_CW);

  if (rectangles) {
    glDisable(GL_CULL_FACE);
  } else {
    const bool cullFront = (suScModeControl & 1u) != 0;
    const bool cullBack = (suScModeControl & 2u) != 0;
    if (cullFront && !cullBack) {
      glEnable(GL_CULL_FACE);
      glCullFace(GL_FRONT);
    } else if (cullBack && !cullFront) {
      glEnable(GL_CULL_FACE);
      glCullFace(GL_BACK);
    } else {
      glDisable(GL_CULL_FACE);
    }
  }

  if (hasDepthClamp_) {
    const bool clipDisable = ((clipControl >> 16) & 1u) != 0;
    if (clipDisable) glEnable(kGlDepthClampExt);
    else glDisable(kGlDepthClampExt);
  }

  if (slopeBias != 0.0f || depthBiasUnits != 0.0f) {
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(slopeBias, depthBiasUnits);
  } else {
    glDisable(GL_POLYGON_OFFSET_FILL);
  }
}

bool Gles3Backend::ExecuteDraw(const uint32_t* regs, uint32_t initiator, uint32_t indexBase,
                               rex::IPhysicalMemory* memory) {
  const uint32_t primitive = initiator & 0x3Fu;
  const uint32_t sourceSelect = (initiator >> 6) & 3u;
  const bool wideIndices = ((initiator >> 11) & 1u) != 0;
  const uint32_t indexCount = initiator >> 16;
  const uint32_t modeControl = regs[kRegModeControl];
  const uint32_t surfaceInfo = regs[kRegSurfaceInfo];
  const uint32_t pitch = surfaceInfo & 0x3FFFu;
  if (indexCount == 0 || modeControl == 0 || pitch == 0) return false;

  GLenum glTopo = GL_TRIANGLES;
  bool rectangles = false;
  bool convertQuads = false;
  bool convertFan = false;
  bool convertLineLoop = false;
  switch (primitive) {
    case 1: glTopo = GL_POINTS; break;
    case 2: glTopo = GL_LINES; break;
    case 3: glTopo = GL_LINE_STRIP; break;
    case 4: glTopo = GL_TRIANGLES; break;
    case 5: glTopo = GL_TRIANGLES; convertFan = true; break;
    case 6: glTopo = GL_TRIANGLE_STRIP; break;
    case 8: glTopo = GL_TRIANGLES; rectangles = true; break;
    case 12: glTopo = GL_LINE_STRIP; convertLineLoop = true; break;
    case 13: glTopo = GL_TRIANGLES; convertQuads = true; break;
    default: return false;
  }

  const uint32_t suScModeControl = regs[kRegSuScModeControl];
  const bool polygonal = !rectangles && primitive != 1 && primitive != 2 && primitive != 3 &&
                         primitive != 12;
  const bool cullFront = (suScModeControl & 1u) != 0;
  const bool cullBack = (suScModeControl & 2u) != 0;
  if (polygonal && cullFront && cullBack) return false;

  const Gles3Shaders::Handle vsH = shaders_.Current(false);
  Gles3Shaders::Handle psH = shaders_.Current(true);
  const Gles3Shaders::Stage* vs = shaders_.GetStage(vsH);
  const Gles3Shaders::Stage* ps = shaders_.GetStage(psH);
  if (!vs || vs->failed || !ps || ps->failed) return false;

  // Any draw invalidates a cached EDRAM->host resolve: the source surface changed.
  ++drawsSinceResolve_;

  const uint32_t colorMask = regs[kRegColorMask];
  const uint32_t colorControl = regs[kRegColorControl];
  const bool depthOnly = ((modeControl & 7u) == 5u) || ((colorMask & 0xFFFFu) == 0u);
  const bool alphaTestEnabled = (((colorControl >> 3) & 1u) != 0) && ((colorControl & 7u) != 7u);
  if (depthOnly && !ps->writesDepth && !ps->usesKill && !alphaTestEnabled) {
    psH = 0;
  }

  GLuint prog = 0;
  if (lastLinkedProg_ != 0 && lastVsH_ == vsH && lastPsH_ == psH &&
      lastRectProg_ == rectangles && lastAlphaProg_ == alphaTestEnabled) {
    prog = lastLinkedProg_;
  } else {
    prog = shaders_.GetLinkedProgram(vsH, psH, rectangles, alphaTestEnabled);
    lastVsH_ = vsH;
    lastPsH_ = psH;
    lastRectProg_ = rectangles;
    lastAlphaProg_ = alphaTestEnabled;
    lastLinkedProg_ = prog;
  }
  if (!prog) return false;

  // Target sizing & scissor
  const uint32_t tl = regs[kRegScissorTopLeft];
  const uint32_t br = regs[kRegScissorBottomRight];
  const uint32_t winOff = regs[kRegWindowOffset];
  const bool ignoreWinOff = (tl & 0x80000000u) != 0;
  const int32_t offX = ignoreWinOff ? 0 : (static_cast<int32_t>(winOff << 17) >> 17);
  const int32_t offY = ignoreWinOff ? 0 : (static_cast<int32_t>(winOff << 1) >> 17);
  const int32_t bottom = static_cast<int32_t>((br >> 16) & 0x7FFFu) + offY;
  if (bottom > 0 && resources_.EnsureRows(surfaceInfo, static_cast<uint32_t>(bottom))) {
    for (uint32_t i = 0; i < 4; ++i) lastFboColor_[i] = 0;
    lastFboDepth_ = 0;
    drawFboBound_ = false;
  }

  const uint32_t colorInfos[4] = {
      regs[kRegColorInfo0], regs[kRegColorInfo1], regs[kRegColorInfo2], regs[kRegColorInfo3]};
  const uint32_t depthInfo = regs[kRegDepthInfo];
  const uint32_t depthControl = regs[kRegDepthControl];
  const bool isRectClear = (primitive == 8u && indexCount <= 6u);
  const uint32_t cls = ((surfaceInfo >> 16) & 3u) != 0 ? 1u : 0u;

  GLuint colorTex[4] = {};
  uint32_t colorCount = 0;
  const Gles3Resources::ColorTarget* primaryColor = nullptr;
  for (uint32_t i = 0; i < 4 && !depthOnly; ++i) {
    if (((ps->colourTargets >> i) & 1u) == 0) continue;
    if (((colorMask >> (i * 4)) & 0xFu) == 0) continue;
    const Gles3Resources::ColorTarget* ct =
        isRectClear ? resources_.PrimaryColorTargetAt(colorInfos[i], pitch, cls) : nullptr;
    if (!ct) ct = resources_.ColorTargetFor(colorInfos[i], surfaceInfo);
    if (!ct || !ct->tex) continue;
    bool dup = false;
    for (uint32_t j = 0; j < i; ++j) {
      if (colorTex[j] == ct->tex) { dup = true; break; }
    }
    if (dup) continue;
    colorTex[i] = ct->tex;
    colorCount = i + 1;
    if (!primaryColor) primaryColor = ct;
  }

  GLuint depthTex = 0;
  const Gles3Resources::DepthTarget* boundDepth = nullptr;
  if ((depthControl & 3u) != 0) {
    const Gles3Resources::DepthTarget* dt =
        isRectClear ? resources_.PrimaryDepthTargetAt(depthInfo, pitch, cls) : nullptr;
    if (!dt) dt = resources_.DepthTargetFor(depthInfo, surfaceInfo);
    if (dt && dt->tex) {
      if (!primaryColor || (dt->width == primaryColor->width && dt->height == primaryColor->height)) {
        depthTex = dt->tex;
        boundDepth = dt;
      }
    }
  }
  if (colorCount == 0 && depthTex == 0) return false;

  uint32_t targetW = 0, targetH = 0;
  float scaleX = 1.0f, scaleY = 1.0f;
  uint32_t boundPitch = 0, boundRows = 0, boundMsaa = 0;
  if (primaryColor && primaryColor->pitch > 0 && primaryColor->rows > 0) {
    targetW = primaryColor->width;
    targetH = primaryColor->height;
    boundPitch = primaryColor->pitch;
    boundRows = primaryColor->rows;
    boundMsaa = primaryColor->msaa;
  } else if (boundDepth && boundDepth->pitch > 0 && boundDepth->rows > 0) {
    targetW = boundDepth->width;
    targetH = boundDepth->height;
    boundPitch = boundDepth->pitch;
    boundRows = boundDepth->rows;
    boundMsaa = boundDepth->msaa;
  } else {
    resources_.TargetSize(surfaceInfo, 4, targetW, targetH, scaleX, scaleY);
  }
  if (boundPitch > 0 && boundRows > 0 && pitch > 0) {
    if (boundPitch != pitch) {
      const uint32_t tSx = (boundMsaa == 2u) ? 2u : 1u;
      const uint32_t tSy = (boundMsaa >= 1u) ? 2u : 1u;
      const uint32_t cMsaa = (surfaceInfo >> 16) & 3u;
      const uint32_t cSx = (cMsaa == 2u) ? 2u : 1u;
      const uint32_t cSy = (cMsaa >= 1u) ? 2u : 1u;
      const uint64_t num = static_cast<uint64_t>(boundRows) * boundPitch * tSx * tSy;
      const uint64_t den = static_cast<uint64_t>(pitch) * cSx * cSy;
      const uint32_t clearH = std::max<uint32_t>(
          1u, static_cast<uint32_t>((num + den / 2u) / std::max<uint64_t>(1u, den)));
      scaleX = static_cast<float>(targetW) / static_cast<float>(pitch);
      scaleY = static_cast<float>(targetH) / static_cast<float>(clearH);
    } else {
      scaleX = static_cast<float>(targetW) / static_cast<float>(boundPitch);
      scaleY = static_cast<float>(targetH) / static_cast<float>(boundRows);
    }
  }
  if (targetW == 0 || targetH == 0) return false;

  const int32_t sx0 = std::max(0, static_cast<int32_t>(std::lround((static_cast<int32_t>(tl & 0x7FFFu) + offX) * scaleX)));
  const int32_t sy0 = std::max(0, static_cast<int32_t>(std::lround((static_cast<int32_t>((tl >> 16) & 0x7FFFu) + offY) * scaleY)));
  const int32_t sx1 = std::min(static_cast<int32_t>(targetW),
                               static_cast<int32_t>(std::lround((static_cast<int32_t>(br & 0x7FFFu) + offX) * scaleX)));
  const int32_t sy1 = std::min(static_cast<int32_t>(targetH),
                               static_cast<int32_t>(std::lround((static_cast<int32_t>((br >> 16) & 0x7FFFu) + offY) * scaleY)));
  if (sx1 <= sx0 || sy1 <= sy0) return false;

  // Bind draw FBO & prevent SRV/RTV read-write hazards.
  // CRITICAL FOR ADRENO TILED GPUs: only call glFramebufferTexture2D / glDrawBuffers when the
  // attachment set actually changes; calling them per-draw forces GMEM tile flushes on every draw!
  bool fboChanged = !drawFboBound_ || lastFboDepth_ != depthTex;
  for (uint32_t i = 0; i < 4 && !fboChanged; ++i) {
    if (lastFboColor_[i] != colorTex[i]) fboChanged = true;
  }
  if (!drawFboBound_) {
    glBindFramebuffer(GL_FRAMEBUFFER, drawFbo_);
    drawFboBound_ = true;
  }
  if (fboChanged) {
    GLenum drawBufs[4] = {GL_NONE, GL_NONE, GL_NONE, GL_NONE};
    for (uint32_t i = 0; i < 4; ++i) {
      UnbindTexturesMatching(colorTex[i]);
      if (lastFboColor_[i] != colorTex[i]) {
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i, GL_TEXTURE_2D, colorTex[i],
                               0);
        lastFboColor_[i] = colorTex[i];
      }
      if (colorTex[i]) drawBufs[i] = GL_COLOR_ATTACHMENT0 + i;
    }
    UnbindTexturesMatching(depthTex);
    if (lastFboDepth_ != depthTex) {
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, depthTex,
                             0);
      lastFboDepth_ = depthTex;
    }
    glDrawBuffers(4, drawBufs);
  }

  if (stateDirty_ || lastVpW_ != targetW || lastVpH_ != targetH) {
    glViewport(0, 0, static_cast<GLsizei>(targetW), static_cast<GLsizei>(targetH));
    lastVpW_ = targetW;
    lastVpH_ = targetH;
  }
  if (stateDirty_ || !lastScissorEnabled_) {
    glEnable(GL_SCISSOR_TEST);
    lastScissorEnabled_ = true;
  }
  const int32_t sw = sx1 - sx0;
  const int32_t sh = sy1 - sy0;
  if (stateDirty_ || lastScissor_[0] != sx0 || lastScissor_[1] != sy0 ||
      lastScissor_[2] != sw || lastScissor_[3] != sh) {
    glScissor(sx0, sy0, sw, sh);
    lastScissor_[0] = sx0;
    lastScissor_[1] = sy0;
    lastScissor_[2] = sw;
    lastScissor_[3] = sh;
  }

  // Rasterizer, blend, depth/stencil state (shadow-cached to skip redundant GL driver calls)
  float polyScale = 0.0f, polyOffset = 0.0f;
  if (polygonal) {
    if (((suScModeControl >> 11) & 1u) && !cullFront) {
      polyScale = RegFloat(regs, kRegPolyOffsetFrontScale + 0);
      polyOffset = RegFloat(regs, kRegPolyOffsetFrontScale + 1);
    }
    if (((suScModeControl >> 12) & 1u) && !cullBack && polyScale == 0.0f && polyOffset == 0.0f) {
      polyScale = RegFloat(regs, kRegPolyOffsetFrontScale + 2);
      polyOffset = RegFloat(regs, kRegPolyOffsetFrontScale + 3);
    }
  } else if ((suScModeControl >> 13) & 1u) {
    polyScale = RegFloat(regs, kRegPolyOffsetFrontScale + 0);
    polyOffset = RegFloat(regs, kRegPolyOffsetFrontScale + 1);
  }
  const float hostSlope = polyScale * (1.0f / 16.0f) * std::max(scaleX, scaleY);
  const float hostUnits = polyOffset * 16777215.0f;
  const uint32_t clipCtrl = regs[kRegClipControl];
  if (stateDirty_ || lastSuSc_ != suScModeControl || lastClipCtrl_ != clipCtrl ||
      lastRects_ != rectangles || lastSlopeBias_ != hostSlope || lastDepthBias_ != hostUnits) {
    ApplyRasterizerState(suScModeControl, clipCtrl, rectangles, hostSlope, hostUnits);
    lastSuSc_ = suScModeControl;
    lastClipCtrl_ = clipCtrl;
    lastRects_ = rectangles;
    lastSlopeBias_ = hostSlope;
    lastDepthBias_ = hostUnits;
  }

  const uint32_t blendControls[4] = {
      regs[kRegBlendControl0], regs[kRegBlendControl1], regs[kRegBlendControl2], regs[kRegBlendControl3]};
  float blendFactor[4] = {
      RegFloat(regs, kRegBlendRed + 0), RegFloat(regs, kRegBlendRed + 1),
      RegFloat(regs, kRegBlendRed + 2), RegFloat(regs, kRegBlendRed + 3)};
  if (stateDirty_ || lastColorMask_ != colorMask ||
      std::memcmp(lastBlendCtrl_, blendControls, sizeof(blendControls)) != 0 ||
      std::memcmp(lastBlendFactor_, blendFactor, sizeof(blendFactor)) != 0) {
    ApplyBlendState(blendControls, colorMask, blendFactor);
    lastColorMask_ = colorMask;
    std::memcpy(lastBlendCtrl_, blendControls, sizeof(blendControls));
    std::memcpy(lastBlendFactor_, blendFactor, sizeof(blendFactor));
  }

  const uint32_t stencilRef = regs[kRegStencilRefMask];
  const uint32_t stencilRefBF = regs[kRegStencilRefMaskBF];
  const bool hasDepth = (depthTex != 0);
  if (stateDirty_ || lastDepthCtrl_ != depthControl || lastStencilRef_ != stencilRef ||
      lastStencilRefBF_ != stencilRefBF || lastHasDepth_ != hasDepth ||
      lastDepthPoly_ != polygonal || lastCullFront_ != cullFront || lastCullBack_ != cullBack) {
    ApplyDepthStencilState(depthControl, stencilRef, stencilRefBF, hasDepth, polygonal, cullFront,
                           cullBack);
    lastDepthCtrl_ = depthControl;
    lastStencilRef_ = stencilRef;
    lastStencilRefBF_ = stencilRefBF;
    lastHasDepth_ = hasDepth;
    lastDepthPoly_ = polygonal;
    lastCullFront_ = cullFront;
    lastCullBack_ = cullBack;
  }
  stateDirty_ = false;

  // DrawConstants UBO
  uint32_t vteFlags = regs[kRegVteControl];
  float vpOffX = (vteFlags & 0x2u) ? RegFloat(regs, kRegViewportXScale + 1) : 0.0f;
  float vpOffY = (vteFlags & 0x8u) ? RegFloat(regs, kRegViewportXScale + 3) : 0.0f;
  if ((suScModeControl & (1u << 16)) != 0) {
    const float wx = static_cast<float>(static_cast<int32_t>(winOff << 17) >> 17);
    const float wy = static_cast<float>(static_cast<int32_t>(winOff << 1) >> 17);
    if (wx != 0.0f || wy != 0.0f) {
      vpOffX += wx;
      vpOffY += wy;
      vteFlags |= 0x2u | 0x8u;
    }
  }

  drawConst_.viewportScale[0] = RegFloat(regs, kRegViewportXScale + 0);
  drawConst_.viewportOffset[0] = vpOffX;
  drawConst_.viewportScale[1] = RegFloat(regs, kRegViewportXScale + 2);
  drawConst_.viewportOffset[1] = vpOffY;
  drawConst_.viewportScale[2] = RegFloat(regs, kRegViewportXScale + 4);
  drawConst_.viewportOffset[2] = RegFloat(regs, kRegViewportXScale + 5);
  drawConst_.viewportScale[3] = 0.0f;
  drawConst_.viewportOffset[3] = 0.0f;
  drawConst_.targetSize[0] = static_cast<float>(pitch);
  drawConst_.targetSize[1] = scaleY > 0.0f ? static_cast<float>(targetH) / scaleY : 720.0f;
  drawConst_.targetSize[2] = 1.0f / static_cast<float>(pitch);
  drawConst_.targetSize[3] = targetH > 0 ? scaleY / static_cast<float>(targetH) : 1.0f / 720.0f;
  drawConst_.flags[0] = vteFlags | (((regs[kRegVertexControl] & 1u) != 0) ? 0x10000u : 0u);
  drawConst_.flags[1] = ((colorControl >> 3) & 1u) ? (colorControl & 7u) : 7u;
  drawConst_.flags[2] = regs[kRegAlphaReference];
  drawConst_.flags[3] = static_cast<uint32_t>(ps->hash);
  drawConst_.pixelGen[0] = scaleX > 0.0f ? 1.0f / scaleX : 1.0f;
  drawConst_.pixelGen[1] = scaleY > 0.0f ? 1.0f / scaleY : 1.0f;
  drawConst_.pixelGen[2] = static_cast<float>((regs[kRegContextMisc] >> 8) & 0xFFu);
  drawConst_.pixelGen[3] = ((regs[kRegProgramControl] >> 18) & 1u) ? 1.0f : 0.0f;
  for (uint32_t i = 0; i < 4; ++i) {
    int32_t eb = static_cast<int32_t>((colorInfos[i] >> 20) & 0x3Fu);
    if (eb >= 32) eb -= 64;
    drawConst_.colorExpBias[i] = (eb == 0) ? 1.0f : std::ldexp(1.0f, eb);
  }

  // Bind textures & fill textureSize/textureAdjustment
  auto bindStageTextures = [&](const Gles3Shaders::Stage* st) {
    if (!st) return;
    for (const XenosGlsl::TextureFetch& fetch : st->textureFetches) {
      if (fetch.binding >= 24) continue;
      uint32_t words[6] = {};
      const uint32_t base = kRegFetchConstants + fetch.slot * 6u;
      for (uint32_t i = 0; i < 6; ++i) words[i] = regs[base + i];

      uint32_t tw = 1, th = 1;
      bool isCube = false;
      const Gles3Resources::Resolved* rSurf = nullptr;
      GLuint tex = resources_.TextureFor(words, tw, th, isCube, rSurf);
      // Never sample an attached FBO texture!
      for (uint32_t c = 0; c < 4; ++c) {
        if (tex && tex == colorTex[c]) tex = isCube ? resources_.WhiteTextureCube() : resources_.WhiteTexture2D();
      }
      if (tex && tex == depthTex) tex = resources_.WhiteTexture2D();

      const GLenum target = (fetch.dimension == 3u) ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D;
      if (target == GL_TEXTURE_CUBE_MAP && !isCube) tex = resources_.WhiteTextureCube();
      if (target == GL_TEXTURE_2D && isCube) tex = resources_.WhiteTexture2D();

      const GLuint smp = (rSurf && rSurf->isDepth && !resources_.HasFloatLinear())
                             ? resources_.NearestClampSampler()
                             : resources_.SamplerFor(words);
      if (boundTex_[fetch.binding] != tex || boundTexTarget_[fetch.binding] != target) {
        glActiveTexture(GL_TEXTURE0 + fetch.binding);
        glBindTexture(target, tex);
        boundTex_[fetch.binding] = tex;
        boundTexTarget_[fetch.binding] = target;
      }
      if (boundSampler_[fetch.binding] != smp) {
        glBindSampler(fetch.binding, smp);
        boundSampler_[fetch.binding] = smp;
      }

      if (fetch.slot < 32) {
        drawConst_.texSize[fetch.slot][0] = static_cast<float>(tw);
        drawConst_.texSize[fetch.slot][1] = static_cast<float>(th);
        drawConst_.texSize[fetch.slot][2] = tw > 0 ? 1.0f / static_cast<float>(tw) : 1.0f;
        drawConst_.texSize[fetch.slot][3] = th > 0 ? 1.0f / static_cast<float>(th) : 1.0f;

        uint32_t swizzle = (words[3] >> 1) & 0xFFFu;
        const uint32_t origSwizzle = swizzle;
        if (rSurf && rSurf->rbSwapped) {
          uint32_t unswapped = 0;
          for (uint32_t i = 0; i < 4; ++i) {
            uint32_t sel = (swizzle >> (i * 3)) & 7u;
            if (sel == 0u) sel = 2u;
            else if (sel == 2u) sel = 0u;
            unswapped |= (sel << (i * 3));
          }
          swizzle = unswapped;
        }
        const uint32_t texFmt = words[1] & 0x3Fu;
        const bool repRRRR = (rSurf && rSurf->isDepth) || texFmt == 2u || texFmt == 8u ||
                              texFmt == 9u || texFmt == 22u || texFmt == 23u || texFmt == 24u ||
                              texFmt == 27u || texFmt == 30u || texFmt == 36u || texFmt == 46u ||
                              texFmt == 47u || texFmt == 58u || texFmt == 59u;
        const bool repRGGG = (texFmt == 10u || texFmt == 25u || texFmt == 28u || texFmt == 31u ||
                              texFmt == 37u || texFmt == 49u);
        if (repRRRR || repRGGG) {
          uint32_t rep = 0;
          for (uint32_t i = 0; i < 4; ++i) {
            uint32_t sel = (swizzle >> (i * 3)) & 7u;
            if (repRRRR && sel < 4u) sel = 0u;
            else if (repRGGG && (sel == 2u || sel == 3u)) sel = 1u;
            rep |= (sel << (i * 3));
          }
          swizzle = rep;
        }
        drawConst_.textureAdjustment[fetch.slot][0] = swizzle;
        const uint32_t rawSigns = (words[0] >> 2) & 0xFFu;
        uint32_t permutedSigns = 0;
        for (uint32_t i = 0; i < 4; ++i) {
          const uint32_t origSel = (origSwizzle >> (i * 3)) & 7u;
          const uint32_t compSign = origSel < 4u ? ((rawSigns >> (origSel * 2)) & 3u) : 0u;
          permutedSigns |= (compSign << (i * 2));
        }
        drawConst_.textureAdjustment[fetch.slot][1] = permutedSigns;
        int32_t expAdj = static_cast<int32_t>((words[3] >> 13) & 0x3Fu);
        if (expAdj >= 32) expAdj -= 64;
        // A resolve that used glCopyImageSubData stored its exponent bias and clamp
        // on the resolved texture instead of applying them in a fullscreen pass.
        // Fold them into the fetch adjustment so sampling sees the same value.
        uint8_t deferredClamp = 0u;
        if (rSurf) {
          expAdj += rSurf->expBias;
          deferredClamp = rSurf->clampMode;
        }
        if (expAdj == 0) {
          drawConst_.textureAdjustment[fetch.slot][2] = 0u;
        } else {
          const float expScale = std::ldexp(1.0f, expAdj);
          uint32_t expBits = 0;
          std::memcpy(&expBits, &expScale, 4);
          drawConst_.textureAdjustment[fetch.slot][2] = expBits ^ 0x3F800000u;
        }
        drawConst_.textureAdjustment[fetch.slot][3] = deferredClamp;
      }
    }
  };
  bindStageTextures(vs);
  if (psH) bindStageTextures(ps);

  // Bathysphere projector cutscene investigation: log the exact GL state of the two
  // passes implicated by the 946-draw dump (see docs/10-STATUS.md). Must run AFTER
  // bindStageTextures so textureAdjustment[] reflects this draw, not the previous one.
  {
    static bool loggedFog = false;
    static bool loggedSpot = false;
    // Key on the distinctive blend state rather than the microcode hash: the
    // translator version (and therefore the hash) changes with GLSL edits.
    const bool isFog = blendControls[0] == 0x07060706u;
    const bool isSpot = blendControls[0] == 0x04000400u;
    if ((isFog && !loggedFog) || (isSpot && !loggedSpot)) {
      if (isFog) loggedFog = true;
      if (isSpot) loggedSpot = true;
      GLESLOG(
          "CUTSCENE-DIAG ps=%016llx vs=%016llx bc0=0x%08x dc=0x%08x stencil=0x%08x "
          "stencilBF=0x%08x colorMask=0x%08x colorControl=0x%08x colorInfo=0x%08x "
          "colorClear=0x%08x alphaRef=0x%08x depthTex=%u colorCount=%u "
          "hasStencil=%u depthTest=%u depthWrite=%u fetches=%zu",
          static_cast<unsigned long long>(ps->hash), static_cast<unsigned long long>(vs->hash),
          blendControls[0], depthControl, stencilRef, stencilRefBF, colorMask, colorControl,
          colorInfos[0], regs[kRegCopyColorClear], regs[kRegAlphaReference],
          depthTex != 0 ? 1u : 0u, colorCount, (depthControl & 1u) != 0 ? 1u : 0u,
          (depthControl & 2u) != 0 ? 1u : 0u, (depthControl & 4u) != 0 ? 1u : 0u,
          ps->textureFetches.size());
      for (const XenosGlsl::TextureFetch& fetch : ps->textureFetches) {
        if (fetch.binding >= 24 || fetch.slot >= 32) continue;
        const uint32_t base = kRegFetchConstants + fetch.slot * 6u;
        const uint32_t w1 = regs[base + 1];
        const uint32_t w2 = regs[base + 2];
        const uint32_t w3 = regs[base + 3];
        const auto* rs = resources_.ResolvedAt(w1 & 0xFFFFF000u);
        GLESLOG("CUTSCENE-DIAG   fetch slot=%u dim=%u base=0x%08x fmt=%u size=%ux%u swizzle=0x%03x "
                "adj=0x%08x resolved=%u isDepth=%u",
                fetch.slot, fetch.dimension, w1 & 0xFFFFF000u, w1 & 0x3Fu,
                (w2 & 0x1FFFu) + 1u, ((w2 >> 13) & 0x1FFFu) + 1u, (w3 >> 1) & 0xFFFu,
                drawConst_.textureAdjustment[fetch.slot][0], rs ? 1u : 0u,
                rs && rs->isDepth ? 1u : 0u);
      }
    }
  }

  // Bind vertex SSBOs
  for (const XenosGlsl::VertexFetch& vf : vs->vertexFetches) {
    const uint32_t base = kRegFetchConstants + vf.slot * 2u;
    const uint32_t w0 = regs[base + 0];
    const uint32_t w1 = regs[base + 1];
    const uint32_t bytes = ((w1 >> 2) & 0xFFFFFFu) * 4u;
    size_t vbOff = 0, vbRange = 0;
    GLuint ssbo = resources_.VertexBufferFor(w0 & ~3u, bytes, vbOff, vbRange);
    if (!ssbo) return false;
    if (XenosGlsl::VertexTextureBuffers()) {
      if (vf.binding >= 16u) return false;
      GLuint& tex = vbTex_[vf.binding];
      if (!tex) glGenTextures(1, &tex);
      GLuint& storage = vbTexStorage_[vf.binding];
      if (!storage) glGenBuffers(1, &storage);
      if (boundVbTexBuf_[vf.binding] != ssbo || boundVbTexOff_[vf.binding] != vbOff ||
          boundVbTexSize_[vf.binding] != vbRange) {
        // glTexBufferRange has strict offset/size alignment; copy the exact range
        // into a dedicated buffer and use glTexBuffer instead.
        const GLsizei sz = static_cast<GLsizei>(vbRange);
        // Mali rejects glCopyBufferSubData from the persistently-mapped SSBO ring,
        // and GLES has no glGetBufferSubData, so upload from the guest source pointer.
        const uint8_t* src = resources_.PhysicalPtr((w0 & ~3u) & 0x1FFFFFFFu, vbRange);
        glBindBuffer(GL_TEXTURE_BUFFER, storage);
        if (vbTexStorageSize_[vf.binding] != sz) {
          glBufferData(GL_TEXTURE_BUFFER, sz, nullptr, GL_STREAM_DRAW);
          vbTexStorageSize_[vf.binding] = sz;
        }
        if (src) {
          glBufferSubData(GL_TEXTURE_BUFFER, 0, static_cast<GLsizeiptr>(vbRange), src);
        }
        glActiveTexture(GL_TEXTURE0 + vf.binding + 16u);
        glBindTexture(GL_TEXTURE_BUFFER, tex);
        glTexBuffer(GL_TEXTURE_BUFFER, GL_R32UI, storage);
        boundVbTexBuf_[vf.binding] = ssbo;
        boundVbTexOff_[vf.binding] = vbOff;
        boundVbTexSize_[vf.binding] = vbRange;
      }
      continue;
    }
    if (vbRange > 0) {
      if (vf.binding >= 16u || boundSsbo_[vf.binding] != ssbo ||
          boundSsboOff_[vf.binding] != vbOff || boundSsboSize_[vf.binding] != vbRange) {
        glBindBufferRange(GL_SHADER_STORAGE_BUFFER, vf.binding, ssbo,
                          static_cast<GLintptr>(vbOff), static_cast<GLsizeiptr>(vbRange));
        if (vf.binding < 16u) {
          boundSsbo_[vf.binding] = ssbo;
          boundSsboOff_[vf.binding] = vbOff;
          boundSsboSize_[vf.binding] = vbRange;
        }
      }
    } else if (vf.binding >= 16u || boundSsbo_[vf.binding] != ssbo ||
               boundSsboSize_[vf.binding] != 0) {
      glBindBufferBase(GL_SHADER_STORAGE_BUFFER, vf.binding, ssbo);
      if (vf.binding < 16u) {
        boundSsbo_[vf.binding] = ssbo;
        boundSsboOff_[vf.binding] = 0;
        boundSsboSize_[vf.binding] = 0;
      }
    }
  }

  // Upload UBOs (VS floats, PS floats, Bools+Loops, DrawConstants) via a single unsynchronized
  // ring-buffer map per draw (eliminates per-draw Adreno UBO ghosting/renaming stalls).
  auto alignUp = [this](size_t v) -> size_t {
    return (v + uboAlign_ - 1u) & ~(uboAlign_ - 1u);
  };

  size_t vsBytes = 0;
  const void* vsSrc = nullptr;
  if (vs->constantMap.empty()) {
    vsBytes = 256u * 16u;
    vsSrc = &regs[kRegVertexConstants];
  } else {
    const size_t n = std::min<size_t>(vs->constantMap.size(), 256u);
    for (size_t i = 0; i < n; ++i) {
      std::memcpy(&packedVsFloats_[i * 4], &regs[kRegVertexConstants + vs->constantMap[i] * 4u], 16);
    }
    vsBytes = n * 16u;
    vsSrc = packedVsFloats_;
  }
  bool vsDirty = !vsFloatsValid_ || lastVsFloatBytes_ != vsBytes ||
                 (vsBytes > 0 && std::memcmp(lastVsFloats_, vsSrc, vsBytes) != 0);

  size_t psBytes = 0;
  const void* psSrc = nullptr;
  if (psH) {
    if (ps->constantMap.empty()) {
      psBytes = 256u * 16u;
      psSrc = &regs[kRegPixelConstants];
    } else {
      const size_t n = std::min<size_t>(ps->constantMap.size(), 256u);
      for (size_t i = 0; i < n; ++i) {
        std::memcpy(&packedPsFloats_[i * 4], &regs[kRegPixelConstants + ps->constantMap[i] * 4u], 16);
      }
      psBytes = n * 16u;
      psSrc = packedPsFloats_;
    }
  }
  bool psDirty = psH != 0 && (!psFloatsValid_ || lastPsFloatBytes_ != psBytes ||
                              (psBytes > 0 && std::memcmp(lastPsFloats_, psSrc, psBytes) != 0));

  constexpr size_t kBoolsBytes = 40u * 4u;
  bool boolsDirty =
      !boolsValid_ || std::memcmp(lastBools_, &regs[kRegBoolConstants], kBoolsBytes) != 0;

  constexpr size_t kDrawBytes = sizeof(DrawConstantsCpu);
  bool drawDirty =
      !drawConstValid_ || std::memcmp(&lastDrawConst_, &drawConst_, kDrawBytes) != 0;

  uboRingOffset_ = alignUp(uboRingOffset_);
  const size_t worstBytes = alignUp(std::max<size_t>(vsBytes, 16u)) +
                            alignUp(std::max<size_t>(psBytes, 16u)) +
                            alignUp(kBoolsBytes) + alignUp(kDrawBytes);
  if (uboRingPtr_) {
    const size_t halfSize = uboRingSize_ / 2u;
    const size_t halfEnd = uboHalf_ ? uboRingSize_ : halfSize;
    if (uboRingOffset_ + worstBytes > halfEnd) {
      if (uboHalfFence_[uboHalf_]) glDeleteSync(uboHalfFence_[uboHalf_]);
      uboHalfFence_[uboHalf_] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
      uboHalf_ ^= 1;
      if (uboHalfFence_[uboHalf_]) {
        glClientWaitSync(uboHalfFence_[uboHalf_], GL_SYNC_FLUSH_COMMANDS_BIT, 500000000ULL);
        glDeleteSync(uboHalfFence_[uboHalf_]);
        uboHalfFence_[uboHalf_] = nullptr;
      }
      uboRingOffset_ = uboHalf_ ? halfSize : 0u;
    }
  } else if (uboRingOffset_ + worstBytes > uboRingSize_) {
    glBindBuffer(GL_UNIFORM_BUFFER, uboRing_);
    glBufferData(GL_UNIFORM_BUFFER, static_cast<GLsizeiptr>(uboRingSize_), nullptr,
                 GL_STREAM_DRAW);
    uboRingOffset_ = 0;
    vsDirty = true;
    psDirty = (psH != 0);
    boolsDirty = true;
    drawDirty = true;
  }

  const size_t vsSlot = vsDirty ? alignUp(std::max<size_t>(vsBytes, 16u)) : 0u;
  const size_t psSlot = psDirty ? alignUp(std::max<size_t>(psBytes, 16u)) : 0u;
  const size_t boolSlot = boolsDirty ? alignUp(kBoolsBytes) : 0u;
  const size_t drawSlot = drawDirty ? alignUp(kDrawBytes) : 0u;
  const size_t totalUboBytes = vsSlot + psSlot + boolSlot + drawSlot;

  if (totalUboBytes > 0) {
    const size_t baseOff = uboRingOffset_;
    uint8_t* mapped = nullptr;
    if (uboRingPtr_) {
      mapped = uboRingPtr_ + baseOff;
    } else {
      glBindBuffer(GL_UNIFORM_BUFFER, uboRing_);
      mapped = static_cast<uint8_t*>(
          glMapBufferRange(GL_UNIFORM_BUFFER, static_cast<GLintptr>(baseOff),
                           static_cast<GLsizeiptr>(totalUboBytes),
                           GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT));
    }
    size_t cur = 0;
    size_t vsOff = 0, psOff = 0, boolOff = 0, drawOff = 0;
    if (mapped) {
      if (vsDirty) {
        vsOff = baseOff + cur;
        if (vsBytes > 0) {
          std::memcpy(mapped + cur, vsSrc, vsBytes);
          std::memcpy(lastVsFloats_, vsSrc, vsBytes);
        }
        lastVsFloatBytes_ = vsBytes;
        vsFloatsValid_ = true;
        cur += vsSlot;
      }
      if (psDirty) {
        psOff = baseOff + cur;
        if (psBytes > 0) {
          std::memcpy(mapped + cur, psSrc, psBytes);
          std::memcpy(lastPsFloats_, psSrc, psBytes);
        }
        lastPsFloatBytes_ = psBytes;
        psFloatsValid_ = true;
        cur += psSlot;
      }
      if (boolsDirty) {
        boolOff = baseOff + cur;
        std::memcpy(mapped + cur, &regs[kRegBoolConstants], kBoolsBytes);
        std::memcpy(lastBools_, &regs[kRegBoolConstants], kBoolsBytes);
        boolsValid_ = true;
        cur += boolSlot;
      }
      if (drawDirty) {
        drawOff = baseOff + cur;
        std::memcpy(mapped + cur, &drawConst_, kDrawBytes);
        lastDrawConst_ = drawConst_;
        drawConstValid_ = true;
        cur += drawSlot;
      }
      if (!uboRingPtr_) {
        glUnmapBuffer(GL_UNIFORM_BUFFER);
      }
      uboRingOffset_ = baseOff + totalUboBytes;
      if (vsDirty) {
        glBindBufferRange(GL_UNIFORM_BUFFER, 0, uboRing_, static_cast<GLintptr>(vsOff),
                          static_cast<GLsizeiptr>(std::max<size_t>(vsBytes, 16u)));
      }
      if (psDirty) {
        glBindBufferRange(GL_UNIFORM_BUFFER, 1, uboRing_, static_cast<GLintptr>(psOff),
                          static_cast<GLsizeiptr>(std::max<size_t>(psBytes, 16u)));
      }
      if (boolsDirty) {
        glBindBufferRange(GL_UNIFORM_BUFFER, 2, uboRing_, static_cast<GLintptr>(boolOff),
                          static_cast<GLsizeiptr>(kBoolsBytes));
      }
      if (drawDirty) {
        glBindBufferRange(GL_UNIFORM_BUFFER, 3, uboRing_, static_cast<GLintptr>(drawOff),
                          static_cast<GLsizeiptr>(kDrawBytes));
      }
    }
  }

  if (lastProg_ != prog) {
    glUseProgram(prog);
    lastProg_ = prog;
  }

  const int32_t baseVertex = static_cast<int32_t>(regs[kRegIndexOffset]);
  const bool dmaIndexed = (sourceSelect == 0u && indexBase != 0u);
  if (!dmaIndexed && !convertQuads && !convertFan && !convertLineLoop) {
    if (lastPrimRestart_) {
      glDisable(GL_PRIMITIVE_RESTART_FIXED_INDEX);
      lastPrimRestart_ = false;
    }
    glDrawArrays(glTopo, baseVertex, static_cast<GLsizei>(indexCount));
    return true;
  }

  const bool strips = (glTopo == GL_TRIANGLE_STRIP || glTopo == GL_LINE_STRIP);
  const bool resetEnabled = strips && ((suScModeControl & (1u << 21)) != 0);
  if (resetEnabled != lastPrimRestart_) {
    if (resetEnabled) glEnable(GL_PRIMITIVE_RESTART_FIXED_INDEX);
    else glDisable(GL_PRIMITIVE_RESTART_FIXED_INDEX);
    lastPrimRestart_ = resetEnabled;
  }

  // Fast path: cached static index buffer for direct indexed draws (eliminates CPU byte-swap
  // and glBufferSubData on ~95% of 3D mesh draws).
  if (dmaIndexed && !convertQuads && !convertFan && !convertLineLoop) {
    const uint32_t resetVal = regs[kRegResetIndex];
    if (GLuint staticEbo = resources_.StaticIndexBufferFor(indexBase, indexCount, wideIndices,
                                                           resetEnabled, resetVal)) {
      if (lastEbo_ != staticEbo) {
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, staticEbo);
        lastEbo_ = staticEbo;
      }
      if (baseVertex != 0) {
        glDrawElementsBaseVertex(glTopo, static_cast<GLsizei>(indexCount), GL_UNSIGNED_INT, nullptr,
                                 baseVertex);
      } else {
        glDrawElements(glTopo, static_cast<GLsizei>(indexCount), GL_UNSIGNED_INT, nullptr);
      }
      return true;
    }
  }

  // Build 32-bit index stream for indexed draws / quad / fan / line-loop expansion
  scratchIdx_.resize(indexCount);
  if (dmaIndexed) {
    const uint32_t elemBytes = wideIndices ? 4u : 2u;
    const uint8_t* ptr = resources_.PhysicalPtr(indexBase & kPhysicalMask, indexCount * elemBytes);
    if (!ptr) return false;
    (void)memory;
    if (wideIndices) {
      const auto* src = reinterpret_cast<const uint32_t*>(ptr);
      const uint32_t reset = regs[kRegResetIndex] & 0xFFFFFFu;
      for (uint32_t i = 0; i < indexCount; ++i) {
        const uint32_t idx = ByteSwap32(src[i]) & 0xFFFFFFu;
        scratchIdx_[i] = (resetEnabled && idx == reset) ? 0xFFFFFFFFu : idx;
      }
    } else {
      const auto* src = reinterpret_cast<const uint16_t*>(ptr);
      const uint32_t reset = regs[kRegResetIndex] & 0xFFFFu;
      for (uint32_t i = 0; i < indexCount; ++i) {
        const uint32_t idx = ByteSwap16(src[i]);
        scratchIdx_[i] = (resetEnabled && idx == reset) ? 0xFFFFFFFFu : idx;
      }
    }
  } else {
    for (uint32_t i = 0; i < indexCount; ++i) scratchIdx_[i] = i;
  }

  const uint32_t* drawIdx = scratchIdx_.data();
  uint32_t drawCount = indexCount;
  if (convertQuads) {
    expandedIdx_.clear();
    expandedIdx_.reserve((indexCount / 4u) * 6u);
    for (uint32_t i = 0; i + 3 < indexCount; i += 4) {
      const uint32_t t[6] = {scratchIdx_[i + 0], scratchIdx_[i + 1], scratchIdx_[i + 2],
                             scratchIdx_[i + 0], scratchIdx_[i + 2], scratchIdx_[i + 3]};
      expandedIdx_.insert(expandedIdx_.end(), t, t + 6);
    }
    drawIdx = expandedIdx_.data();
    drawCount = static_cast<uint32_t>(expandedIdx_.size());
  } else if (convertFan) {
    expandedIdx_.clear();
    if (indexCount >= 3) {
      expandedIdx_.reserve((indexCount - 2u) * 3u);
      for (uint32_t i = 1; i + 1 < indexCount; ++i) {
        const uint32_t t[3] = {scratchIdx_[0], scratchIdx_[i], scratchIdx_[i + 1]};
        expandedIdx_.insert(expandedIdx_.end(), t, t + 3);
      }
    }
    drawIdx = expandedIdx_.data();
    drawCount = static_cast<uint32_t>(expandedIdx_.size());
  } else if (convertLineLoop) {
    expandedIdx_ = scratchIdx_;
    if (indexCount >= 2) expandedIdx_.push_back(scratchIdx_[0]);
    drawIdx = expandedIdx_.data();
    drawCount = static_cast<uint32_t>(expandedIdx_.size());
  }
  if (drawCount == 0) return false;

  const size_t idxBytes = static_cast<size_t>(drawCount) * 4u;
  if (lastEbo_ != indexBuf_) {
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, indexBuf_);
    lastEbo_ = indexBuf_;
  }
  size_t idxOff = 0;
  if (indexBufPtr_ && idxBytes <= indexBufCap_ / 2u) {
    const size_t halfSize = indexBufCap_ / 2u;
    const size_t halfEnd = indexHalf_ ? indexBufCap_ : halfSize;
    if (indexRingOffset_ + idxBytes > halfEnd) {
      if (indexHalfFence_[indexHalf_]) glDeleteSync(indexHalfFence_[indexHalf_]);
      indexHalfFence_[indexHalf_] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
      indexHalf_ ^= 1;
      if (indexHalfFence_[indexHalf_]) {
        glClientWaitSync(indexHalfFence_[indexHalf_], GL_SYNC_FLUSH_COMMANDS_BIT, 500000000ULL);
        glDeleteSync(indexHalfFence_[indexHalf_]);
        indexHalfFence_[indexHalf_] = nullptr;
      }
      indexRingOffset_ = indexHalf_ ? halfSize : 0u;
    }
    idxOff = indexRingOffset_;
    std::memcpy(indexBufPtr_ + idxOff, drawIdx, idxBytes);
    indexRingOffset_ = (idxOff + idxBytes + 15u) & ~15u;
  } else {
    if (idxBytes > indexBufCap_) {
      indexBufCap_ = std::max(idxBytes * 2u, indexBufCap_ * 2u);
      if (indexBufPtr_) {
        glUnmapBuffer(GL_ELEMENT_ARRAY_BUFFER);
        indexBufPtr_ = nullptr;
      }
      glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(indexBufCap_), nullptr,
                   GL_STREAM_DRAW);
      indexRingOffset_ = 0;
    } else if (indexRingOffset_ + idxBytes > indexBufCap_) {
      glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(indexBufCap_), nullptr,
                   GL_STREAM_DRAW);
      indexRingOffset_ = 0;
    }
    idxOff = indexRingOffset_;
    void* idxMapped = glMapBufferRange(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLintptr>(idxOff),
                                       static_cast<GLsizeiptr>(idxBytes),
                                       GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT);
    if (idxMapped) {
      std::memcpy(idxMapped, drawIdx, idxBytes);
      glUnmapBuffer(GL_ELEMENT_ARRAY_BUFFER);
    } else {
      glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLintptr>(idxOff),
                      static_cast<GLsizeiptr>(idxBytes), drawIdx);
    }
    indexRingOffset_ = (idxOff + idxBytes + 15u) & ~15u;
  }
  const auto* idxPtr = reinterpret_cast<const void*>(static_cast<uintptr_t>(idxOff));
  if (baseVertex != 0) {
    glDrawElementsBaseVertex(glTopo, static_cast<GLsizei>(drawCount), GL_UNSIGNED_INT, idxPtr,
                             baseVertex);
  } else {
    glDrawElements(glTopo, static_cast<GLsizei>(drawCount), GL_UNSIGNED_INT, idxPtr);
  }
  return true;
}

void Gles3Backend::UploadUboPresent(const PresentParamsCpu& pp) {
  const size_t slot = (sizeof(pp) + uboAlign_ - 1u) & ~(uboAlign_ - 1u);
  uboRingOffset_ = (uboRingOffset_ + uboAlign_ - 1u) & ~(uboAlign_ - 1u);
  if (uboRingPtr_) {
    const size_t halfSize = uboRingSize_ / 2u;
    const size_t halfEnd = uboHalf_ ? uboRingSize_ : halfSize;
    if (uboRingOffset_ + slot > halfEnd) {
      if (uboHalfFence_[uboHalf_]) glDeleteSync(uboHalfFence_[uboHalf_]);
      uboHalfFence_[uboHalf_] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
      uboHalf_ ^= 1;
      if (uboHalfFence_[uboHalf_]) {
        glClientWaitSync(uboHalfFence_[uboHalf_], GL_SYNC_FLUSH_COMMANDS_BIT, 500000000ULL);
        glDeleteSync(uboHalfFence_[uboHalf_]);
        uboHalfFence_[uboHalf_] = nullptr;
      }
      uboRingOffset_ = uboHalf_ ? halfSize : 0u;
    }
    const size_t off = uboRingOffset_;
    std::memcpy(uboRingPtr_ + off, &pp, sizeof(pp));
    uboRingOffset_ = off + slot;
    glBindBufferRange(GL_UNIFORM_BUFFER, 4, uboRing_, static_cast<GLintptr>(off),
                      static_cast<GLsizeiptr>(sizeof(pp)));
    return;
  }
  glBindBuffer(GL_UNIFORM_BUFFER, uboPresent_);
  glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(pp), &pp);
  glBindBufferBase(GL_UNIFORM_BUFFER, 4, uboPresent_);
  glBindBuffer(GL_UNIFORM_BUFFER, 0);
}

bool Gles3Backend::ExecuteResolve(const uint32_t* regs) {
  const uint32_t control = regs[kRegCopyControl];
  const uint32_t destBase = regs[kRegCopyDestBase];
  const uint32_t destInfo = regs[kRegCopyDestInfo];
  const uint32_t surfaceInfo = regs[kRegSurfaceInfo];
  const uint32_t pitch = surfaceInfo & 0x3FFFu;
  if (pitch == 0) return false;

  const uint32_t tl = regs[kRegScissorTopLeft];
  const uint32_t br = regs[kRegScissorBottomRight];
  const uint32_t x0 = tl & 0x7FFFu;
  const uint32_t y0 = (tl >> 16) & 0x7FFFu;
  uint32_t x1 = br & 0x7FFFu;
  const uint32_t y1 = (br >> 16) & 0x7FFFu;
  if (x1 > pitch) x1 = pitch;
  if (x1 <= x0 || y1 <= y0 || x1 - x0 > 4096 || y1 - y0 > 4096) return false;

  const uint32_t width = x1 - x0;
  const uint32_t height = y1 - y0;
  const uint32_t sourceSelect = control & 7u;
  const uint32_t command = (control >> 20) & 3u;
  const bool colorClear = ((control >> 8) & 1u) != 0;
  const bool depthClear = ((control >> 9) & 1u) != 0;

  resources_.EnsureRows(surfaceInfo, y1);
  uint32_t targetW = 0, targetH = 0;
  float scaleX = 1.0f, scaleY = 1.0f;
  resources_.TargetSize(surfaceInfo, 4, targetW, targetH, scaleX, scaleY);

  const float hostX0 = static_cast<float>(x0) * scaleX;
  const float hostY0 = static_cast<float>(y0) * scaleY;
  const uint32_t hostW = std::max(1u, static_cast<uint32_t>(std::lround(width * scaleX)));
  const uint32_t hostH = std::max(1u, static_cast<uint32_t>(std::lround(height * scaleY)));

  const uint32_t colorInfos[4] = {
      regs[kRegColorInfo0], regs[kRegColorInfo1], regs[kRegColorInfo2], regs[kRegColorInfo3]};
  const uint32_t depthInfo = regs[kRegDepthInfo];

  // An EDRAM->host resolve only needs to run when the source has been drawn into
  // since the previous resolve with identical parameters. Clears also change the
  // source, so they invalidate the cached key below.
  ResolveKey key{};
  key.control = control;
  key.destBase = destBase;
  key.destInfo = destInfo;
  key.surfaceInfo = surfaceInfo;
  key.tl = tl;
  key.br = br;
  key.depthInfo = depthInfo;
  key.frontbufferCandidate = (sourceSelect != 4u && width >= 480u && height >= 240u);
  std::memcpy(key.colorInfo, colorInfos, sizeof(colorInfos));
  const bool isCopy = (command != 3u && destBase != 0);
  // Never skip a frontbuffer resolve: SetLastFrontbuffer() and resolved-texture
  // re-creation on guest writes must still run so present cannot show a stale surface.
  const bool skipCopy = isCopy && !key.frontbufferCandidate && drawsSinceResolve_ == 0 &&
                        lastResolveKey_ == key;
  if (skipCopy) ++resolveStats_.skipped;
  lastResolveKey_ = key;
  lastResolveKey_.valid = isCopy && !colorClear && !depthClear;
  drawsSinceResolve_ = 0;

  bool didAny = false;

  // 1) Copy/resolve RT into GPU resolved texture
  if (isCopy && !skipCopy) {
    const bool fromDepth = (sourceSelect == 4u);
    GLuint srcTex = 0;
    uint32_t srcW = 1, srcH = 1;
    GLenum dstFmt = GL_RGBA8;
    if (fromDepth) {
      const auto* dt = resources_.DepthTargetFor(depthInfo, surfaceInfo);
      if (dt && dt->tex) {
        srcTex = dt->tex;
        srcW = dt->width;
        srcH = dt->height;
        dstFmt = GL_R32F;
      }
    } else {
      const uint32_t info = colorInfos[sourceSelect < 4u ? sourceSelect : 0u];
      const auto* ct = resources_.ColorTargetFor(info, surfaceInfo);
      if (ct && ct->tex) {
        srcTex = ct->tex;
        srcW = ct->width;
        srcH = ct->height;
        dstFmt = ct->internalFormat;
      }
    }

    if (srcTex) {
      auto* dst = resources_.ResolvedFor(destBase, hostW, hostH, dstFmt, fromDepth);
      if (dst && dst->tex) {
        dst->guestWidth = width;
        dst->guestHeight = height;
        dst->rbSwapped = !fromDepth && (((destInfo >> 24) & 1u) != 0);
        dst->lastWriteSeq = resources_.CurrentWriteSeq();
        if (!fromDepth && width >= 480 && height >= 240) {
          resources_.SetLastFrontbuffer(dst);
        }

        float colorScale = 1.0f;
        float clampMode = 0.0f;
        int32_t expBias = 0;
        uint8_t clampModeI = 0;
        if (!fromDepth) {
          int32_t eb = static_cast<int32_t>((destInfo >> 16) & 0x3Fu);
          if (eb >= 32) eb -= 64;
          expBias = eb;
          colorScale = std::ldexp(1.0f, eb);
          const uint32_t destFmt = (destInfo >> 7) & 0x3Fu;
          const uint32_t destNum = (destInfo >> 13) & 7u;
          const bool isFloat = (destNum == 7u) || destFmt == 30u || destFmt == 31u ||
                               destFmt == 32u || destFmt == 36u || destFmt == 37u || destFmt == 38u;
          clampMode = isFloat ? 0.0f : (destNum == 0u ? 1.0f : (destNum == 1u ? 2.0f : 0.0f));
          clampModeI = isFloat ? 0u : (destNum == 0u ? 1u : (destNum == 1u ? 2u : 0u));
        }

        // Hardware direct texture copy fast path (GLES 3.2 core glCopyImageSubData):
        // skips FBO switch, state invalidation, shader/UBO bind, and full-screen draw!
        const int32_t srcX0 = static_cast<int32_t>(std::lround(hostX0));
        const int32_t srcY0 = static_cast<int32_t>(std::lround(hostY0));
        const bool boundsOk = srcX0 >= 0 && srcY0 >= 0 &&
            static_cast<uint32_t>(srcX0) + hostW <= srcW &&
            static_cast<uint32_t>(srcY0) + hostH <= srcH;

        // The resolved texture is always allocated with the SOURCE GL format
        // (dstFmt is taken from ct->internalFormat above), so glCopyImageSubData is
        // always format-compatible. The exponent bias and clamp the old shader path
        // applied at resolve time are deferred to sample time via textureAdjustment[]
        // (see bindStageTextures), which makes scaled resolves eligible for the fast
        // path too. Frontbuffers are covered as well: ExecutePresent applies the
        // stored bias via PresentParams::w before its final clamp.
        const bool canDirectCopy = !fromDepth && boundsOk;

        if (fromDepth) ++resolveStats_.depth;
        else ++resolveStats_.color;
        resolveStats_.pixels += static_cast<uint64_t>(hostW) * hostH;

        if (canDirectCopy) {
          ++resolveStats_.directCopy;
          dst->expBias = expBias;
          dst->clampMode = clampModeI;
          glCopyImageSubData(srcTex, GL_TEXTURE_2D, 0, srcX0, srcY0, 0,
                             dst->tex, GL_TEXTURE_2D, 0, 0, 0, 0,
                             static_cast<GLsizei>(hostW), static_cast<GLsizei>(hostH), 1);
          didAny = true;
        } else {
          dst->expBias = 0;
          dst->clampMode = 0;
          UnbindTexturesMatching(dst->tex);
          drawFboBound_ = false;
          stateDirty_ = true;
          lastProg_ = 0;
          glBindFramebuffer(GL_FRAMEBUFFER, resolveFbo_);
          glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dst->tex, 0);
          glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
          const GLenum db = GL_COLOR_ATTACHMENT0;
          glDrawBuffers(1, &db);
          // Discard previous dst->tex contents so Adreno skips loading old tiles from RAM into GMEM
          glInvalidateFramebuffer(GL_FRAMEBUFFER, 1, &db);

          glDisable(GL_SCISSOR_TEST);
          glDisable(GL_DEPTH_TEST);
          glDisable(GL_STENCIL_TEST);
          glDisable(GL_CULL_FACE);
          glDisablei(GL_BLEND, 0);
          glColorMaski(0, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
          glViewport(0, 0, static_cast<GLsizei>(hostW), static_cast<GLsizei>(hostH));

          PresentParamsCpu pp{};
          pp.uvRect[0] = hostX0 / static_cast<float>(srcW);
          pp.uvRect[1] = hostY0 / static_cast<float>(srcH);
          pp.uvRect[2] = static_cast<float>(hostW) / static_cast<float>(srcW);
          pp.uvRect[3] = static_cast<float>(hostH) / static_cast<float>(srcH);
          pp.params[0] = 0.0f;
          pp.params[1] = colorScale;
          pp.params[2] = clampMode;
          pp.params[3] = fromDepth ? 1.0f : 0.0f;
          UploadUboPresent(pp);

          // GL_DEPTH24_STENCIL8 is NOT linear-filterable in GLES 3.2 (binding GL_LINEAR makes it
          // incomplete -> returns 0.0 for all shadow/depth resolves!). Always use NEAREST for depth.
          const GLuint resolveSmp =
              fromDepth ? resources_.NearestClampSampler() : resources_.LinearClampSampler();
          glActiveTexture(GL_TEXTURE0);
          glBindTexture(GL_TEXTURE_2D, srcTex);
          glBindSampler(0, resolveSmp);
          boundTex_[0] = srcTex;
          boundTexTarget_[0] = GL_TEXTURE_2D;
          boundSampler_[0] = resolveSmp;

          glUseProgram(shaders_.ResolveCopyProgram());
          glBindVertexArray(emptyVao_);
          glDrawArrays(GL_TRIANGLES, 0, 3);
          didAny = true;
        }
      }
    }
  }

  // 2) Color & Depth/stencil clear (combined into a single FBO pass when both targets match size)
  const int32_t cx0 = static_cast<int32_t>(std::lround(hostX0));
  const int32_t cy0 = static_cast<int32_t>(std::lround(hostY0));
  const int32_t cx1 = static_cast<int32_t>(std::lround(static_cast<float>(x1) * scaleX));
  const int32_t cy1 = static_cast<int32_t>(std::lround(static_cast<float>(y1) * scaleY));

  auto decodeClearDepth = [&]() -> float {
    const uint32_t v = regs[kRegCopyDepthClear];
    const bool isFloat24 = ((depthInfo >> 16) & 1u) != 0;
    const uint32_t raw24 = v >> 8;
    if (isFloat24) {
      if (raw24 == 0) return 0.0f;
      if (raw24 >= 0xFFFFFFu) return 1.0f;
      const uint32_t exp = raw24 >> 20;
      const uint32_t mant = raw24 & 0xFFFFFu;
      if (exp == 0) return static_cast<float>(mant) / 17179869184.0f;
      const uint32_t fbits = ((exp + 112u) << 23) | (mant << 3);
      float f = 1.0f;
      std::memcpy(&f, &fbits, 4);
      return f;
    }
    return static_cast<float>(raw24) / 16777215.0f;
  };

  const Gles3Resources::ColorTarget* clearCt =
      colorClear ? resources_.ColorTargetFor(colorInfos[0], surfaceInfo) : nullptr;
  const Gles3Resources::DepthTarget* clearDt =
      depthClear ? resources_.DepthTargetFor(depthInfo, surfaceInfo) : nullptr;
  if (clearCt && !clearCt->tex) clearCt = nullptr;
  if (clearDt && !clearDt->tex) clearDt = nullptr;

  if (clearCt && clearDt && clearCt->width == clearDt->width && clearCt->height == clearDt->height) {
    UnbindTexturesMatching(clearCt->tex);
    UnbindTexturesMatching(clearDt->tex);
    drawFboBound_ = false;
    stateDirty_ = true;
    glBindFramebuffer(GL_FRAMEBUFFER, resolveFbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, clearCt->tex, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, clearDt->tex,
                           0);
    const GLenum db = GL_COLOR_ATTACHMENT0;
    glDrawBuffers(1, &db);
    glEnable(GL_SCISSOR_TEST);
    glScissor(cx0, cy0, std::max(0, cx1 - cx0), std::max(0, cy1 - cy0));
    glColorMaski(0, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glStencilMask(0xFFu);
    if (cx0 == 0 && cy0 == 0 && cx1 >= static_cast<int32_t>(clearCt->width) &&
        cy1 >= static_cast<int32_t>(clearCt->height)) {
      const GLenum inv[2] = {GL_COLOR_ATTACHMENT0, GL_DEPTH_STENCIL_ATTACHMENT};
      glInvalidateFramebuffer(GL_FRAMEBUFFER, 2, inv);
    }
    const uint32_t cv = regs[kRegCopyColorClear];
    glClearColor(((cv >> 16) & 0xFFu) / 255.0f, ((cv >> 8) & 0xFFu) / 255.0f,
                 (cv & 0xFFu) / 255.0f, ((cv >> 24) & 0xFFu) / 255.0f);
    glClearDepthf(decodeClearDepth());
    glClearStencil(static_cast<GLint>(regs[kRegCopyDepthClear] & 0xFFu));
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    didAny = true;
  } else {
    if (clearCt) {
      UnbindTexturesMatching(clearCt->tex);
      drawFboBound_ = false;
      stateDirty_ = true;
      glBindFramebuffer(GL_FRAMEBUFFER, resolveFbo_);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, clearCt->tex, 0);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
      const GLenum db = GL_COLOR_ATTACHMENT0;
      glDrawBuffers(1, &db);
      glEnable(GL_SCISSOR_TEST);
      glScissor(cx0, cy0, std::max(0, cx1 - cx0), std::max(0, cy1 - cy0));
      glColorMaski(0, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
      if (cx0 == 0 && cy0 == 0 && cx1 >= static_cast<int32_t>(clearCt->width) &&
          cy1 >= static_cast<int32_t>(clearCt->height)) {
        const GLenum inv = GL_COLOR_ATTACHMENT0;
        glInvalidateFramebuffer(GL_FRAMEBUFFER, 1, &inv);
      }
      const uint32_t v = regs[kRegCopyColorClear];
      glClearColor(((v >> 16) & 0xFFu) / 255.0f, ((v >> 8) & 0xFFu) / 255.0f,
                   (v & 0xFFu) / 255.0f, ((v >> 24) & 0xFFu) / 255.0f);
      glClear(GL_COLOR_BUFFER_BIT);
      didAny = true;
    }

    if (clearDt) {
      UnbindTexturesMatching(clearDt->tex);
      drawFboBound_ = false;
      stateDirty_ = true;
      glBindFramebuffer(GL_FRAMEBUFFER, resolveFbo_);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D,
                             clearDt->tex, 0);
      glEnable(GL_SCISSOR_TEST);
      glScissor(cx0, cy0, std::max(0, cx1 - cx0), std::max(0, cy1 - cy0));
      glDepthMask(GL_TRUE);
      glStencilMask(0xFFu);
      if (cx0 == 0 && cy0 == 0 && cx1 >= static_cast<int32_t>(clearDt->width) &&
          cy1 >= static_cast<int32_t>(clearDt->height)) {
        const GLenum inv = GL_DEPTH_STENCIL_ATTACHMENT;
        glInvalidateFramebuffer(GL_FRAMEBUFFER, 1, &inv);
      }
      glClearDepthf(decodeClearDepth());
      glClearStencil(static_cast<GLint>(regs[kRegCopyDepthClear] & 0xFFu));
      glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
      didAny = true;
    }
  }

  return didAny;
}

void Gles3Backend::ExecutePresent(uint32_t fbAddr, uint32_t fbWidth, uint32_t fbHeight,
                                  bool useLanczos, bool letterbox, bool vsync,
                                  const uint32_t* gammaLut, uint64_t gammaVersion, bool hasGamma) {
  UpdatePendingSurface();
  if (surface_ == EGL_NO_SURFACE) return;

  if (vsync != currentVsync_) {
    currentVsync_ = vsync;
    eglSwapInterval(display_, vsync ? 1 : 0);
  }

  const Gles3Resources::Resolved* fb = resources_.ResolvedAt(fbAddr);
  if (!fb) fb = resources_.LastFrontbuffer();
  if (!fb || !fb->tex) return;

  if (hasGamma && gammaLut) {
    resources_.UpdateGammaLut(gammaLut, gammaVersion);
  }

  EGLint winW = static_cast<EGLint>(surfaceWidth_.load(std::memory_order_relaxed));
  EGLint winH = static_cast<EGLint>(surfaceHeight_.load(std::memory_order_relaxed));
  if (winW <= 0 || winH <= 0) {
    eglQuerySurface(display_, surface_, EGL_WIDTH, &winW);
    eglQuerySurface(display_, surface_, EGL_HEIGHT, &winH);
  }
  if (winW <= 0 || winH <= 0) return;

  drawFboBound_ = false;
  stateDirty_ = true;
  lastProg_ = 0;
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_STENCIL_TEST);
  glDisable(GL_CULL_FACE);
  glDisablei(GL_BLEND, 0);
  glColorMaski(0, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  // Compute aspect-preserving letterbox viewport
  GLint vpX = 0, vpY = 0, vpW = winW, vpH = winH;
  if (letterbox) {
    const float srcAspect = (fbWidth > 0 && fbHeight > 0)
                                ? (static_cast<float>(fbWidth) / static_cast<float>(fbHeight))
                                : (16.0f / 9.0f);
    const float dstAspect = static_cast<float>(winW) / static_cast<float>(winH);
    if (dstAspect > srcAspect) {
      vpW = std::max<GLint>(1, static_cast<GLint>(std::lround(winH * srcAspect)));
      vpX = (winW - vpW) / 2;
    } else if (dstAspect < srcAspect) {
      vpH = std::max<GLint>(1, static_cast<GLint>(std::lround(winW / srcAspect)));
      vpY = (winH - vpH) / 2;
    }
  }
  if (vpX > 0 || vpY > 0 || vpW < winW || vpH < winH) {
    glViewport(0, 0, winW, winH);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
  }
  glViewport(vpX, vpY, vpW, vpH);

  PresentParamsCpu pp{};
  pp.uvRect[0] = 0.0f;
  pp.uvRect[1] = 0.0f;
  pp.uvRect[2] = 1.0f;
  pp.uvRect[3] = 1.0f;
  pp.params[0] = resources_.HasGammaLut() ? 1.0f : 0.0f;
  pp.params[1] = static_cast<float>(fb->width);
  pp.params[2] = static_cast<float>(fb->height);
  // Exponent bias deferred from a fast-path resolve; present does not go through
  // textureAdjust(), so apply it here before the final clamp.
  pp.params[3] = static_cast<float>(fb->expBias);

  UploadUboPresent(pp);

  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, fb->tex);
  glBindSampler(0, resources_.LinearClampSampler());
  boundTex_[0] = fb->tex;
  boundTexTarget_[0] = GL_TEXTURE_2D;
  boundSampler_[0] = resources_.LinearClampSampler();

  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, resources_.GammaTexture());
  glBindSampler(1, resources_.LinearClampSampler());
  boundTex_[1] = resources_.GammaTexture();
  boundTexTarget_[1] = GL_TEXTURE_2D;
  boundSampler_[1] = resources_.LinearClampSampler();

  // Use Lanczos2 when upscaling or explicitly requested; bilinear when downscaling >= 1.05x.
  const bool isDownscaling = (static_cast<int>(fb->width) > vpW + 4) ||
                             (static_cast<int>(fb->height) > vpH + 4);
  const GLuint prog = (useLanczos && !isDownscaling) ? shaders_.PresentLanczosProgram()
                                                     : shaders_.PresentBilinearProgram();
  glUseProgram(prog);
  glBindVertexArray(emptyVao_);
  glDrawArrays(GL_TRIANGLES, 0, 3);

  eglSwapBuffers(display_, surface_);
  g_android_custom_present_count.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace bs1::renderer::gles3
