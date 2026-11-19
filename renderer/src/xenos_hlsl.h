#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bs1::renderer::XenosHlsl {

struct VertexFetch {
  uint32_t slot = 0;    // fetch constant slot [0..95]
  uint32_t stride = 0;  // stride in dwords from the program's first fetch
};

struct TextureFetch {
  uint32_t slot = 0;       // fetch constant slot [0..31] and HLSL t-register
  uint32_t sampler = 0;    // HLSL s-register
  uint32_t dimension = 1;  // 0=1D, 1=2D, 2=3D, 3=Cube
};

struct Translation {
  bool ok = false;
  std::string hlsl;
  std::string problem;
  std::vector<VertexFetch> vertexFetches;
  std::vector<TextureFetch> textureFetches;
  uint32_t interpolators = 0;
  std::vector<uint16_t> constantMap;  // packed c[i] -> guest float4 constant index
  uint32_t colourTargets = 1;         // bitmask of exported oC0..oC3
  bool writesDepth = false;
  bool usesKill = false;
};

/** Translates big-endian-swapped Xenos shader microcode words into HLSL 5.0. */
Translation Translate(const std::vector<uint32_t>& words, bool pixel);

/** Version hash for invalidating disk shader caches when translator output changes. */
uint64_t Version();

/** Whether pixel shader depth is quantized to Xenos 20e4 (24-bit float) precision. */
bool Depth24();

extern const char* const Depth20e4Source;

}  // namespace bs1::renderer::XenosHlsl
