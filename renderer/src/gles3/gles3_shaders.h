#pragma once

#include "xenos_glsl.h"

#include <EGL/egl.h>
#include <GLES3/gl32.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace bs1::renderer::gles3 {

/**
 * Compiles Xenos microcode into GLSL ES 3.20 shaders, links (VS, PS, rectGS)
 * programs on demand, and persists Adreno binary programs to disk via
 * GL_OES_get_program_binary / GLES 3.0 core glGetProgramBinary.
 */
class Gles3Shaders {
 public:
  using Handle = uint32_t;

  struct Stage {
    bool pixel = false;
    uint64_t hash = 0;
    GLuint shader = 0;
    GLuint shaderVariants[8] = {};  // bit0=alphaTest, bit1=pixelGen, bit2=forceHighp
    bool failed = false;
    std::string glslSource;
    std::vector<XenosGlsl::VertexFetch> vertexFetches;
    std::vector<XenosGlsl::TextureFetch> textureFetches;
    std::vector<uint16_t> constantMap;
    uint32_t colourTargets = 1;
    uint32_t interpolators = 0;
    bool writesDepth = false;
    bool usesKill = false;
  };

  void Initialize(const std::filesystem::path& cacheDir, EGLDisplay display = EGL_NO_DISPLAY,
                  EGLConfig config = nullptr, EGLContext sharedContext = EGL_NO_CONTEXT,
                  bool noErrorContext = false);
  void Shutdown();
  /** Drains asynchronously compiled programs from the background EGL thread into programs_. */
  void PollCompleted();

  Handle Intern(const uint32_t* words, uint32_t count, bool pixel);
  void Select(Handle h, bool pixel) { current_[pixel ? 1 : 0] = h; }
  Handle Current(bool pixel) const { return current_[pixel ? 1 : 0]; }
  const Stage* GetStage(Handle h) const;

  /** Returns a linked GLES program for (vs, ps, rectangles, alphaTest, pixelGen, forceHighp). ps == 0 uses null depth-only FS. */
  GLuint GetLinkedProgram(Handle vsHandle, Handle psHandle, bool rectangles, bool alphaTest,
                          bool pixelGen, bool forceHighp);

  GLuint PresentLanczosProgram() const { return presentLanczosProg_; }
  GLuint PresentBilinearProgram() const { return presentBilinearProg_; }
  GLuint ResolveCopyProgram() const { return resolveCopyProg_; }

  uint32_t CompiledProgramCount() const { return static_cast<uint32_t>(programs_.size()); }
  uint32_t BinaryCacheHits() const { return binaryCacheHits_; }

 private:
  struct ProgramKey {
    Handle vs = 0;
    Handle ps = 0;
    bool rect = false;
    bool alphaTest = false;
    bool pixelGen = false;
    bool forceHighp = false;
    bool operator==(const ProgramKey& o) const {
      return vs == o.vs && ps == o.ps && rect == o.rect && alphaTest == o.alphaTest &&
             pixelGen == o.pixelGen && forceHighp == o.forceHighp;
    }
  };
  struct ProgramKeyHash {
    size_t operator()(const ProgramKey& k) const {
      return (static_cast<size_t>(k.vs) * 1000003u) ^ (static_cast<size_t>(k.ps) * 9176u) ^
             (k.rect ? 1u : 0u) ^ (k.alphaTest ? 2u : 0u) ^ (k.pixelGen ? 4u : 0u) ^
             (k.forceHighp ? 8u : 0u);
    }
  };

  static uint64_t HashWords(const uint32_t* words, uint32_t count, bool pixel);
  bool EnsureStageCompiled(Stage& stage);
  void PrewarmCache();
  GLuint CompileShader(GLenum type, const std::string& source, const char* label);
  GLuint LinkBuiltin(const char* vsSrc, const char* fsSrc, const char* label);
  std::filesystem::path BinaryCachePath(uint64_t vsHash, uint64_t psHash, bool rect,
                                        bool alphaTest, bool pixelGen, bool forceHighp) const;
  GLuint TryLoadProgramBinary(uint64_t vsHash, uint64_t psHash, bool rect, bool alphaTest,
                              bool pixelGen, bool forceHighp);
  void SaveProgramBinary(GLuint prog, uint64_t vsHash, uint64_t psHash, bool rect, bool alphaTest,
                         bool pixelGen, bool forceHighp);

  struct HashProgramKey {
    uint64_t vsHash = 0;
    uint64_t psHash = 0;
    bool rect = false;
    bool alphaTest = false;
    bool pixelGen = false;
    bool forceHighp = false;
    bool operator==(const HashProgramKey& o) const {
      return vsHash == o.vsHash && psHash == o.psHash && rect == o.rect &&
             alphaTest == o.alphaTest && pixelGen == o.pixelGen && forceHighp == o.forceHighp;
    }
  };
  struct HashProgramKeyHash {
    size_t operator()(const HashProgramKey& k) const {
      return static_cast<size_t>(k.vsHash ^ (k.psHash * 0x9E3779B97F4A7C15ULL)) ^
             (k.rect ? 1u : 0u) ^ (k.alphaTest ? 2u : 0u) ^ (k.pixelGen ? 4u : 0u) ^
             (k.forceHighp ? 8u : 0u);
    }
  };

  void CompileWorkerMain(EGLDisplay display, EGLConfig config, EGLContext sharedContext,
                         bool noErrorContext);

  struct CompileJob {
    ProgramKey key;
    uint64_t vsHash = 0;
    uint64_t psHash = 0;
    uint32_t psInterps = 0;
    bool rect = false;
    bool alphaTest = false;
    bool pixelGen = false;
    bool forceHighp = false;
    std::string vsGlsl;
    std::string psGlsl;
  };
  struct CompletedProgram {
    ProgramKey key;
    GLuint prog = 0;
  };

  std::filesystem::path cacheDir_;
  std::vector<Stage> stages_;
  std::unordered_map<uint64_t, Handle> byHash_;
  std::unordered_map<ProgramKey, GLuint, ProgramKeyHash> programs_;
  std::unordered_map<HashProgramKey, GLuint, HashProgramKeyHash> prewarmed_;
  std::unordered_set<ProgramKey, ProgramKeyHash> inFlight_;
  Handle current_[2] = {0, 0};

  // Shared-EGLContext background compiler pool (2 low-priority threads avoid stalling BS1-GLES3).
  static constexpr uint32_t kNumCompileWorkers = 2;
  std::thread compileThreads_[kNumCompileWorkers];
  std::mutex compileMutex_;
  std::condition_variable compileCv_;
  std::deque<CompileJob> pendingJobs_;
  uint32_t workersFinishedInit_ = 0;
  bool compileStop_ = false;
  bool asyncReady_ = false;
  std::mutex completedMutex_;
  std::vector<CompletedProgram> completedJobs_;
  std::atomic<uint32_t> completedCount_{0};

  GLuint nullFs_ = 0;
  GLuint rectGs_[17] = {};
  GLuint presentLanczosProg_ = 0;
  GLuint presentBilinearProg_ = 0;
  GLuint resolveCopyProg_ = 0;
  uint32_t binaryCacheHits_ = 0;
  bool hasBinaryFormats_ = false;
};

}  // namespace bs1::renderer::gles3
