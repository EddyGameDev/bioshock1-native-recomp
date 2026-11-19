#pragma once

#include "render_state.h"
#include "xenos_hlsl.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct ID3D11Device;
struct ID3D11DeviceChild;
struct ID3D11PixelShader;

namespace bs1::renderer {

/**
 * Translates Xenos microcode into HLSL 5.0, compiles DXBC via d3dcompiler_47.dll,
 * and caches both in memory and on disk.
 */
class RenderShaders {
 public:
  using Handle = RenderState::Handle;

  enum class State : uint8_t { New, Translating, Compiling, Ready, Failed };

  struct Program {
    uint64_t hash = 0;
    bool pixel = false;
    std::atomic<State> state{State::New};
    std::string problem;
    ID3D11DeviceChild* shader = nullptr;
    std::vector<XenosHlsl::VertexFetch> vertexFetches;
    std::vector<XenosHlsl::TextureFetch> textureFetches;
    uint32_t colourTargets = 1;
    std::vector<uint16_t> constantMap;
    bool writesDepth = false;
    bool usesKill = false;
    std::vector<uint8_t> bytecode;
    std::atomic<uint8_t> noDepthState{0};  // 0=none, 1=building, 2=ready, 3=failed
    ID3D11PixelShader* noDepth = nullptr;
  };

  void Initialize(ID3D11Device* device, const std::filesystem::path& cache_dir);
  void Shutdown();

  /** Registers big-endian microcode words and selects the stage's active handle. */
  Handle Loaded(bool pixel, const uint8_t* words, uint32_t sizeDwords);
  Handle Current(bool pixel) const { return current_[pixel ? 1 : 0]; }

  const Program* Get(Handle handle) const;
  /** Non-blocking check: returns the program only if already compiled & ready, else nullptr. */
  const Program* TryGetReady(Handle handle) const;
  /** Blocks until the program finishes compiling (if still pending), then returns it. */
  const Program* GetReady(Handle handle);
  ID3D11PixelShader* NoDepthVariant(Handle handle);
  uint64_t HashOf(Handle handle) const;

 private:
  struct Words {
    std::vector<uint32_t> words;
  };
  struct Job {
    Program* program = nullptr;
    const std::vector<uint32_t>* words = nullptr;
    bool noDepth = false;
  };

  static uint64_t HashBytes(const uint8_t* bytes, size_t count);
  Handle FindHandle(uint64_t hash) const;
  void InsertHandle(uint64_t hash, Handle handle);
  Handle Intern(bool pixel, const uint8_t* bytes, uint32_t sizeDwords);

  std::filesystem::path CachePath(uint64_t hash, bool pixel) const;
  bool ReadCache(Program& program);
  void WriteCache(const Program& program);
  bool MakeShaderObject(Program& program);
  void Build(const Job& job);
  void BuildNoDepth(const Job& job);
  void Enqueue(Program* program, const std::vector<uint32_t>* words, bool noDepth);
  void WorkerLoop();

  ID3D11Device* device_ = nullptr;
  void* compilerModule_ = nullptr;
  void* compileFn_ = nullptr;
  std::filesystem::path cacheDir_;
  uint64_t translatorVersion_ = 0;

  std::deque<Program> programs_;
  std::deque<Words> microcode_;
  std::vector<uint64_t> keys_;
  std::vector<Handle> handles_;
  uint32_t used_ = 0;

  Handle current_[2] = {0, 0};
  uint64_t currentHash_[2] = {0, 0};

  std::mutex queueMutex_;
  std::condition_variable queueWake_;
  std::condition_variable readyWake_;
  std::deque<Job> queue_;
  std::vector<std::thread> workers_;
  bool stopping_ = false;
};

}  // namespace bs1::renderer
