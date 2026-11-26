#include "gles3_shaders.h"

#include <android/log.h>
#include <pthread.h>
#include <sys/resource.h>

#include <atomic>
#include <cstring>
#include <fstream>
#include <sstream>

#include "bs1_filelog.h"
#define GLESLOG(...) BS1_GLES_LOG_IMPL(ANDROID_LOG_INFO, __VA_ARGS__)
#define GLESERR(...) BS1_GLES_LOG_IMPL(ANDROID_LOG_ERROR, __VA_ARGS__)

namespace bs1::renderer::gles3 {

namespace {

constexpr uint32_t kBinaryCacheMagic = 0x32474253u;  // 'SBG2'

const char* const kNullFsSource = R"GLSL(#version 320 es
precision highp float;
void main() {}
)GLSL";

std::string MakeRectangleGsSource(uint32_t numInterps) {
  if (numInterps > 16u) numInterps = 16u;
  std::ostringstream ss;
  ss << "#version 320 es\n"
     << "precision highp float;\n"
     << "layout(triangles) in;\n"
     << "layout(triangle_strip, max_vertices = 4) out;\n";
  for (uint32_t i = 0; i < numInterps; ++i) {
    ss << "layout(location = " << i << ") in vec4 gi" << i << "[3];\n";
  }
  for (uint32_t i = 0; i < numInterps; ++i) {
    ss << "layout(location = " << i << ") out vec4 v_o" << i << ";\n";
  }
  ss << "void emitIdx(int i) {\n"
     << "  gl_Position = gl_in[i].gl_Position;\n";
  for (uint32_t i = 0; i < numInterps; ++i) {
    ss << "  v_o" << i << " = gi" << i << "[i];\n";
  }
  ss << "  EmitVertex();\n"
     << "}\n"
     << "void emitFourth(int a, int c, int b) {\n"
     << "  gl_Position = gl_in[a].gl_Position + gl_in[b].gl_Position - gl_in[c].gl_Position;\n";
  for (uint32_t i = 0; i < numInterps; ++i) {
    ss << "  v_o" << i << " = gi" << i << "[a] + gi" << i << "[b] - gi" << i << "[c];\n";
  }
  ss << "  EmitVertex();\n"
     << "}\n"
     << "void main() {\n"
     << "  vec2 p0 = gl_in[0].gl_Position.xy / gl_in[0].gl_Position.w;\n"
     << "  vec2 p1 = gl_in[1].gl_Position.xy / gl_in[1].gl_Position.w;\n"
     << "  vec2 p2 = gl_in[2].gl_Position.xy / gl_in[2].gl_Position.w;\n"
     << "  float d01 = dot(p1 - p0, p1 - p0);\n"
     << "  float d12 = dot(p2 - p1, p2 - p1);\n"
     << "  float d02 = dot(p2 - p0, p2 - p0);\n"
     << "  int c = (d02 >= d01 && d02 >= d12) ? 1 : (d01 >= d12 ? 2 : 0);\n"
     << "  int a = (c + 1) % 3;\n"
     << "  int b = (c + 2) % 3;\n"
     << "  emitIdx(c);\n"
     << "  emitIdx(a);\n"
     << "  emitIdx(b);\n"
     << "  emitFourth(a, c, b);\n"
     << "  EndPrimitive();\n"
     << "}\n";
  return ss.str();
}

// Present VS: flips V so row 0 of our GL_LOWER_LEFT offscreen FBO maps to TOP of FBO 0 (screen).
const char* const kPresentVsSource = R"GLSL(#version 320 es
precision highp float;
layout(location = 0) out vec2 v_uv;
void main() {
  vec2 t = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
  gl_Position = vec4(t * 2.0 - 1.0, 0.0, 1.0);
  v_uv = vec2(t.x, 1.0 - t.y);
}
)GLSL";

const char* const kPresentBilinearFsSource = R"GLSL(#version 320 es
precision highp float;
layout(binding = 0) uniform highp sampler2D u_src;
layout(binding = 1) uniform highp sampler2D u_gamma;
layout(std140, binding = 4) uniform PresentParams {
  vec4 u_uvRect;   // xy = offset, zw = size
  vec4 u_params;   // x = useGamma, y = srcW, z = srcH, w = resolve exp bias
};
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 frag_color;

vec3 applyGamma(vec3 c) {
  if (u_params.x < 0.5) return c;
  vec3 at = clamp(c, 0.0, 1.0) * (255.0 / 256.0) + (0.5 / 256.0);
  float r = textureLod(u_gamma, vec2(at.r, 0.5), 0.0).r;
  float g = textureLod(u_gamma, vec2(at.g, 0.5), 0.0).g;
  float b = textureLod(u_gamma, vec2(at.b, 0.5), 0.0).b;
  return vec3(r, g, b);
}

void main() {
  vec2 uv = u_uvRect.xy + v_uv * u_uvRect.zw;
  vec3 c = textureLod(u_src, uv, 0.0).rgb;
  c *= exp2(u_params.w);
  frag_color = vec4(applyGamma(c), 1.0);
}
)GLSL";

const char* const kPresentLanczosFsSource = R"GLSL(#version 320 es
precision highp float;
layout(binding = 0) uniform highp sampler2D u_src;
layout(binding = 1) uniform highp sampler2D u_gamma;
layout(std140, binding = 4) uniform PresentParams {
  vec4 u_uvRect;   // xy = offset, zw = size
  vec4 u_params;   // x = useGamma, y = srcW, z = srcH, w = resolve exp bias
};
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 frag_color;

vec4 LanczosWeights(float f) {
  // Fast Catmull-Rom / Lanczos2 polynomial approximation (eliminates 6 transcendental sin/cos
  // + 8 divisions per 2248x1080 screen pixel while preserving identical 4-tap sharpening).
  float f2 = f * f;
  float f3 = f2 * f;
  return vec4(-0.5 * f + f2 - 0.5 * f3,
               1.0 - 2.5 * f2 + 1.5 * f3,
               0.5 * f + 2.0 * f2 - 1.5 * f3,
              -0.5 * f2 + 0.5 * f3);
}

vec3 applyGamma(vec3 c) {
  if (u_params.x < 0.5) return c;
  vec3 at = clamp(c, 0.0, 1.0) * (255.0 / 256.0) + (0.5 / 256.0);
  float r = textureLod(u_gamma, vec2(at.r, 0.5), 0.0).r;
  float g = textureLod(u_gamma, vec2(at.g, 0.5), 0.0).g;
  float b = textureLod(u_gamma, vec2(at.b, 0.5), 0.0).b;
  return vec3(r, g, b);
}

void main() {
  vec2 srcSize = max(vec2(textureSize(u_src, 0)), vec2(1.0));
  vec2 uv = u_uvRect.xy + v_uv * u_uvRect.zw;
  vec2 p = uv * srcSize - 0.5;
  vec2 f = fract(p);
  vec2 base = floor(p);
  vec4 wx = LanczosWeights(f.x);
  vec4 wy = LanczosWeights(f.y);
  vec2 w0 = vec2(wx.x, wy.x);
  vec2 w1 = vec2(wx.y, wy.y);
  vec2 w2 = vec2(wx.z, wy.z);
  vec2 w3 = vec2(wx.w, wy.w);
  vec2 w12 = w1 + w2;
  vec2 inv = 1.0 / srcSize;
  vec2 t0 = (base - 0.5) * inv;
  vec2 t12 = (base + 0.5 + w2 / w12) * inv;
  vec2 t3 = (base + 2.5) * inv;

  vec3 s00 = textureLod(u_src, vec2(t0.x,  t0.y),  0.0).rgb;
  vec3 s10 = textureLod(u_src, vec2(t12.x, t0.y),  0.0).rgb;
  vec3 s20 = textureLod(u_src, vec2(t3.x,  t0.y),  0.0).rgb;
  vec3 s01 = textureLod(u_src, vec2(t0.x,  t12.y), 0.0).rgb;
  vec3 s11 = textureLod(u_src, vec2(t12.x, t12.y), 0.0).rgb;
  vec3 s21 = textureLod(u_src, vec2(t3.x,  t12.y), 0.0).rgb;
  vec3 s02 = textureLod(u_src, vec2(t0.x,  t3.y),  0.0).rgb;
  vec3 s12 = textureLod(u_src, vec2(t12.x, t3.y),  0.0).rgb;
  vec3 s22 = textureLod(u_src, vec2(t3.x,  t3.y),  0.0).rgb;

  vec3 acc = s00 * (w0.x  * w0.y)  + s10 * (w12.x * w0.y)  + s20 * (w3.x * w0.y) +
             s01 * (w0.x  * w12.y) + s11 * (w12.x * w12.y) + s21 * (w3.x * w12.y) +
             s02 * (w0.x  * w3.y)  + s12 * (w12.x * w3.y)  + s22 * (w3.x * w3.y);

  vec3 mn = min(min(min(s01, s11), min(s21, s10)), s12);
  vec3 mx = max(max(max(s01, s11), max(s21, s10)), s12);

  vec2 sum = w0 + w12 + w3;
  vec3 c = clamp(acc / (sum.x * sum.y), mn, mx) * exp2(u_params.w);
  c = clamp(c, 0.0, 1.0);
  frag_color = vec4(applyGamma(c), 1.0);
}
)GLSL";

// Resolve/Copy VS: NO V-flip (offscreen FBO -> offscreen FBO, row 0 stays row 0).
const char* const kResolveVsSource = R"GLSL(#version 320 es
precision highp float;
layout(location = 0) out vec2 v_uv;
void main() {
  vec2 t = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
  gl_Position = vec4(t * 2.0 - 1.0, 0.0, 1.0);
  v_uv = t;
}
)GLSL";

const char* const kResolveCopyFsSource = R"GLSL(#version 320 es
precision highp float;
layout(binding = 0) uniform highp sampler2D u_src;
layout(std140, binding = 4) uniform ResolveParams {
  vec4 u_uvRect;   // xy = offset, zw = size
  vec4 u_params;   // x = unused, y = colorScale, z = clampMode (1=unorm,2=snorm), w = fromDepth
};
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 frag_color;
void main() {
  vec2 uv = u_uvRect.xy + v_uv * u_uvRect.zw;
  vec4 c = textureLod(u_src, uv, 0.0);
  if (u_params.w > 0.5) {
    frag_color = vec4(c.r, c.r, c.r, 1.0);
    return;
  }
  c *= u_params.y;
  if (u_params.z > 0.5 && u_params.z < 1.5) {
    c = clamp(c, 0.0, 1.0);
  } else if (u_params.z >= 1.5) {
    c = clamp(c, -1.0, 1.0);
  }
  frag_color = c;
}
)GLSL";

}  // namespace

uint64_t Gles3Shaders::HashWords(const uint32_t* words, uint32_t count, bool pixel) {
  uint64_t h = (pixel ? 0x9E3779B97F4A7C15ULL : 0x517CC1B727220A95ULL) ^ XenosGlsl::Version();
  for (uint32_t i = 0; i < count; ++i) {
    h ^= words[i];
    h *= 1099511628211ULL;
  }
  return h;
}

void Gles3Shaders::Initialize(const std::filesystem::path& cacheDir, EGLDisplay display,
                              EGLConfig config, EGLContext sharedContext, bool noErrorContext) {
  Shutdown();
  cacheDir_ = cacheDir;
  if (!cacheDir_.empty()) {
    std::error_code ec;
    std::filesystem::create_directories(cacheDir_, ec);
  }
  GLint numFormats = 0;
  glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &numFormats);
  hasBinaryFormats_ = numFormats > 0;

  stages_.clear();
  stages_.emplace_back();  // 0 = invalid sentinel

  nullFs_ = CompileShader(GL_FRAGMENT_SHADER, kNullFsSource, "nullFS");
  for (uint32_t i = 0; i <= 16; ++i) {
    rectGs_[i] = CompileShader(GL_GEOMETRY_SHADER, MakeRectangleGsSource(i), "rectGS");
  }
  presentLanczosProg_ = LinkBuiltin(kPresentVsSource, kPresentLanczosFsSource, "presentLanczos");
  presentBilinearProg_ = LinkBuiltin(kPresentVsSource, kPresentBilinearFsSource, "presentBilinear");
  resolveCopyProg_ = LinkBuiltin(kResolveVsSource, kResolveCopyFsSource, "resolveCopy");
  PrewarmCache();

  // Start background shared-EGLContext shader compiler pool (3 worker threads) so brand-new
  // shaders compile, link, and save .glesbin in parallel off the render thread.
  asyncReady_ = false;
  compileStop_ = false;
  workersFinishedInit_ = 0;
  if (display != EGL_NO_DISPLAY && config != nullptr && sharedContext != EGL_NO_CONTEXT) {
    // Flush main context before sharing so any newly created objects are visible.
    glFlush();
    for (uint32_t i = 0; i < kNumCompileWorkers; ++i) {
      compileThreads_[i] = std::thread(&Gles3Shaders::CompileWorkerMain, this, display, config,
                                       sharedContext, noErrorContext);
    }
    // Wait briefly for all workers to initialize their shared EGLContexts.
    std::unique_lock lock(compileMutex_);
    compileCv_.wait(lock, [this] { return workersFinishedInit_ >= kNumCompileWorkers; });
  }

  GLESLOG("Gles3Shaders initialized (binaryFormats=%d, prewarmed=%zu, asyncCompile=%d, "
          "rectGS=%u, lanczos=%u, resolve=%u)",
          numFormats, prewarmed_.size(), asyncReady_ ? 1 : 0, rectGs_[0], presentLanczosProg_,
          resolveCopyProg_);
}

void Gles3Shaders::Shutdown() {
  {
    std::lock_guard lock(compileMutex_);
    compileStop_ = true;
  }
  compileCv_.notify_all();
  for (uint32_t i = 0; i < kNumCompileWorkers; ++i) {
    if (compileThreads_[i].joinable()) {
      compileThreads_[i].join();
    }
  }
  asyncReady_ = false;
  compileStop_ = false;
  workersFinishedInit_ = 0;
  {
    std::lock_guard lock(compileMutex_);
    pendingJobs_.clear();
  }
  {
    std::lock_guard lock(completedMutex_);
    for (const auto& c : completedJobs_) {
      if (c.prog) glDeleteProgram(c.prog);
    }
    completedJobs_.clear();
    completedCount_.store(0, std::memory_order_relaxed);
  }
  inFlight_.clear();
  for (auto& [k, prog] : prewarmed_) {
    if (prog) glDeleteProgram(prog);
  }
  prewarmed_.clear();
  for (auto& [k, prog] : programs_) {
    if (prog) glDeleteProgram(prog);
  }
  programs_.clear();
  for (Stage& s : stages_) {
    if (s.shader) glDeleteShader(s.shader);
    for (uint32_t v = 1; v < 8; ++v) {
      if (s.shaderVariants[v]) glDeleteShader(s.shaderVariants[v]);
    }
  }
  stages_.clear();
  byHash_.clear();
  if (nullFs_) { glDeleteShader(nullFs_); nullFs_ = 0; }
  for (uint32_t i = 0; i <= 16; ++i) {
    if (rectGs_[i]) { glDeleteShader(rectGs_[i]); rectGs_[i] = 0; }
  }
  if (presentLanczosProg_) { glDeleteProgram(presentLanczosProg_); presentLanczosProg_ = 0; }
  if (presentBilinearProg_) { glDeleteProgram(presentBilinearProg_); presentBilinearProg_ = 0; }
  if (resolveCopyProg_) { glDeleteProgram(resolveCopyProg_); resolveCopyProg_ = 0; }
  current_[0] = current_[1] = 0;
  binaryCacheHits_ = 0;
}

void Gles3Shaders::CompileWorkerMain(EGLDisplay display, EGLConfig config,
                                     EGLContext sharedContext, bool noErrorContext) {
  pthread_setname_np(pthread_self(), "BS1-ShaderBg");
  setpriority(PRIO_PROCESS, 0, 10);
  constexpr EGLint kEglContextOpenGlNoErrorKhr = 0x31B3;
  const EGLint ctxAttribsNoErr[] = {
      EGL_CONTEXT_MAJOR_VERSION, 3,
      EGL_CONTEXT_MINOR_VERSION, 2,
      kEglContextOpenGlNoErrorKhr, EGL_TRUE,
      EGL_NONE,
  };
  const EGLint ctxAttribsStd[] = {
      EGL_CONTEXT_MAJOR_VERSION, 3,
      EGL_CONTEXT_MINOR_VERSION, 2,
      EGL_NONE,
  };
  EGLContext bgCtx = eglCreateContext(display, config, sharedContext,
                                      noErrorContext ? ctxAttribsNoErr : ctxAttribsStd);
  const EGLint pbufAttribs[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
  EGLSurface bgPbuf =
      (bgCtx != EGL_NO_CONTEXT) ? eglCreatePbufferSurface(display, config, pbufAttribs)
                                : EGL_NO_SURFACE;
  const bool madeCurrent =
      (bgCtx != EGL_NO_CONTEXT && bgPbuf != EGL_NO_SURFACE &&
       eglMakeCurrent(display, bgPbuf, bgPbuf, bgCtx) == EGL_TRUE);

  if (!madeCurrent) {
    GLESERR("Async shader compiler: shared EGLContext init failed (0x%x)", eglGetError());
    if (bgPbuf != EGL_NO_SURFACE) eglDestroySurface(display, bgPbuf);
    if (bgCtx != EGL_NO_CONTEXT) eglDestroyContext(display, bgCtx);
    eglReleaseThread();
    {
      std::lock_guard lock(compileMutex_);
      ++workersFinishedInit_;
    }
    compileCv_.notify_all();
    return;
  }

  GLuint bgNullFs = CompileShader(GL_FRAGMENT_SHADER, kNullFsSource, "bgNullFS");
  GLuint bgRectGs[17] = {};
  for (uint32_t i = 0; i <= 16; ++i) {
    bgRectGs[i] = CompileShader(GL_GEOMETRY_SHADER, MakeRectangleGsSource(i), "bgRectGS");
  }

  {
    std::lock_guard lock(compileMutex_);
    asyncReady_ = true;
    ++workersFinishedInit_;
  }
  compileCv_.notify_all();

  struct BgStageKey {
    uint64_t hash = 0;
    uint8_t flags = 0;  // bit0=pixel, bit1=alphaTest, bit2=pixelGen
    bool operator==(const BgStageKey& o) const { return hash == o.hash && flags == o.flags; }
  };
  struct BgStageKeyHash {
    size_t operator()(const BgStageKey& k) const {
      return static_cast<size_t>(k.hash ^ (static_cast<uint64_t>(k.flags) * 0x9E3779B97F4A7C15ULL));
    }
  };
  std::unordered_map<BgStageKey, GLuint, BgStageKeyHash> bgStages;

  auto getOrCompile = [&](uint64_t hash, bool pixel, bool alphaTest, bool pixelGen,
                          bool forceHighp, const std::string& glsl) -> GLuint {
    BgStageKey sk{hash, static_cast<uint8_t>((pixel ? 1u : 0u) | (alphaTest ? 2u : 0u) |
                                             (pixelGen ? 4u : 0u) | (forceHighp ? 8u : 0u))};
    if (auto it = bgStages.find(sk); it != bgStages.end()) return it->second;
    std::string src = glsl;
    if (pixel && (alphaTest || pixelGen || forceHighp)) {
      const size_t nl = src.find('\n');
      if (nl != std::string::npos) {
        std::string defs;
        if (alphaTest) defs += "#define BS1_ALPHA_TEST 1\n";
        if (pixelGen) defs += "#define BS1_PIXEL_GEN 1\n";
        if (forceHighp) defs += "#define BS1_HIGHP 1\n";
        src.insert(nl + 1, defs);
      }
    }
    char label[64];
    std::snprintf(label, sizeof(label), "%s_%016llx%s%s%s", pixel ? "PS" : "VS",
                  static_cast<unsigned long long>(hash), alphaTest ? "_AT" : "",
                  pixelGen ? "_PG" : "", forceHighp ? "_HP" : "");
    GLuint sh = CompileShader(pixel ? GL_FRAGMENT_SHADER : GL_VERTEX_SHADER, src, label);
    bgStages[sk] = sh;
    return sh;
  };

  for (;;) {
    CompileJob job;
    {
      std::unique_lock lock(compileMutex_);
      compileCv_.wait(lock, [this] { return compileStop_ || !pendingJobs_.empty(); });
      if (compileStop_) break;
      job = std::move(pendingJobs_.front());
      pendingJobs_.pop_front();
    }

    GLuint vsObj = getOrCompile(job.vsHash, false, false, false, false, job.vsGlsl);
    GLuint fsObj = bgNullFs;
    bool okToLink = (vsObj != 0);
    if (okToLink && job.psHash != 0) {
      fsObj = getOrCompile(job.psHash, true, job.alphaTest, job.pixelGen, job.forceHighp, job.psGlsl);
      if (!fsObj && (job.alphaTest || job.pixelGen || job.forceHighp)) {
        fsObj = getOrCompile(job.psHash, true, false, false, false, job.psGlsl);
      }
      if (!fsObj) okToLink = false;
    }

    GLuint prog = 0;
    if (okToLink) {
      prog = glCreateProgram();
      if (hasBinaryFormats_) {
        glProgramParameteri(prog, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
      }
      glAttachShader(prog, vsObj);
      if (job.rect) {
        const uint32_t gsIdx = std::min(16u, job.psInterps);
        if (bgRectGs[gsIdx]) glAttachShader(prog, bgRectGs[gsIdx]);
      }
      glAttachShader(prog, fsObj);
      glLinkProgram(prog);
      GLint ok = 0;
      glGetProgramiv(prog, GL_LINK_STATUS, &ok);
      if (!ok) {
        char log[2048] = {};
        glGetProgramInfoLog(prog, sizeof(log) - 1, nullptr, log);
        GLESERR("Async LinkProgram(vs=%016llx ps=%016llx rect=%d at=%d pg=%d hp=%d) failed: %s",
                static_cast<unsigned long long>(job.vsHash),
                static_cast<unsigned long long>(job.psHash), job.rect ? 1 : 0,
                job.alphaTest ? 1 : 0, job.pixelGen ? 1 : 0, job.forceHighp ? 1 : 0, log);
        glDeleteProgram(prog);
        prog = 0;
      } else {
        SaveProgramBinary(prog, job.vsHash, job.psHash, job.rect, job.alphaTest, job.pixelGen,
                          job.forceHighp);
        // Ensure the linked program object is visible to the main shared context.
        glFlush();
      }
    }

    {
      std::lock_guard lock(completedMutex_);
      completedJobs_.push_back(CompletedProgram{job.key, prog});
      completedCount_.fetch_add(1u, std::memory_order_release);
    }
  }

  for (auto& [k, sh] : bgStages) {
    if (sh) glDeleteShader(sh);
  }
  if (bgNullFs) glDeleteShader(bgNullFs);
  for (uint32_t i = 0; i <= 16; ++i) {
    if (bgRectGs[i]) glDeleteShader(bgRectGs[i]);
  }
  eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  eglDestroySurface(display, bgPbuf);
  eglDestroyContext(display, bgCtx);
  eglReleaseThread();
}

void Gles3Shaders::PollCompleted() {
  if (completedCount_.load(std::memory_order_acquire) == 0u) return;
  std::vector<CompletedProgram> done;
  {
    std::lock_guard lock(completedMutex_);
    done.swap(completedJobs_);
    completedCount_.store(0u, std::memory_order_relaxed);
  }
  for (const auto& c : done) {
    inFlight_.erase(c.key);
    programs_[c.key] = c.prog;
  }
}

GLuint Gles3Shaders::CompileShader(GLenum type, const std::string& source, const char* label) {
  GLuint sh = glCreateShader(type);
  const char* ptr = source.c_str();
  GLint len = static_cast<GLint>(source.size());
  glShaderSource(sh, 1, &ptr, &len);
  glCompileShader(sh);
  GLint ok = 0;
  glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[2048] = {};
    glGetShaderInfoLog(sh, sizeof(log) - 1, nullptr, log);
    GLESERR("CompileShader(%s) failed: %s", label, log);
    glDeleteShader(sh);
    return 0;
  }
  return sh;
}

GLuint Gles3Shaders::LinkBuiltin(const char* vsSrc, const char* fsSrc, const char* label) {
  GLuint vs = CompileShader(GL_VERTEX_SHADER, vsSrc, label);
  GLuint fs = CompileShader(GL_FRAGMENT_SHADER, fsSrc, label);
  if (!vs || !fs) {
    if (vs) glDeleteShader(vs);
    if (fs) glDeleteShader(fs);
    return 0;
  }
  GLuint prog = glCreateProgram();
  glAttachShader(prog, vs);
  glAttachShader(prog, fs);
  glLinkProgram(prog);
  glDeleteShader(vs);
  glDeleteShader(fs);
  GLint ok = 0;
  glGetProgramiv(prog, GL_LINK_STATUS, &ok);
  if (!ok) {
    char log[2048] = {};
    glGetProgramInfoLog(prog, sizeof(log) - 1, nullptr, log);
    GLESERR("LinkBuiltin(%s) failed: %s", label, log);
    glDeleteProgram(prog);
    return 0;
  }
  return prog;
}

Gles3Shaders::Handle Gles3Shaders::Intern(const uint32_t* words, uint32_t count, bool pixel) {
  if (!words || count == 0) return 0;
  const uint64_t hash = HashWords(words, count, pixel);
  if (auto it = byHash_.find(hash); it != byHash_.end()) {
    return it->second;
  }
  const Handle h = static_cast<Handle>(stages_.size());
  stages_.emplace_back();
  Stage& stage = stages_.back();
  stage.pixel = pixel;
  stage.hash = hash;
  byHash_[hash] = h;

  std::vector<uint32_t> vec(words, words + count);
  XenosGlsl::Translation tr = XenosGlsl::Translate(vec, pixel);
  if (!tr.ok) {
    stage.failed = true;
    GLESERR("XenosGlsl::Translate(%s %016llx) failed: %s",
            pixel ? "PS" : "VS", static_cast<unsigned long long>(hash), tr.problem.c_str());
    return h;
  }

  // Perf diagnostic: count shaders that fell back to the 65536-step interpreter
  // instead of structured GLSL. These are catastrophically slow on Mali if hot.
  {
    static std::atomic<uint32_t> s_interpShaders{0};
    static std::atomic<uint32_t> s_totalShaders{0};
    const uint32_t total = s_totalShaders.fetch_add(1, std::memory_order_relaxed) + 1;
    if (tr.usedInterpreter) {
      const uint32_t n = s_interpShaders.fetch_add(1, std::memory_order_relaxed) + 1;
      if (n <= 16u || (n % 50u) == 0u) {
        GLESLOG("SHADER-PERF-DIAG interpreter-fallback shader %s hash=%016llx (total interp=%u/%u)",
                pixel ? "PS" : "VS", static_cast<unsigned long long>(hash), n, total);
      }
    }
  }
  stage.vertexFetches = std::move(tr.vertexFetches);
  stage.textureFetches = std::move(tr.textureFetches);
  stage.constantMap = std::move(tr.constantMap);
  stage.colourTargets = tr.colourTargets;
  stage.interpolators = tr.interpolators;
  stage.writesDepth = tr.writesDepth;
  stage.usesKill = tr.usesKill;
  stage.glslSource = std::move(tr.glsl);
  return h;
}

bool Gles3Shaders::EnsureStageCompiled(Stage& stage) {
  if (stage.shader) return true;
  if (stage.failed || stage.glslSource.empty()) return false;
  char label[64];
  std::snprintf(label, sizeof(label), "%s_%016llx", stage.pixel ? "PS" : "VS",
                static_cast<unsigned long long>(stage.hash));
  stage.shader =
      CompileShader(stage.pixel ? GL_FRAGMENT_SHADER : GL_VERTEX_SHADER, stage.glslSource, label);
  if (!stage.shader) {
    stage.failed = true;
    return false;
  }
  return true;
}

void Gles3Shaders::PrewarmCache() {
  if (!hasBinaryFormats_ || cacheDir_.empty()) return;
  std::error_code ec;
  const auto verPath = cacheDir_ / "shader_cache.ver";
  const uint64_t curVer = XenosGlsl::Version();
  uint64_t savedVer = 0;
  {
    std::ifstream vif(verPath);
    if (vif) vif >> std::hex >> savedVer;
  }
  if (savedVer != curVer) {
    uint32_t removed = 0;
    for (const auto& entry : std::filesystem::directory_iterator(cacheDir_, ec)) {
      if (!entry.is_regular_file()) continue;
      const std::string name = entry.path().filename().string();
      if (name.size() >= 8 && name.compare(name.size() - 8, 8, ".glesbin") == 0) {
        std::filesystem::remove(entry.path(), ec);
        ++removed;
      }
    }
    std::ofstream vof(verPath, std::ios::trunc);
    if (vof) vof << std::hex << curVer << "\n";
    GLESLOG("Purged %u stale .glesbin shader cache files for version %016llx", removed,
            static_cast<unsigned long long>(curVer));
    return;
  }
  for (const auto& entry : std::filesystem::directory_iterator(cacheDir_, ec)) {
    if (!entry.is_regular_file()) continue;
    const std::string name = entry.path().filename().string();
    if (name.size() < 8 || name.compare(name.size() - 8, 8, ".glesbin") != 0) continue;
    unsigned long long vsH = 0, psH = 0;
    unsigned int rFlag = 0, aFlag = 0;
    if (std::sscanf(name.c_str(), "%016llx_%016llx_%u_%u.glesbin", &vsH, &psH, &rFlag, &aFlag) !=
        4) {
      continue;
    }
    const uint64_t vsHash = static_cast<uint64_t>(vsH);
    const uint64_t psHash = static_cast<uint64_t>(psH);
    const bool rect = (rFlag != 0);
    const bool alpha = (aFlag & 1u) != 0;
    const bool pixelGen = (aFlag & 2u) != 0;
    const bool forceHighp = (aFlag & 4u) != 0;
    HashProgramKey hk{vsHash, psHash, rect, alpha, pixelGen, forceHighp};
    if (prewarmed_.find(hk) != prewarmed_.end()) continue;
    if (GLuint prog = TryLoadProgramBinary(vsHash, psHash, rect, alpha, pixelGen, forceHighp)) {
      prewarmed_[hk] = prog;
    }
  }
}

const Gles3Shaders::Stage* Gles3Shaders::GetStage(Handle h) const {
  if (h == 0 || h >= stages_.size()) return nullptr;
  return &stages_[h];
}

std::filesystem::path Gles3Shaders::BinaryCachePath(uint64_t vsHash, uint64_t psHash, bool rect,
                                                    bool alphaTest, bool pixelGen,
                                                    bool forceHighp) const {
  if (cacheDir_.empty()) return {};
  char name[96];
  const uint32_t aFlag = (alphaTest ? 1u : 0u) | (pixelGen ? 2u : 0u) | (forceHighp ? 4u : 0u);
  std::snprintf(name, sizeof(name), "%016llx_%016llx_%u_%u.glesbin",
                static_cast<unsigned long long>(vsHash), static_cast<unsigned long long>(psHash),
                rect ? 1u : 0u, aFlag);
  return cacheDir_ / name;
}

GLuint Gles3Shaders::TryLoadProgramBinary(uint64_t vsHash, uint64_t psHash, bool rect,
                                          bool alphaTest, bool pixelGen, bool forceHighp) {
  if (!hasBinaryFormats_ || cacheDir_.empty()) return 0;
  const auto path = BinaryCachePath(vsHash, psHash, rect, alphaTest, pixelGen, forceHighp);
  std::ifstream ifs(path, std::ios::binary);
  if (!ifs) return 0;
  struct Header {
    uint32_t magic;
    uint32_t format;
    uint32_t size;
  } hdr{};
  if (!ifs.read(reinterpret_cast<char*>(&hdr), sizeof(hdr))) return 0;
  if (hdr.magic != kBinaryCacheMagic || hdr.size == 0 || hdr.size > 16u * 1024u * 1024u) return 0;
  std::vector<uint8_t> blob(hdr.size);
  if (!ifs.read(reinterpret_cast<char*>(blob.data()), hdr.size)) return 0;

  GLuint prog = glCreateProgram();
  glProgramBinary(prog, static_cast<GLenum>(hdr.format), blob.data(),
                  static_cast<GLsizei>(blob.size()));
  GLint ok = 0;
  glGetProgramiv(prog, GL_LINK_STATUS, &ok);
  if (!ok) {
    glDeleteProgram(prog);
    return 0;
  }
  ++binaryCacheHits_;
  return prog;
}

void Gles3Shaders::SaveProgramBinary(GLuint prog, uint64_t vsHash, uint64_t psHash, bool rect,
                                     bool alphaTest, bool pixelGen, bool forceHighp) {
  if (!hasBinaryFormats_ || cacheDir_.empty() || !prog) return;
  GLint len = 0;
  glGetProgramiv(prog, GL_PROGRAM_BINARY_LENGTH, &len);
  if (len <= 0 || len > 16 * 1024 * 1024) return;
  std::vector<uint8_t> blob(static_cast<size_t>(len));
  GLenum fmt = 0;
  GLsizei actual = 0;
  glGetProgramBinary(prog, len, &actual, &fmt, blob.data());
  if (actual <= 0) return;

  const auto path = BinaryCachePath(vsHash, psHash, rect, alphaTest, pixelGen, forceHighp);
  auto tmp = path;
  tmp += ".tmp";
  std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
  if (!ofs) return;
  struct Header {
    uint32_t magic;
    uint32_t format;
    uint32_t size;
  } hdr{kBinaryCacheMagic, static_cast<uint32_t>(fmt), static_cast<uint32_t>(actual)};
  ofs.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
  ofs.write(reinterpret_cast<const char*>(blob.data()), actual);
  ofs.close();
  if (ofs) {
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
  }
}

GLuint Gles3Shaders::GetLinkedProgram(Handle vsHandle, Handle psHandle, bool rectangles,
                                      bool alphaTest, bool pixelGen, bool forceHighp) {
  if (completedCount_.load(std::memory_order_acquire) != 0u) {
    PollCompleted();
  }
  if (vsHandle == 0 || vsHandle >= stages_.size()) return 0;
  Stage& vs = stages_[vsHandle];
  if (vs.failed) return 0;
  Stage* ps = (psHandle != 0 && psHandle < stages_.size()) ? &stages_[psHandle] : nullptr;
  if (psHandle && (!ps || ps->failed)) return 0;
  const bool useAlpha = (ps != nullptr) && alphaTest;
  const bool usePixelGen = (ps != nullptr) && pixelGen;
  const bool useHighp = (ps != nullptr) && forceHighp && !ps->writesDepth;

  ProgramKey key{vsHandle, psHandle, rectangles, useAlpha, usePixelGen, useHighp};
  if (auto it = programs_.find(key); it != programs_.end()) {
    return it->second;
  }

  const uint64_t psHash = ps ? ps->hash : 0ULL;
  HashProgramKey hk{vs.hash, psHash, rectangles, useAlpha, usePixelGen, useHighp};
  if (auto pit = prewarmed_.find(hk); pit != prewarmed_.end()) {
    const GLuint prog = pit->second;
    prewarmed_.erase(pit);
    programs_[key] = prog;
    return prog;
  }

  // Non-blocking async compile path: queue brand-new shaders to the low-priority background
  // shared-EGLContext threads so the render thread never stalls on disk open() or glCompileShader.
  // (All existing .glesbin binaries on disk were already preloaded into prewarmed_ at startup.)
  if (asyncReady_) {
    if (inFlight_.insert(key).second) {
      CompileJob job;
      job.key = key;
      job.vsHash = vs.hash;
      job.psHash = psHash;
      job.psInterps = ps ? ps->interpolators : 0u;
      job.rect = rectangles;
      job.alphaTest = useAlpha;
      job.pixelGen = usePixelGen;
      job.forceHighp = useHighp;
      job.vsGlsl = vs.glslSource;
      if (ps) job.psGlsl = ps->glslSource;
      {
        std::lock_guard lock(compileMutex_);
        pendingJobs_.push_back(std::move(job));
      }
      compileCv_.notify_one();
    }
    return 0;
  }

  if (!EnsureStageCompiled(vs)) {
    programs_[key] = 0;
    return 0;
  }
  GLuint fsObj = nullFs_;
  if (ps) {
    if (!EnsureStageCompiled(*ps)) {
      programs_[key] = 0;
      return 0;
    }
    const uint32_t vIdx =
        (useAlpha ? 1u : 0u) | (usePixelGen ? 2u : 0u) | (useHighp ? 4u : 0u);
    if (vIdx != 0u) {
      if (!ps->shaderVariants[vIdx] && !ps->glslSource.empty()) {
        std::string src = ps->glslSource;
        const size_t nl = src.find('\n');
        if (nl != std::string::npos) {
          std::string defs;
          if (useAlpha) defs += "#define BS1_ALPHA_TEST 1\n";
          if (usePixelGen) defs += "#define BS1_PIXEL_GEN 1\n";
          if (useHighp) defs += "#define BS1_HIGHP 1\n";
          src.insert(nl + 1, defs);
        }
        char label[64];
        std::snprintf(label, sizeof(label), "PS_%016llx%s%s%s",
                      static_cast<unsigned long long>(ps->hash), useAlpha ? "_AT" : "",
                      usePixelGen ? "_PG" : "", useHighp ? "_HP" : "");
        ps->shaderVariants[vIdx] = CompileShader(GL_FRAGMENT_SHADER, src, label);
      }
      fsObj = ps->shaderVariants[vIdx] ? ps->shaderVariants[vIdx] : ps->shader;
    } else {
      fsObj = ps->shader;
    }
  }

  GLuint prog = glCreateProgram();
  if (hasBinaryFormats_) {
    glProgramParameteri(prog, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
  }
  glAttachShader(prog, vs.shader);
  if (rectangles) {
    const uint32_t gsIdx = std::min(16u, ps ? ps->interpolators : 0u);
    if (rectGs_[gsIdx]) glAttachShader(prog, rectGs_[gsIdx]);
  }
  glAttachShader(prog, fsObj);
  glLinkProgram(prog);
  GLint ok = 0;
  glGetProgramiv(prog, GL_LINK_STATUS, &ok);
  if (!ok) {
    char log[2048] = {};
    glGetProgramInfoLog(prog, sizeof(log) - 1, nullptr, log);
    GLESERR("LinkProgram(vs=%016llx ps=%016llx rect=%d at=%d pg=%d hp=%d) failed: %s",
            static_cast<unsigned long long>(vs.hash), static_cast<unsigned long long>(psHash),
            rectangles ? 1 : 0, useAlpha ? 1 : 0, usePixelGen ? 1 : 0, useHighp ? 1 : 0, log);
    glDeleteProgram(prog);
    programs_[key] = 0;
    return 0;
  }

  SaveProgramBinary(prog, vs.hash, psHash, rectangles, useAlpha, usePixelGen, useHighp);
  programs_[key] = prog;
  return prog;
}

}  // namespace bs1::renderer::gles3
