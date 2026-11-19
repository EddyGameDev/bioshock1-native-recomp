#include "render_shaders.h"

#include <Windows.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

#include <rex/logging/log_category.h>

namespace bs1::renderer {

namespace {

using CompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*,
                                   LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);

struct CacheHeader {
  uint32_t magic;
  uint32_t version;
  uint64_t translator;
  uint32_t colourTargets;
  uint32_t writesDepth;
  uint32_t usesKill;
  uint32_t vertexFetches;
  uint32_t textureFetches;
  uint32_t bytecodeBytes;
  uint32_t constantMap;
};
constexpr uint32_t kCacheMagic = 0x42533153u;  // 'BS1S'
constexpr uint32_t kCacheVersion = 2;

}  // namespace

uint64_t RenderShaders::HashBytes(const uint8_t* bytes, size_t count) {
  uint64_t hash = 1469598103934665603ULL;
  for (size_t i = 0; i < count; ++i) {
    hash ^= bytes[i];
    hash *= 1099511628211ULL;
  }
  return hash;
}

RenderShaders::Handle RenderShaders::FindHandle(uint64_t hash) const {
  if (keys_.empty()) return 0;
  const size_t mask = keys_.size() - 1;
  size_t slot = static_cast<size_t>(hash ^ (hash >> 29)) & mask;
  for (;;) {
    if (handles_[slot] == 0) return 0;
    if (keys_[slot] == hash) return handles_[slot];
    slot = (slot + 1) & mask;
  }
}

void RenderShaders::InsertHandle(uint64_t hash, Handle handle) {
  if ((used_ + 1) * 2 > keys_.size()) {
    std::vector<uint64_t> oldKeys = std::move(keys_);
    std::vector<Handle> oldHandles = std::move(handles_);
    const size_t newCap = std::max<size_t>(1024, oldKeys.size() * 2);
    keys_.assign(newCap, 0);
    handles_.assign(newCap, 0);
    used_ = 0;
    for (size_t i = 0; i < oldHandles.size(); ++i) {
      if (oldHandles[i] != 0) InsertHandle(oldKeys[i], oldHandles[i]);
    }
  }
  const size_t mask = keys_.size() - 1;
  size_t slot = static_cast<size_t>(hash ^ (hash >> 29)) & mask;
  while (handles_[slot] != 0) {
    if (keys_[slot] == hash) {
      handles_[slot] = handle;
      return;
    }
    slot = (slot + 1) & mask;
  }
  keys_[slot] = hash;
  handles_[slot] = handle;
  ++used_;
}

void RenderShaders::Initialize(ID3D11Device* device, const std::filesystem::path& cache_dir) {
  Shutdown();
  device_ = device;
  translatorVersion_ = XenosHlsl::Version();
  keys_.assign(1024, 0);
  handles_.assign(1024, 0);
  used_ = 0;
  current_[0] = current_[1] = 0;
  currentHash_[0] = currentHash_[1] = 0;

  HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
  compilerModule_ = compiler;
  if (compiler) {
    compileFn_ = reinterpret_cast<void*>(GetProcAddress(compiler, "D3DCompile"));
  }
  if (!compileFn_) {
    REXGPU_WARN("custom d3d11: d3dcompiler_47.dll unavailable; only cached shaders can load");
  }

  if (!cache_dir.empty()) {
    std::error_code ec;
    cacheDir_ = cache_dir / "custom_shaders";
    std::filesystem::create_directories(cacheDir_, ec);
  } else if (const char* local = std::getenv("LOCALAPPDATA")) {
    std::error_code ec;
    cacheDir_ = std::filesystem::path(local) / "BioShock1Recomp" / "custom_shaders";
    std::filesystem::create_directories(cacheDir_, ec);
  }
}

void RenderShaders::Shutdown() {
  {
    std::lock_guard lock(queueMutex_);
    stopping_ = true;
    queue_.clear();
  }
  queueWake_.notify_all();
  for (std::thread& t : workers_) {
    if (t.joinable()) t.join();
  }
  workers_.clear();
  stopping_ = false;

  for (Program& p : programs_) {
    if (p.shader) {
      p.shader->Release();
      p.shader = nullptr;
    }
    if (p.noDepth) {
      p.noDepth->Release();
      p.noDepth = nullptr;
    }
  }
  programs_.clear();
  microcode_.clear();
  keys_.clear();
  handles_.clear();
  used_ = 0;
  current_[0] = current_[1] = 0;
  currentHash_[0] = currentHash_[1] = 0;

  if (compilerModule_) {
    FreeLibrary(static_cast<HMODULE>(compilerModule_));
    compilerModule_ = nullptr;
  }
  compileFn_ = nullptr;
  device_ = nullptr;
}

std::filesystem::path RenderShaders::CachePath(uint64_t hash, bool pixel) const {
  if (cacheDir_.empty()) return {};
  char name[64];
  std::snprintf(name, sizeof(name), "%016llx.%s.%08llx.bin", static_cast<unsigned long long>(hash),
                pixel ? "ps" : "vs",
                static_cast<unsigned long long>(translatorVersion_ & 0xFFFFFFFFULL));
  return cacheDir_ / name;
}

bool RenderShaders::ReadCache(Program& program) {
  const std::filesystem::path path = CachePath(program.hash, program.pixel);
  if (path.empty()) return false;
  FILE* in = std::fopen(path.string().c_str(), "rb");
  if (!in) return false;
  CacheHeader header{};
  bool ok = std::fread(&header, sizeof(header), 1, in) == 1 && header.magic == kCacheMagic &&
            header.version == kCacheVersion && header.translator == translatorVersion_ &&
            header.vertexFetches <= 96 && header.textureFetches <= 32 &&
            header.constantMap <= 256 && header.bytecodeBytes < (16u << 20);
  if (ok) {
    program.vertexFetches.resize(header.vertexFetches);
    program.textureFetches.resize(header.textureFetches);
    program.constantMap.resize(header.constantMap);
    program.bytecode.resize(header.bytecodeBytes);
    ok = (header.vertexFetches == 0 ||
          std::fread(program.vertexFetches.data(), sizeof(XenosHlsl::VertexFetch),
                     header.vertexFetches, in) == header.vertexFetches) &&
         (header.textureFetches == 0 ||
          std::fread(program.textureFetches.data(), sizeof(XenosHlsl::TextureFetch),
                     header.textureFetches, in) == header.textureFetches) &&
         (header.constantMap == 0 ||
          std::fread(program.constantMap.data(), sizeof(uint16_t), header.constantMap, in) ==
              header.constantMap) &&
         std::fread(program.bytecode.data(), 1, header.bytecodeBytes, in) == header.bytecodeBytes;
    program.colourTargets = header.colourTargets;
    program.writesDepth = header.writesDepth != 0;
    program.usesKill = header.usesKill != 0;
  }
  std::fclose(in);
  if (!ok) {
    program.bytecode.clear();
    return false;
  }
  return true;
}

void RenderShaders::WriteCache(const Program& program) {
  const std::filesystem::path path = CachePath(program.hash, program.pixel);
  if (path.empty()) return;
  const std::filesystem::path partial = path.string() + ".part";
  FILE* out = std::fopen(partial.string().c_str(), "wb");
  if (!out) return;
  CacheHeader header{};
  header.magic = kCacheMagic;
  header.version = kCacheVersion;
  header.translator = translatorVersion_;
  header.colourTargets = program.colourTargets;
  header.writesDepth = program.writesDepth ? 1 : 0;
  header.usesKill = program.usesKill ? 1 : 0;
  header.vertexFetches = static_cast<uint32_t>(program.vertexFetches.size());
  header.textureFetches = static_cast<uint32_t>(program.textureFetches.size());
  header.bytecodeBytes = static_cast<uint32_t>(program.bytecode.size());
  header.constantMap = static_cast<uint32_t>(program.constantMap.size());
  bool written = std::fwrite(&header, sizeof(header), 1, out) == 1;
  if (!program.vertexFetches.empty()) {
    written = written &&
              std::fwrite(program.vertexFetches.data(), sizeof(XenosHlsl::VertexFetch),
                          program.vertexFetches.size(), out) == program.vertexFetches.size();
  }
  if (!program.textureFetches.empty()) {
    written = written &&
              std::fwrite(program.textureFetches.data(), sizeof(XenosHlsl::TextureFetch),
                          program.textureFetches.size(), out) == program.textureFetches.size();
  }
  if (!program.constantMap.empty()) {
    written = written && std::fwrite(program.constantMap.data(), sizeof(uint16_t),
                                     program.constantMap.size(), out) == program.constantMap.size();
  }
  written = written &&
            std::fwrite(program.bytecode.data(), 1, program.bytecode.size(), out) ==
                program.bytecode.size();
  std::fclose(out);
  if (written) {
    std::error_code ec;
    std::filesystem::rename(partial, path, ec);
    if (ec) {
      std::filesystem::remove(path, ec);
      std::filesystem::rename(partial, path, ec);
    }
  } else {
    std::error_code ec;
    std::filesystem::remove(partial, ec);
  }
}

bool RenderShaders::MakeShaderObject(Program& program) {
  if (!device_ || program.bytecode.empty()) return false;
  HRESULT hr = E_FAIL;
  if (program.pixel) {
    ID3D11PixelShader* ps = nullptr;
    hr = device_->CreatePixelShader(program.bytecode.data(), program.bytecode.size(), nullptr, &ps);
    program.shader = ps;
  } else {
    ID3D11VertexShader* vs = nullptr;
    hr = device_->CreateVertexShader(program.bytecode.data(), program.bytecode.size(), nullptr, &vs);
    program.shader = vs;
  }
  return SUCCEEDED(hr) && program.shader != nullptr;
}

void RenderShaders::Build(const Job& job) {
  Program& program = *job.program;
  const char* what = program.pixel ? "pixel program" : "vertex program";
  auto finish = [&](State finalState) {
    {
      std::lock_guard lock(queueMutex_);
      program.state.store(finalState, std::memory_order_release);
    }
    readyWake_.notify_all();
  };
  if (ReadCache(program)) {
    const bool created = MakeShaderObject(program);
    program.bytecode.clear();
    program.bytecode.shrink_to_fit();
    finish(created ? State::Ready : State::Failed);
    return;
  }

  program.state.store(State::Translating, std::memory_order_relaxed);
  const XenosHlsl::Translation translation = XenosHlsl::Translate(*job.words, program.pixel);
  if (!translation.ok) {
    program.problem = translation.problem;
    REXGPU_WARN("custom d3d11: {} {:016X} translate failed: {}", what, program.hash,
                translation.problem);
    finish(State::Failed);
    return;
  }

  program.vertexFetches = translation.vertexFetches;
  program.textureFetches = translation.textureFetches;
  program.colourTargets = translation.colourTargets;
  program.writesDepth = translation.writesDepth;
  program.usesKill = translation.usesKill;
  program.constantMap = translation.constantMap;

  if (!compileFn_) {
    program.problem = "d3dcompiler_47.dll missing";
    finish(State::Failed);
    return;
  }

  program.state.store(State::Compiling, std::memory_order_relaxed);
  ID3DBlob* code = nullptr;
  ID3DBlob* errors = nullptr;
  auto compile = reinterpret_cast<CompileFn>(compileFn_);
  // Use O0 for 5-15x faster FXC compile times so async-compiled scene shaders pop in immediately.
  const UINT compileFlags =
      program.pixel ? D3DCOMPILE_OPTIMIZATION_LEVEL0
                    : (D3DCOMPILE_OPTIMIZATION_LEVEL0 | D3DCOMPILE_IEEE_STRICTNESS);
  const HRESULT hr = compile(translation.hlsl.data(), translation.hlsl.size(), what, nullptr,
                             nullptr, "main", program.pixel ? "ps_5_0" : "vs_5_0", compileFlags, 0,
                             &code, &errors);
  if (FAILED(hr) || !code) {
    const char* err = errors ? static_cast<const char*>(errors->GetBufferPointer()) : "no details";
    REXGPU_WARN("custom d3d11: {} {:016X} D3DCompile failed: {}", what, program.hash, err);
    if (errors) errors->Release();
    if (code) code->Release();
    program.problem = "D3DCompile failed";
    finish(State::Failed);
    return;
  }
  if (errors) errors->Release();

  program.bytecode.assign(static_cast<const uint8_t*>(code->GetBufferPointer()),
                          static_cast<const uint8_t*>(code->GetBufferPointer()) +
                              code->GetBufferSize());
  code->Release();
  WriteCache(program);
  const bool created = MakeShaderObject(program);
  program.bytecode.clear();
  program.bytecode.shrink_to_fit();
  finish(created ? State::Ready : State::Failed);
}

void RenderShaders::BuildNoDepth(const Job& job) {
  Program& program = *job.program;
  if (!compileFn_) {
    program.noDepthState.store(3, std::memory_order_release);
    return;
  }
  const XenosHlsl::Translation translation = XenosHlsl::Translate(*job.words, true);
  if (!translation.ok) {
    program.noDepthState.store(3, std::memory_order_release);
    return;
  }
  const D3D_SHADER_MACRO macros[2] = {{"XENOS_NO_DEPTH", "1"}, {nullptr, nullptr}};
  ID3DBlob* code = nullptr;
  ID3DBlob* errors = nullptr;
  auto compile = reinterpret_cast<CompileFn>(compileFn_);
  const HRESULT hr =
      compile(translation.hlsl.data(), translation.hlsl.size(), "ps_nodepth", macros, nullptr,
              "main", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL1, 0, &code, &errors);
  if (errors) errors->Release();
  if (FAILED(hr) || !code) {
    if (code) code->Release();
    program.noDepthState.store(3, std::memory_order_release);
    return;
  }
  ID3D11PixelShader* ps = nullptr;
  const HRESULT created =
      device_->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &ps);
  code->Release();
  if (FAILED(created) || !ps) {
    program.noDepthState.store(3, std::memory_order_release);
    return;
  }
  program.noDepth = ps;
  program.noDepthState.store(2, std::memory_order_release);
}

void RenderShaders::WorkerLoop() {
  for (;;) {
    Job job{};
    {
      std::unique_lock lock(queueMutex_);
      queueWake_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
      if (stopping_) return;
      job = queue_.front();
      queue_.pop_front();
    }
    if (job.noDepth) {
      BuildNoDepth(job);
    } else {
      Build(job);
    }
  }
}

void RenderShaders::Enqueue(Program* program, const std::vector<uint32_t>* words, bool noDepth) {
  std::lock_guard lock(queueMutex_);
  if (workers_.empty()) {
    const unsigned hw = std::thread::hardware_concurrency();
    const unsigned count = std::max(2u, std::min(6u, hw > 2u ? hw - 2u : 2u));
    for (unsigned i = 0; i < count; ++i) {
      workers_.emplace_back(&RenderShaders::WorkerLoop, this);
    }
  }
  const Job job{program, words, noDepth};
  queue_.push_back(job);
  queueWake_.notify_one();
}

RenderShaders::Handle RenderShaders::Intern(bool pixel, const uint8_t* bytes, uint32_t sizeDwords) {
  const uint64_t hash = HashBytes(bytes, static_cast<size_t>(sizeDwords) * 4);
  if (const Handle found = FindHandle(hash)) return found;

  programs_.emplace_back();
  microcode_.emplace_back();
  const Handle handle = static_cast<Handle>(programs_.size());
  Program& program = programs_.back();
  program.hash = hash;
  program.pixel = pixel;
  std::vector<uint32_t>& words = microcode_.back().words;
  words.resize(sizeDwords);
  for (uint32_t i = 0; i < sizeDwords; ++i) {
    words[i] = (static_cast<uint32_t>(bytes[i * 4]) << 24) |
               (static_cast<uint32_t>(bytes[i * 4 + 1]) << 16) |
               (static_cast<uint32_t>(bytes[i * 4 + 2]) << 8) | bytes[i * 4 + 3];
  }
  InsertHandle(hash, handle);
  // Fast path: load already-cached DXBC synchronously (<0.1ms) without worker wakeup latency.
  if (ReadCache(program)) {
    const bool created = MakeShaderObject(program);
    program.bytecode.clear();
    program.bytecode.shrink_to_fit();
    program.state.store(created ? State::Ready : State::Failed, std::memory_order_release);
    return handle;
  }
  Enqueue(&program, &words, false);
  return handle;
}

RenderShaders::Handle RenderShaders::Loaded(bool pixel, const uint8_t* words, uint32_t sizeDwords) {
  const int stage = pixel ? 1 : 0;
  if (sizeDwords == 0 || sizeDwords > 64 * 1024 || !words) return current_[stage];
  const uint64_t hash = HashBytes(words, static_cast<size_t>(sizeDwords) * 4);
  if (current_[stage] != 0 && currentHash_[stage] == hash) return current_[stage];
  Handle handle = FindHandle(hash);
  if (handle == 0) handle = Intern(pixel, words, sizeDwords);
  current_[stage] = handle;
  currentHash_[stage] = hash;
  return handle;
}

const RenderShaders::Program* RenderShaders::Get(Handle handle) const {
  return (handle != 0 && handle <= programs_.size()) ? &programs_[handle - 1] : nullptr;
}

const RenderShaders::Program* RenderShaders::TryGetReady(Handle handle) const {
  if (handle == 0 || handle > programs_.size()) return nullptr;
  const Program& p = programs_[handle - 1];
  return p.state.load(std::memory_order_acquire) == State::Ready ? &p : nullptr;
}

const RenderShaders::Program* RenderShaders::GetReady(Handle handle) {
  if (handle == 0 || handle > programs_.size()) return nullptr;
  Program& p = programs_[handle - 1];
  State s = p.state.load(std::memory_order_acquire);
  if (s != State::Ready && s != State::Failed) {
    std::unique_lock lock(queueMutex_);
    readyWake_.wait(lock, [&] {
      if (stopping_) return true;
      const State st = p.state.load(std::memory_order_acquire);
      return st == State::Ready || st == State::Failed;
    });
  }
  return &p;
}

ID3D11PixelShader* RenderShaders::NoDepthVariant(Handle handle) {
  if (handle == 0 || handle > programs_.size()) return nullptr;
  Program& program = programs_[handle - 1];
  const uint8_t state = program.noDepthState.load(std::memory_order_acquire);
  if (state == 2) return program.noDepth;
  if (state != 0 || !program.pixel || program.writesDepth) return nullptr;
  program.noDepthState.store(1, std::memory_order_relaxed);
  Enqueue(&program, &microcode_[handle - 1].words, true);
  return nullptr;
}

uint64_t RenderShaders::HashOf(Handle handle) const {
  const Program* p = Get(handle);
  return p ? p->hash : 0;
}

}  // namespace bs1::renderer
