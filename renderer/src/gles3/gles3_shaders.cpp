#include "gles3_shaders.h"

#include <android/log.h>

#include <cstring>
#include <fstream>
#include <sstream>

#define GLESLOG(...) __android_log_print(ANDROID_LOG_INFO, "BS1-GLES3", __VA_ARGS__)
#define GLESERR(...) __android_log_print(ANDROID_LOG_ERROR, "BS1-GLES3", __VA_ARGS__)

namespace bs1::renderer::gles3 {

namespace {

constexpr uint32_t kBinaryCacheMagic = 0x32474253u;  // 'SBG2'

const char* const kNullFsSource = R"GLSL(#version 320 es
precision highp float;
void main() {}
)GLSL";

const char* const kRectangleGsSource = R"GLSL(#version 320 es
precision highp float;
layout(triangles) in;
layout(triangle_strip, max_vertices = 4) out;

layout(location = 0)  in vec4 gi0[3];  layout(location = 1)  in vec4 gi1[3];
layout(location = 2)  in vec4 gi2[3];  layout(location = 3)  in vec4 gi3[3];
layout(location = 4)  in vec4 gi4[3];  layout(location = 5)  in vec4 gi5[3];
layout(location = 6)  in vec4 gi6[3];  layout(location = 7)  in vec4 gi7[3];
layout(location = 8)  in vec4 gi8[3];  layout(location = 9)  in vec4 gi9[3];
layout(location = 10) in vec4 gi10[3]; layout(location = 11) in vec4 gi11[3];
layout(location = 12) in vec4 gi12[3]; layout(location = 13) in vec4 gi13[3];
layout(location = 14) in vec4 gi14[3]; layout(location = 15) in vec4 gi15[3];

layout(location = 0)  out vec4 v_o0;  layout(location = 1)  out vec4 v_o1;
layout(location = 2)  out vec4 v_o2;  layout(location = 3)  out vec4 v_o3;
layout(location = 4)  out vec4 v_o4;  layout(location = 5)  out vec4 v_o5;
layout(location = 6)  out vec4 v_o6;  layout(location = 7)  out vec4 v_o7;
layout(location = 8)  out vec4 v_o8;  layout(location = 9)  out vec4 v_o9;
layout(location = 10) out vec4 v_o10; layout(location = 11) out vec4 v_o11;
layout(location = 12) out vec4 v_o12; layout(location = 13) out vec4 v_o13;
layout(location = 14) out vec4 v_o14; layout(location = 15) out vec4 v_o15;

void emitIdx(int i) {
  gl_Position = gl_in[i].gl_Position;
  v_o0 = gi0[i];   v_o1 = gi1[i];   v_o2 = gi2[i];   v_o3 = gi3[i];
  v_o4 = gi4[i];   v_o5 = gi5[i];   v_o6 = gi6[i];   v_o7 = gi7[i];
  v_o8 = gi8[i];   v_o9 = gi9[i];   v_o10 = gi10[i]; v_o11 = gi11[i];
  v_o12 = gi12[i]; v_o13 = gi13[i]; v_o14 = gi14[i]; v_o15 = gi15[i];
  EmitVertex();
}

void emitFourth(int a, int c, int b) {
  gl_Position = gl_in[a].gl_Position + gl_in[b].gl_Position - gl_in[c].gl_Position;
  v_o0 = gi0[a] + gi0[b] - gi0[c];     v_o1 = gi1[a] + gi1[b] - gi1[c];
  v_o2 = gi2[a] + gi2[b] - gi2[c];     v_o3 = gi3[a] + gi3[b] - gi3[c];
  v_o4 = gi4[a] + gi4[b] - gi4[c];     v_o5 = gi5[a] + gi5[b] - gi5[c];
  v_o6 = gi6[a] + gi6[b] - gi6[c];     v_o7 = gi7[a] + gi7[b] - gi7[c];
  v_o8 = gi8[a] + gi8[b] - gi8[c];     v_o9 = gi9[a] + gi9[b] - gi9[c];
  v_o10 = gi10[a] + gi10[b] - gi10[c]; v_o11 = gi11[a] + gi11[b] - gi11[c];
  v_o12 = gi12[a] + gi12[b] - gi12[c]; v_o13 = gi13[a] + gi13[b] - gi13[c];
  v_o14 = gi14[a] + gi14[b] - gi14[c]; v_o15 = gi15[a] + gi15[b] - gi15[c];
  EmitVertex();
}

void main() {
  vec2 p0 = gl_in[0].gl_Position.xy / gl_in[0].gl_Position.w;
  vec2 p1 = gl_in[1].gl_Position.xy / gl_in[1].gl_Position.w;
  vec2 p2 = gl_in[2].gl_Position.xy / gl_in[2].gl_Position.w;
  float d01 = dot(p1 - p0, p1 - p0);
  float d12 = dot(p2 - p1, p2 - p1);
  float d02 = dot(p2 - p0, p2 - p0);
  int c = (d02 >= d01 && d02 >= d12) ? 1 : (d01 >= d12 ? 2 : 0);
  int a = (c + 1) % 3;
  int b = (c + 2) % 3;
  emitIdx(c);
  emitIdx(a);
  emitIdx(b);
  emitFourth(a, c, b);
  EndPrimitive();
}
)GLSL";

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
  vec4 u_params;   // x = useGamma, y = colorScale, z = clampMode, w = fromDepth
};
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 frag_color;

vec3 applyGamma(vec3 c) {
  if (u_params.x < 0.5) return c;
  c = clamp(c, 0.0, 1.0);
  float r = textureLod(u_gamma, vec2(c.r, 0.5), 0.0).r;
  float g = textureLod(u_gamma, vec2(c.g, 0.5), 0.0).g;
  float b = textureLod(u_gamma, vec2(c.b, 0.5), 0.0).b;
  return vec3(r, g, b);
}

void main() {
  vec2 uv = u_uvRect.xy + v_uv * u_uvRect.zw;
  vec3 c = textureLod(u_src, uv, 0.0).rgb;
  frag_color = vec4(applyGamma(c), 1.0);
}
)GLSL";

const char* const kPresentLanczosFsSource = R"GLSL(#version 320 es
precision highp float;
layout(binding = 0) uniform highp sampler2D u_src;
layout(binding = 1) uniform highp sampler2D u_gamma;
layout(std140, binding = 4) uniform PresentParams {
  vec4 u_uvRect;   // xy = offset, zw = size
  vec4 u_params;   // x = useGamma, y = srcW, z = srcH, w = unused
};
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 frag_color;

vec4 LanczosWeights(float f) {
  f = clamp(f, 1e-4, 1.0 - 1e-4);
  float sf = sin(3.14159265 * f);
  float sh = sin(1.57079633 * f);
  float ch = cos(1.57079633 * f);
  float a0 = f + 1.0;
  float a1 = f;
  float a2 = 1.0 - f;
  float a3 = 2.0 - f;
  return vec4(-sf * ch / (a0 * a0), sf * sh / (a1 * a1),
              sf * ch / (a2 * a2), -sf * sh / (a3 * a3));
}

vec3 applyGamma(vec3 c) {
  if (u_params.x < 0.5) return c;
  c = clamp(c, 0.0, 1.0);
  float r = textureLod(u_gamma, vec2(c.r, 0.5), 0.0).r;
  float g = textureLod(u_gamma, vec2(c.g, 0.5), 0.0).g;
  float b = textureLod(u_gamma, vec2(c.b, 0.5), 0.0).b;
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

  vec3 acc = vec3(0.0);
  acc += textureLod(u_src, vec2(t0.x,  t0.y),  0.0).rgb * (w0.x  * w0.y);
  acc += textureLod(u_src, vec2(t12.x, t0.y),  0.0).rgb * (w12.x * w0.y);
  acc += textureLod(u_src, vec2(t3.x,  t0.y),  0.0).rgb * (w3.x  * w0.y);
  acc += textureLod(u_src, vec2(t0.x,  t12.y), 0.0).rgb * (w0.x  * w12.y);
  acc += textureLod(u_src, vec2(t12.x, t12.y), 0.0).rgb * (w12.x * w12.y);
  acc += textureLod(u_src, vec2(t3.x,  t12.y), 0.0).rgb * (w3.x  * w12.y);
  acc += textureLod(u_src, vec2(t0.x,  t3.y),  0.0).rgb * (w0.x  * w3.y);
  acc += textureLod(u_src, vec2(t12.x, t3.y),  0.0).rgb * (w12.x * w3.y);
  acc += textureLod(u_src, vec2(t3.x,  t3.y),  0.0).rgb * (w3.x  * w3.y);

  vec2 sum = w0 + w12 + w3;
  vec3 c = clamp(acc / (sum.x * sum.y), 0.0, 1.0);
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

void Gles3Shaders::Initialize(const std::filesystem::path& cacheDir) {
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
  rectGs_ = CompileShader(GL_GEOMETRY_SHADER, kRectangleGsSource, "rectGS");
  presentLanczosProg_ = LinkBuiltin(kPresentVsSource, kPresentLanczosFsSource, "presentLanczos");
  presentBilinearProg_ = LinkBuiltin(kPresentVsSource, kPresentBilinearFsSource, "presentBilinear");
  resolveCopyProg_ = LinkBuiltin(kResolveVsSource, kResolveCopyFsSource, "resolveCopy");
  PrewarmCache();

  GLESLOG("Gles3Shaders initialized (binaryFormats=%d, prewarmed=%zu, rectGS=%u, lanczos=%u, resolve=%u)",
          numFormats, prewarmed_.size(), rectGs_, presentLanczosProg_, resolveCopyProg_);
}

void Gles3Shaders::Shutdown() {
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
    if (s.shaderAlphaTest) glDeleteShader(s.shaderAlphaTest);
  }
  stages_.clear();
  byHash_.clear();
  if (nullFs_) { glDeleteShader(nullFs_); nullFs_ = 0; }
  if (rectGs_) { glDeleteShader(rectGs_); rectGs_ = 0; }
  if (presentLanczosProg_) { glDeleteProgram(presentLanczosProg_); presentLanczosProg_ = 0; }
  if (presentBilinearProg_) { glDeleteProgram(presentBilinearProg_); presentBilinearProg_ = 0; }
  if (resolveCopyProg_) { glDeleteProgram(resolveCopyProg_); resolveCopyProg_ = 0; }
  current_[0] = current_[1] = 0;
  binaryCacheHits_ = 0;
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

  stage.vertexFetches = std::move(tr.vertexFetches);
  stage.textureFetches = std::move(tr.textureFetches);
  stage.constantMap = std::move(tr.constantMap);
  stage.colourTargets = tr.colourTargets;
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
    const bool alpha = (aFlag != 0);
    HashProgramKey hk{vsHash, psHash, rect, alpha};
    if (prewarmed_.find(hk) != prewarmed_.end()) continue;
    if (GLuint prog = TryLoadProgramBinary(vsHash, psHash, rect, alpha)) {
      prewarmed_[hk] = prog;
    }
  }
}

const Gles3Shaders::Stage* Gles3Shaders::GetStage(Handle h) const {
  if (h == 0 || h >= stages_.size()) return nullptr;
  return &stages_[h];
}

std::filesystem::path Gles3Shaders::BinaryCachePath(uint64_t vsHash, uint64_t psHash, bool rect,
                                                    bool alphaTest) const {
  if (cacheDir_.empty()) return {};
  char name[96];
  std::snprintf(name, sizeof(name), "%016llx_%016llx_%u_%u.glesbin",
                static_cast<unsigned long long>(vsHash), static_cast<unsigned long long>(psHash),
                rect ? 1u : 0u, alphaTest ? 1u : 0u);
  return cacheDir_ / name;
}

GLuint Gles3Shaders::TryLoadProgramBinary(uint64_t vsHash, uint64_t psHash, bool rect,
                                          bool alphaTest) {
  if (!hasBinaryFormats_ || cacheDir_.empty()) return 0;
  const auto path = BinaryCachePath(vsHash, psHash, rect, alphaTest);
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
                                     bool alphaTest) {
  if (!hasBinaryFormats_ || cacheDir_.empty() || !prog) return;
  GLint len = 0;
  glGetProgramiv(prog, GL_PROGRAM_BINARY_LENGTH, &len);
  if (len <= 0 || len > 16 * 1024 * 1024) return;
  std::vector<uint8_t> blob(static_cast<size_t>(len));
  GLenum fmt = 0;
  GLsizei actual = 0;
  glGetProgramBinary(prog, len, &actual, &fmt, blob.data());
  if (actual <= 0) return;

  const auto path = BinaryCachePath(vsHash, psHash, rect, alphaTest);
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
                                      bool alphaTest) {
  if (vsHandle == 0 || vsHandle >= stages_.size()) return 0;
  Stage& vs = stages_[vsHandle];
  if (vs.failed) return 0;
  Stage* ps = (psHandle != 0 && psHandle < stages_.size()) ? &stages_[psHandle] : nullptr;
  if (psHandle && (!ps || ps->failed)) return 0;
  const bool useAlpha = (ps != nullptr) && alphaTest;

  ProgramKey key{vsHandle, psHandle, rectangles, useAlpha};
  if (auto it = programs_.find(key); it != programs_.end()) {
    return it->second;
  }

  const uint64_t psHash = ps ? ps->hash : 0ULL;
  HashProgramKey hk{vs.hash, psHash, rectangles, useAlpha};
  if (auto pit = prewarmed_.find(hk); pit != prewarmed_.end()) {
    const GLuint prog = pit->second;
    prewarmed_.erase(pit);
    programs_[key] = prog;
    return prog;
  }
  if (GLuint cached = TryLoadProgramBinary(vs.hash, psHash, rectangles, useAlpha)) {
    programs_[key] = cached;
    return cached;
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
    if (useAlpha) {
      if (!ps->shaderAlphaTest && !ps->glslSource.empty()) {
        std::string src = ps->glslSource;
        const size_t nl = src.find('\n');
        if (nl != std::string::npos) {
          src.insert(nl + 1, "#define BS1_ALPHA_TEST 1\n");
        }
        char label[64];
        std::snprintf(label, sizeof(label), "PS_%016llx_AT",
                      static_cast<unsigned long long>(ps->hash));
        ps->shaderAlphaTest = CompileShader(GL_FRAGMENT_SHADER, src, label);
      }
      fsObj = ps->shaderAlphaTest ? ps->shaderAlphaTest : ps->shader;
    } else {
      fsObj = ps->shader;
    }
  }

  GLuint prog = glCreateProgram();
  if (hasBinaryFormats_) {
    glProgramParameteri(prog, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
  }
  glAttachShader(prog, vs.shader);
  if (rectangles && rectGs_) {
    glAttachShader(prog, rectGs_);
  }
  glAttachShader(prog, fsObj);
  glLinkProgram(prog);
  GLint ok = 0;
  glGetProgramiv(prog, GL_LINK_STATUS, &ok);
  if (!ok) {
    char log[2048] = {};
    glGetProgramInfoLog(prog, sizeof(log) - 1, nullptr, log);
    GLESERR("LinkProgram(vs=%016llx ps=%016llx rect=%d at=%d) failed: %s",
            static_cast<unsigned long long>(vs.hash), static_cast<unsigned long long>(psHash),
            rectangles ? 1 : 0, useAlpha ? 1 : 0, log);
    glDeleteProgram(prog);
    programs_[key] = 0;
    return 0;
  }

  SaveProgramBinary(prog, vs.hash, psHash, rectangles, useAlpha);
  programs_[key] = prog;
  return prog;
}

}  // namespace bs1::renderer::gles3
