#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bs1::renderer::XenosGlsl {

struct VertexFetch {
  uint32_t slot = 0;     // Xenos fetch constant slot [0..95]
  uint32_t binding = 0;  // Compact GLES SSBO binding [0..N-1]
  uint32_t stride = 0;   // Stride in dwords from the program's first fetch
};

struct TextureFetch {
  uint32_t slot = 0;       // Xenos fetch constant slot [0..31]
  uint32_t binding = 0;    // Compact GLES texture unit [0..15]
  uint32_t dimension = 1;  // 0=1D, 1=2D, 2=3D, 3=Cube
};

struct Translation {
  bool ok = false;
  std::string glsl;
  std::string problem;
  std::vector<VertexFetch> vertexFetches;
  std::vector<TextureFetch> textureFetches;
  uint32_t interpolators = 0;
  std::vector<uint16_t> constantMap;  // packed c[i] -> guest float4 constant index
  uint32_t colourTargets = 1;         // bitmask of exported oC0..oC3
  bool writesDepth = false;
  bool usesKill = false;
};

/** Translates big-endian-swapped Xenos shader microcode words into GLSL ES 3.20. */
Translation Translate(const std::vector<uint32_t>& words, bool pixel);

/** Some GPUs (e.g. Mali) expose zero vertex SSBOs; use texture buffers there. */
void SetVertexTextureBuffers(bool enabled);
bool VertexTextureBuffers();

/** Version hash for invalidating disk binary shader caches when translator output changes. */
uint64_t Version();

}  // namespace bs1::renderer::XenosGlsl
