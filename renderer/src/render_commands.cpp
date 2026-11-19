#include "render_commands.h"

#include "pm4_processor.h"
#include "render_pipeline.h"
#include "render_resources.h"
#include "render_shaders.h"
#include "xenos_hlsl.h"

#include <d3d11_1.h>
#include <rex/logging/log_category.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace bs1::renderer {

namespace {

inline uint16_t ByteSwap16(uint16_t v) { return static_cast<uint16_t>((v >> 8) | (v << 8)); }

inline uint32_t ByteSwap32(uint32_t v) {
  return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
}

void Swap16Into(uint16_t* dst, const uint16_t* src, uint32_t count) {
  for (uint32_t i = 0; i < count; ++i) dst[i] = ByteSwap16(src[i]);
}

void Swap32IntoMasked(uint32_t* dst, const uint32_t* src, uint32_t count) {
  for (uint32_t i = 0; i < count; ++i) dst[i] = ByteSwap32(src[i]) & 0xFFFFFFu;
}

bool DepthOnlyMode(const RenderState::Snapshot& state) {
  return (state.modeControl & 7u) == 5u || (state.colorMask & 0xFFFFu) == 0;
}

bool DepthUnseen(uint32_t depthControl) {
  // Depth test disabled, OR (compare == always (7) and no depth write).
  if ((depthControl & 2) == 0) return true;
  const bool writes = (depthControl & 4) != 0;
  const uint32_t func = (depthControl >> 4) & 7;
  return !writes && func == 7;
}

}  // namespace

void RenderCommands::Initialize(RenderResources* resources, RenderPipeline* pipeline,
                                RenderShaders* shaders) {
  resources_ = resources;
  pipeline_ = pipeline;
  shaders_ = shaders;
  Reset();
}

void RenderCommands::Reset() {
  std::memset(&constants_, 0, sizeof(constants_));
  std::memset(&lastConstants_, 0, sizeof(lastConstants_));
  uploaded_ = UploadedConstants();
  pendingFirst_[0] = pendingFirst_[1] = 0;
  pendingEnd_[0] = pendingEnd_[1] = 256;
  std::memset(floatFileFallback_, 0, sizeof(floatFileFallback_));
  std::memset(bools_, 0, sizeof(bools_));
  boolsUploaded_ = false;
  observedMainHeight_ = 360;
}

void RenderCommands::ObserveSwapHeight(uint32_t width, uint32_t height) {
  // Ignore degenerate swaps (e.g. 1x1 dummy clears), capture baseline full-frame height.
  if (width >= 480 && height >= 240 && height <= 1080) {
    observedMainHeight_ = height;
  }
}

bool RenderCommands::ResolvePrograms(RenderShaders* shaders, bool allowAsync,
                                     RenderState::DrawCommand& out, const void*& vsOut,
                                     const void*& psOut) {
  out.vertexShader = shaders->Current(false);
  out.pixelShader = shaders->Current(true);
  const RenderShaders::Program* vs =
      allowAsync ? shaders->TryGetReady(out.vertexShader) : shaders->GetReady(out.vertexShader);
  const RenderShaders::Program* ps =
      allowAsync ? shaders->TryGetReady(out.pixelShader) : shaders->GetReady(out.pixelShader);
  if (!vs || vs->state.load() != RenderShaders::State::Ready || !vs->shader) return false;
  if (!ps || ps->state.load() != RenderShaders::State::Ready || !ps->shader) return false;
  out.vs = reinterpret_cast<ID3D11VertexShader*>(vs->shader);
  out.ps = reinterpret_cast<ID3D11PixelShader*>(ps->shader);
  vsOut = vs;
  psOut = ps;
  return true;
}

bool RenderCommands::SetupTargets(const RenderState::Snapshot& state, uint32_t psColourTargets,
                                  RenderState::DrawCommand& out, float& scaleXOut,
                                  float& scaleYOut) {
  const uint32_t tl = state.scissorTopLeft;
  const uint32_t br = state.scissorBottomRight;
  const bool ignoreWindowOffset = (tl & 0x80000000u) != 0;
  const int32_t offsetX =
      ignoreWindowOffset ? 0 : (static_cast<int32_t>(state.windowOffset << 17) >> 17);
  const int32_t offsetY =
      ignoreWindowOffset ? 0 : (static_cast<int32_t>(state.windowOffset << 1) >> 17);
  const int32_t bottom = static_cast<int32_t>((br >> 16) & 0x7FFFu) + offsetY;
  if (bottom > 0 && resources_->EnsureRows(state.surfaceInfo, static_cast<uint32_t>(bottom))) {
    out.bindingsLost = true;
  }

  const bool depthOnly = DepthOnlyMode(state);
  out.targetCount = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    out.targets[i] = nullptr;
    out.targetResources[i] = nullptr;
  }
  out.depthView = nullptr;
  out.depthResource = nullptr;

  const bool isRectClear =
      (state.primitive == RenderState::Rectangles && state.indexCount <= 6);
  const uint32_t cls = ((state.surfaceInfo >> 16) & 3u) != 0 ? 1u : 0u;
  const RenderResources::ColorTarget* primaryColor = nullptr;
  for (uint32_t i = 0; i < 4 && !depthOnly; ++i) {
    if (((psColourTargets >> i) & 1u) == 0) continue;
    if (((state.colorMask >> (i * 4)) & 0xFu) == 0) continue;
    RenderResources::Handle ch =
        isRectClear ? resources_->PrimaryColorTargetAt(state.colorInfo[i], state.pitch, cls) : 0;
    if (ch == 0) ch = resources_->ColorTargetFor(state.colorInfo[i], state.surfaceInfo);
    const RenderResources::ColorTarget* target = resources_->ColorTargetOf(ch);
    if (!target || !target->view) continue;
    bool duplicate = false;
    for (uint32_t j = 0; j < i; ++j) {
      if (out.targets[j] == target->view) {
        duplicate = true;
        break;
      }
    }
    if (duplicate) continue;
    out.targets[i] = target->view;
    out.targetResources[i] = target->resource;
    out.targetCount = i + 1;
    if (!primaryColor) primaryColor = target;
  }

  const RenderResources::DepthTarget* boundDepth = nullptr;
  if ((state.depthControl & 3u) != 0) {
    RenderResources::Handle dh =
        isRectClear ? resources_->PrimaryDepthTargetAt(state.depthInfo, state.pitch, cls) : 0;
    if (dh == 0) dh = resources_->DepthTargetFor(state.depthInfo, state.surfaceInfo);
    const RenderResources::DepthTarget* depth = resources_->DepthTargetOf(dh);
    if (depth && depth->view) {
      if (!primaryColor ||
          (depth->width == primaryColor->width && depth->height == primaryColor->height)) {
        out.depthView = depth->view;
        out.depthResource = depth->resource;
        boundDepth = depth;
      }
    }
  }

  if (out.targetCount == 0 && out.depthView == nullptr) {
    return false;
  }

  uint32_t boundPitch = 0;
  uint32_t boundRows = 0;
  uint32_t boundMsaa = 0;
  if (primaryColor && primaryColor->pitch > 0 && primaryColor->rows > 0) {
    out.targetWidth = primaryColor->width;
    out.targetHeight = primaryColor->height;
    boundPitch = primaryColor->pitch;
    boundRows = primaryColor->rows;
    boundMsaa = primaryColor->msaa;
  } else if (boundDepth && boundDepth->pitch > 0 && boundDepth->rows > 0) {
    out.targetWidth = boundDepth->width;
    out.targetHeight = boundDepth->height;
    boundPitch = boundDepth->pitch;
    boundRows = boundDepth->rows;
    boundMsaa = boundDepth->msaa;
  } else {
    resources_->TargetSize(state.surfaceInfo, 4, out.targetWidth, out.targetHeight, scaleXOut,
                           scaleYOut);
  }
  if (boundPitch > 0 && boundRows > 0 && state.pitch > 0) {
    if (boundPitch != state.pitch) {
      const uint32_t targetSx = (boundMsaa == 2u) ? 2u : 1u;
      const uint32_t targetSy = (boundMsaa >= 1u) ? 2u : 1u;
      const uint32_t clearMsaa = (state.surfaceInfo >> 16) & 3u;
      const uint32_t clearSx = (clearMsaa == 2u) ? 2u : 1u;
      const uint32_t clearSy = (clearMsaa >= 1u) ? 2u : 1u;
      const uint64_t num =
          static_cast<uint64_t>(boundRows) * boundPitch * targetSx * targetSy;
      const uint64_t den = static_cast<uint64_t>(state.pitch) * clearSx * clearSy;
      const uint32_t clearH = std::max<uint32_t>(
          1u, static_cast<uint32_t>((num + den / 2u) / std::max<uint64_t>(1u, den)));
      scaleXOut = static_cast<float>(out.targetWidth) / static_cast<float>(state.pitch);
      scaleYOut = static_cast<float>(out.targetHeight) / static_cast<float>(clearH);
    } else {
      scaleXOut = static_cast<float>(out.targetWidth) / static_cast<float>(boundPitch);
      scaleYOut = static_cast<float>(out.targetHeight) / static_cast<float>(boundRows);
    }
  }
  if (out.targetWidth == 0 || out.targetHeight == 0) return false;

  out.scissor[0] = std::max(
      0, static_cast<int32_t>(
             std::lround((static_cast<int32_t>(tl & 0x7FFFu) + offsetX) * scaleXOut)));
  out.scissor[1] = std::max(
      0, static_cast<int32_t>(
             std::lround((static_cast<int32_t>((tl >> 16) & 0x7FFFu) + offsetY) * scaleYOut)));
  out.scissor[2] = std::min(
      static_cast<int32_t>(out.targetWidth),
      static_cast<int32_t>(
          std::lround((static_cast<int32_t>(br & 0x7FFFu) + offsetX) * scaleXOut)));
  out.scissor[3] = std::min(
      static_cast<int32_t>(out.targetHeight),
      static_cast<int32_t>(
          std::lround((static_cast<int32_t>((br >> 16) & 0x7FFFu) + offsetY) * scaleYOut)));
  return out.scissor[2] > out.scissor[0] && out.scissor[3] > out.scissor[1];
}

void RenderCommands::SetupTextures(const Pm4Processor& pm4, const void* vsPtr, const void* psPtr,
                                   RenderState::DrawCommand& out,
                                   RenderState::DrawConstants& constants) {
  const auto* vs = static_cast<const RenderShaders::Program*>(vsPtr);
  const auto* ps = static_cast<const RenderShaders::Program*>(psPtr);
  out.textureCount = 0;

  for (uint8_t stage = 0; stage < 2; ++stage) {
    const RenderShaders::Program* program = stage == 0 ? ps : vs;
    if (!program) continue;
    for (const XenosHlsl::TextureFetch& fetch : program->textureFetches) {
      if (out.textureCount >= RenderState::DrawCommand::MaxTextures) return;
      uint32_t words[6] = {};
      RenderState::ReadTextureFetch(pm4, fetch.slot, words);

      uint32_t width = 1;
      uint32_t height = 1;
      bool resolved = false;
      const RenderState::Handle tex = resources_->TextureFor(words, width, height, resolved);

      RenderState::DrawCommand::Texture& binding = out.textures[out.textureCount++];
      binding.stage = stage;
      binding.slot = static_cast<uint8_t>(fetch.slot);
      binding.samplerSlot = static_cast<uint8_t>(fetch.sampler);
      const RenderResources::Resolved* resolvedSurface =
          resolved ? resources_->ResolvedAt(words[1] & 0xFFFFF000u) : nullptr;
      if (resolvedSurface) {
        binding.view = resolvedSurface->resource;
      } else {
        binding.view = resources_->TextureView(tex, fetch.dimension == 3, false);
      }
      binding.sampler = pipeline_->SamplerObject(pipeline_->Sampler(words));

      if (fetch.slot < 32) {
        constants.textureSize[fetch.slot][0] = static_cast<float>(width);
        constants.textureSize[fetch.slot][1] = static_cast<float>(height);
        constants.textureSize[fetch.slot][2] = width > 0 ? 1.0f / static_cast<float>(width) : 1.0f;
        constants.textureSize[fetch.slot][3] =
            height > 0 ? 1.0f / static_cast<float>(height) : 1.0f;

        uint32_t swizzle = (words[3] >> 1) & 0xFFFu;
        const uint32_t origSwizzle = swizzle;
        // Host EDRAM resolves keep (R,G,B,A) without applying RB_COPY_DEST_INFO.copy_dest_swap
        // in memory; undo the hardware R<->B swap in the consumer's fetch swizzle.
        if (resolvedSurface && resolvedSurface->rbSwapped) {
          uint32_t unswapped = 0;
          for (uint32_t i = 0; i < 4; ++i) {
            uint32_t sel = (swizzle >> (i * 3)) & 7u;
            if (sel == 0u) sel = 2u;
            else if (sel == 2u) sel = 0u;
            unswapped |= (sel << (i * 3));
          }
          swizzle = unswapped;
        }
        // Xenos 1-channel (RRRR) and 2-channel (RGGG) formats replicate channels in hardware,
        // whereas D3D11 R* / RG* SRVs return 0.0 / 1.0 in missing lanes.
        const uint32_t texFmt = words[1] & 0x3Fu;
        const bool repRRRR = (texFmt == 2u || texFmt == 8u || texFmt == 9u || texFmt == 22u ||
                              texFmt == 23u || texFmt == 24u || texFmt == 27u || texFmt == 30u ||
                              texFmt == 36u || texFmt == 58u || texFmt == 59u);
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
        constants.textureAdjustment[fetch.slot][0] = swizzle;
        // Xenos TextureSign applies per source component before swizzle; permute to match output.
        const uint32_t rawSigns = (words[0] >> 2) & 0xFFu;
        uint32_t permutedSigns = 0;
        for (uint32_t i = 0; i < 4; ++i) {
          const uint32_t origSel = (origSwizzle >> (i * 3)) & 7u;
          const uint32_t compSign = origSel < 4u ? ((rawSigns >> (origSel * 2)) & 3u) : 0u;
          permutedSigns |= (compSign << (i * 2));
        }
        constants.textureAdjustment[fetch.slot][1] = permutedSigns;
        int32_t expAdj = static_cast<int32_t>((words[3] >> 13) & 0x3Fu);
        if (expAdj >= 32) expAdj -= 64;
        const float expScale = std::ldexp(1.0f, expAdj);
        uint32_t expBits = 0;
        std::memcpy(&expBits, &expScale, sizeof(expBits));
        constants.textureAdjustment[fetch.slot][2] = expBits ^ 0x3F800000u;
      }
    }
  }
}

bool RenderCommands::SetupBuffers(const Pm4Processor& pm4, const void* vsPtr,
                                  RenderState::DrawCommand& out) {
  const auto* vs = static_cast<const RenderShaders::Program*>(vsPtr);
  out.bufferCount = 0;
  for (const XenosHlsl::VertexFetch& fetch : vs->vertexFetches) {
    uint32_t word0 = 0;
    uint32_t word1 = 0;
    RenderState::ReadVertexFetch(pm4, fetch.slot, word0, word1);
    const uint32_t bytes = ((word1 >> 2) & 0xFFFFFFu) * 4u;
    const RenderState::Handle buf = resources_->VertexBufferFor(word0 & ~3u, bytes);
    ID3D11ShaderResourceView* view = resources_->VertexBufferView(buf);
    if (!view || out.bufferCount >= RenderState::DrawCommand::MaxBuffers) return false;
    RenderState::DrawCommand::Buffer& binding = out.buffers[out.bufferCount++];
    binding.slot = static_cast<uint8_t>(fetch.slot);
    binding.view = view;
  }
  return true;
}

bool RenderCommands::SetupConstants(Pm4Processor& pm4, const void* vsPtr, const void* psPtr,
                                    RenderState::DrawCommand& out,
                                    const RenderState::DrawConstants& constants) {
  const auto* vs = static_cast<const RenderShaders::Program*>(vsPtr);
  const auto* ps = static_cast<const RenderShaders::Program*>(psPtr);
  const uint32_t* file = pm4.Registers();

  for (uint32_t range = 0; range < 2; ++range) {
    uint32_t spanFirst = 0;
    uint32_t spanEnd = 0;
    if (!pm4.TakeConstantSpan(range, spanFirst, spanEnd)) continue;
    pendingFirst_[range] = std::min(pendingFirst_[range], spanFirst / 4u);
    pendingEnd_[range] = std::max(pendingEnd_[range], (spanEnd + 3u) / 4u);
  }

  {
    uint32_t spanFirst = 0;
    uint32_t spanEnd = 0;
    if (pm4.TakeConstantSpan(2, spanFirst, spanEnd)) {
      boolsUploaded_ = false;
    }
    if (!boolsUploaded_) {
      std::memcpy(bools_, file + RenderState::BoolConstants, sizeof(bools_));
      out.bools = bools_;
      out.boolsChanged = true;
      boolsUploaded_ = true;
    } else {
      out.bools = nullptr;
      out.boolsChanged = false;
    }
  }

  const bool drawChanged = std::memcmp(&lastConstants_, &constants, sizeof(constants)) != 0;
  const RenderShaders::Program* programs[2] = {vs, ps};
  const RenderState::Handle handles[2] = {out.vertexShader, out.pixelShader};
  constexpr uint32_t kDrawBytes = (sizeof(constants) + 255u) & ~255u;

  out.ring = true;
  uint32_t need[2] = {0, 0};
  uint32_t bytes = 0;
  bool upload[2] = {false, false};

  for (uint32_t stage = 0; stage < 2; ++stage) {
    const std::vector<uint16_t>& map = programs[stage]->constantMap;
    const uint32_t count = map.empty() ? 256u : static_cast<uint32_t>(map.size());
    need[stage] = std::min(256u, std::max(16u, (count + 15u) & ~15u));
    const uint32_t readFirst = map.empty() ? 0u : map.front();
    const uint32_t readEnd = map.empty() ? 256u : static_cast<uint32_t>(map.back()) + 1u;
    const bool dirty = pendingFirst_[stage] < readEnd && pendingEnd_[stage] > readFirst;
    upload[stage] =
        uploaded_.floatAt[stage] == 0xFFFFFFFFu || uploaded_.floatProgram[stage] != handles[stage] || dirty;
    if (upload[stage]) bytes += need[stage] * 16u;
  }

  bool drawUpload = drawChanged || uploaded_.drawAt == 0xFFFFFFFFu;
  if (drawUpload) bytes += kDrawBytes;

  if (bytes != 0) {
    const uint32_t worstBytes = need[0] * 16u + need[1] * 16u + kDrawBytes;
    if (resources_->ConstantWouldWrap(worstBytes)) {
      uploaded_ = UploadedConstants();
      upload[0] = upload[1] = drawUpload = true;
      bytes = worstBytes;
    }
    uint32_t base = 0;
    uint8_t* dst = resources_->ConstantMap(bytes, base);
    if (!dst) return false;

    uint32_t at = 0;
    for (uint32_t stage = 0; stage < 2; ++stage) {
      if (!upload[stage]) continue;
      const uint32_t regBase = stage == 0 ? RenderState::VertexConstants : RenderState::PixelConstants;
      auto* words = reinterpret_cast<uint32_t*>(dst + at);
      const std::vector<uint16_t>& map = programs[stage]->constantMap;
      if (map.empty()) {
        std::memcpy(words, file + regBase, 4096);
      } else {
        for (size_t i = 0; i < map.size(); ++i) {
          std::memcpy(words + i * 4, file + regBase + map[i] * 4u, 16);
        }
        if (need[stage] > map.size()) {
          std::memset(words + map.size() * 4, 0, (need[stage] - map.size()) * 16u);
        }
      }
      uploaded_.floatAt[stage] = base + at;
      uploaded_.floatCount[stage] = need[stage];
      uploaded_.floatProgram[stage] = handles[stage];
      at += need[stage] * 16u;
      pendingFirst_[stage] = 256;
      pendingEnd_[stage] = 0;
    }
    if (drawUpload) {
      std::memcpy(dst + at, &constants, sizeof(constants));
      lastConstants_ = constants;
      uploaded_.drawAt = base + at;
    }
    resources_->ConstantUnmap();
  }

  out.floatAt[0] = uploaded_.floatAt[0];
  out.floatAt[1] = uploaded_.floatAt[1];
  out.floatCount[0] = uploaded_.floatCount[0];
  out.floatCount[1] = uploaded_.floatCount[1];
  out.drawAt = uploaded_.drawAt;
  out.drawBytes = kDrawBytes;
  return true;
}

bool RenderCommands::SetupIndices(const RenderState::Snapshot& state, bool convertQuads,
                                  bool convertFan, bool convertLineLoop,
                                  RenderState::DrawCommand& out) {
  const uint32_t indexCount = state.indexCount;
  const bool strips = out.topology == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP ||
                      out.topology == D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP;
  const bool resetEnabled = strips && (state.suScModeControl & (1u << 21)) != 0;
  const bool convert = convertQuads || convertFan || convertLineLoop;

  out.indexBuffer = nullptr;
  out.indexRingOffset = 0;

  if (state.sourceSelect == 0 && !convert) {
    if (state.indexBase == 0) return false;
    if (!resetEnabled) {
      if (ID3D11Buffer* kept = resources_->StaticIndicesFor(
              state.indexBase, indexCount, state.wideIndices, 0xFFFFFFFFu)) {
        out.indexBuffer = kept;
        out.indices32 = state.wideIndices;
        out.indexed = true;
        out.indexCount = indexCount;
        return true;
      }
    }

    const uint32_t rawBytes = indexCount * (state.wideIndices ? 4u : 2u);
    const uint8_t* data = resources_->PhysicalPtr(state.indexBase, rawBytes);
    if (!data) return false;

    if (state.wideIndices) {
      out.indices32 = true;
      auto* to = reinterpret_cast<uint32_t*>(resources_->IndexMap(indexCount * 4u, out.indexRingOffset));
      if (!to) return false;
      Swap32IntoMasked(to, reinterpret_cast<const uint32_t*>(data), indexCount);
      if (resetEnabled) {
        const uint32_t reset = state.resetIndex & 0xFFFFFFu;
        for (uint32_t i = 0; i < indexCount; ++i) {
          if (to[i] == reset) to[i] = 0xFFFFFFFFu;
        }
      }
      resources_->IndexUnmap();
    } else {
      const auto* from = reinterpret_cast<const uint16_t*>(data);
      const uint32_t reset = state.resetIndex & 0xFFFFu;
      if (resetEnabled && reset != 0xFFFFu) {
        out.indices32 = true;
        auto* to =
            reinterpret_cast<uint32_t*>(resources_->IndexMap(indexCount * 4u, out.indexRingOffset));
        if (!to) return false;
        for (uint32_t i = 0; i < indexCount; ++i) {
          const uint32_t idx = ByteSwap16(from[i]);
          to[i] = idx == reset ? 0xFFFFFFFFu : idx;
        }
        resources_->IndexUnmap();
      } else {
        out.indices32 = false;
        auto* to =
            reinterpret_cast<uint16_t*>(resources_->IndexMap(indexCount * 2u, out.indexRingOffset));
        if (!to) return false;
        Swap16Into(to, from, indexCount);
        resources_->IndexUnmap();
      }
    }
    out.indexed = true;
    out.indexCount = indexCount;
    return true;
  }

  if (!convert) {
    out.indexed = false;
    out.indexCount = indexCount;
    return true;
  }

  scratchIndices_.resize(indexCount);
  if (state.sourceSelect == 0) {
    if (state.indexBase == 0) return false;
    const uint32_t rawBytes = indexCount * (state.wideIndices ? 4u : 2u);
    const uint8_t* data = resources_->PhysicalPtr(state.indexBase, rawBytes);
    if (!data) return false;
    if (state.wideIndices) {
      Swap32IntoMasked(scratchIndices_.data(), reinterpret_cast<const uint32_t*>(data), indexCount);
    } else {
      const auto* from = reinterpret_cast<const uint16_t*>(data);
      for (uint32_t i = 0; i < indexCount; ++i) scratchIndices_[i] = ByteSwap16(from[i]);
    }
  } else {
    for (uint32_t i = 0; i < indexCount; ++i) scratchIndices_[i] = i;
  }

  expandedIndices_.clear();
  if (convertQuads) {
    for (uint32_t i = 0; i + 3 < indexCount; i += 4) {
      const uint32_t q[6] = {scratchIndices_[i],     scratchIndices_[i + 1], scratchIndices_[i + 2],
                             scratchIndices_[i],     scratchIndices_[i + 2], scratchIndices_[i + 3]};
      expandedIndices_.insert(expandedIndices_.end(), q, q + 6);
    }
  } else if (convertFan) {
    for (uint32_t i = 1; i + 1 < indexCount; ++i) {
      const uint32_t t[3] = {scratchIndices_[0], scratchIndices_[i], scratchIndices_[i + 1]};
      expandedIndices_.insert(expandedIndices_.end(), t, t + 3);
    }
  } else if (convertLineLoop) {
    expandedIndices_ = scratchIndices_;
    if (indexCount >= 2) expandedIndices_.push_back(scratchIndices_[0]);
  }
  if (expandedIndices_.empty()) return false;

  const uint32_t bytes = static_cast<uint32_t>(expandedIndices_.size()) * 4u;
  out.indexRingOffset = resources_->IndexAppend(expandedIndices_.data(), bytes);
  out.indices32 = true;
  out.indexed = true;
  out.indexCount = static_cast<uint32_t>(expandedIndices_.size());
  return true;
}

bool RenderCommands::PrepareDraw(Pm4Processor& pm4, uint32_t initiator, uint32_t indexBase,
                                 RenderState::DrawCommand& out) {
  out = RenderState::DrawCommand();
  RenderState::Snapshot state;
  RenderState::ReadSnapshot(pm4, initiator, indexBase, shaders_->Current(false),
                            shaders_->Current(true), state);
  if (state.indexCount == 0 || state.modeControl == 0 || state.pitch == 0) return false;

  bool convertQuads = false;
  bool convertFan = false;
  bool convertLineLoop = false;
  switch (state.primitive) {
    case RenderState::Points: out.topology = D3D11_PRIMITIVE_TOPOLOGY_POINTLIST; break;
    case RenderState::Lines: out.topology = D3D11_PRIMITIVE_TOPOLOGY_LINELIST; break;
    case RenderState::LineStrip: out.topology = D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP; break;
    case RenderState::LineLoop:
      out.topology = D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP;
      convertLineLoop = true;
      break;
    case RenderState::Triangles: out.topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST; break;
    case RenderState::TriangleFan:
      out.topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
      convertFan = true;
      break;
    case RenderState::TriangleStrip: out.topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP; break;
    case RenderState::Rectangles:
      out.topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
      out.rectangles = true;
      break;
    case RenderState::Quads:
      out.topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
      convertQuads = true;
      break;
    default: return false;
  }

  const bool polygonal = !out.rectangles && state.primitive != RenderState::Points &&
                         state.primitive != RenderState::Lines &&
                         state.primitive != RenderState::LineStrip &&
                         state.primitive != RenderState::LineLoop;
  const bool cullFront = (state.suScModeControl & 1u) != 0;
  const bool cullBack = (state.suScModeControl & 2u) != 0;
  if (polygonal && cullFront && cullBack) return false;

  const void* vsPtr = nullptr;
  const void* psPtr = nullptr;
  if (!ResolvePrograms(shaders_, !out.rectangles, out, vsPtr, psPtr)) return false;
  const auto* ps = static_cast<const RenderShaders::Program*>(psPtr);

  const bool alphaTestEnabled =
      (((state.colorControl >> 3) & 1u) != 0) && ((state.colorControl & 7u) != 7u);
  if (DepthOnlyMode(state)) {
    if (!ps->writesDepth && !ps->usesKill && !alphaTestEnabled) {
      out.ps = nullptr;
      out.pixelShader = 0;
    }
  } else if (DepthUnseen(state.depthControl) && XenosHlsl::Depth24() && !ps->writesDepth) {
    if (ID3D11PixelShader* variant = shaders_->NoDepthVariant(out.pixelShader)) {
      out.ps = variant;
      out.psNoDepth = true;
    }
  }

  float scaleX = 1.0f;
  float scaleY = 1.0f;
  if (!SetupTargets(state, ps->colourTargets, out, scaleX, scaleY)) return false;

  float polyScale = 0.0f;
  float polyOffset = 0.0f;
  if (polygonal) {
    if (((state.suScModeControl >> 11) & 1u) && !cullFront) {
      polyScale = state.polyOffset[0];
      polyOffset = state.polyOffset[1];
    }
    if (((state.suScModeControl >> 12) & 1u) && !cullBack && polyScale == 0.0f &&
        polyOffset == 0.0f) {
      polyScale = state.polyOffset[2];
      polyOffset = state.polyOffset[3];
    }
  } else {
    if ((state.suScModeControl >> 13) & 1u) {
      polyScale = state.polyOffset[0];
      polyOffset = state.polyOffset[1];
    }
  }

  const bool isFloat24Depth = ((state.depthInfo >> 16) & 1u) != 0;
  int32_t hostDepthBias = 0;
  if (polyOffset != 0.0f) {
    const float factor =
        isFloat24Depth ? static_cast<float>(1u << 21) : static_cast<float>((1u << 24) - 1u);
    int32_t mag = static_cast<int32_t>(std::ceil(std::abs(polyOffset) * factor));
    if (isFloat24Depth) mag <<= 3;
    hostDepthBias = polyOffset < 0.0f ? -mag : mag;
  }
  const float hostSlopeBias = polyScale * (1.0f / 16.0f) * std::max(scaleX, scaleY);
  const bool depthClip = ((state.clipControl >> 16) & 1u) == 0;

  out.rasterizer = pipeline_->Rasterizer(state.suScModeControl, true, out.rectangles, depthClip,
                                         hostSlopeBias, hostDepthBias);
  out.blend = pipeline_->Blend(state.blendControl, state.colorMask);
  const bool backfaceStencil =
      polygonal && (((state.depthControl >> 7) & 1u) != 0) && cullFront && !cullBack;
  const uint32_t activeStencilRefMask =
      backfaceStencil ? state.stencilRefMaskBF : state.stencilRefMask;
  out.depth = pipeline_->Depth(out.depthView ? state.depthControl : 0u, activeStencilRefMask);
  std::memcpy(out.blendFactor, state.blendFactor, sizeof(out.blendFactor));
  if (RenderPipeline::BlendTakesConstantAlpha(state.blendControl)) {
    out.blendFactor[0] = out.blendFactor[1] = out.blendFactor[2] = state.blendFactor[3];
  }
  out.stencilReference = activeStencilRefMask & 0xFFu;

  uint32_t vteFlags = state.vteControl;
  float vpOffX = (vteFlags & 0x2u) ? state.viewport[1] : 0.0f;
  float vpOffY = (vteFlags & 0x8u) ? state.viewport[3] : 0.0f;
  if ((state.suScModeControl & (1u << 16)) != 0) {
    const float winX = static_cast<float>(static_cast<int32_t>(state.windowOffset << 17) >> 17);
    const float winY = static_cast<float>(static_cast<int32_t>(state.windowOffset << 1) >> 17);
    if (winX != 0.0f || winY != 0.0f) {
      vpOffX += winX;
      vpOffY += winY;
      vteFlags |= 0x2u | 0x8u;
    }
  }

  RenderState::DrawConstants& constants = constants_;
  constants.viewportScale[0] = state.viewport[0];
  constants.viewportOffset[0] = vpOffX;
  constants.viewportScale[1] = state.viewport[2];
  constants.viewportOffset[1] = vpOffY;
  constants.viewportScale[2] = state.viewport[4];
  constants.viewportOffset[2] = state.viewport[5];
  constants.viewportOffset[3] = 0.0f;
  constants.viewportScale[3] = 0.0f;
  constants.targetSize[0] = static_cast<float>(state.pitch);
  constants.targetSize[1] = scaleY > 0.0f ? static_cast<float>(out.targetHeight) / scaleY : 720.0f;
  constants.targetSize[2] = 1.0f / static_cast<float>(state.pitch);
  constants.targetSize[3] =
      out.targetHeight > 0 ? scaleY / static_cast<float>(out.targetHeight) : 1.0f / 720.0f;

  constants.flags[0] = vteFlags | (((state.vertexControl & 1u) != 0) ? 0x10000u : 0u);
  constants.flags[1] = ((state.colorControl >> 3) & 1u) ? (state.colorControl & 7u) : 7u;
  constants.flags[2] = state.alphaReference;
  constants.flags[3] = static_cast<uint32_t>(ps->hash);

  constants.pixelGen[0] = scaleX > 0.0f ? 1.0f / scaleX : 1.0f;
  constants.pixelGen[1] = scaleY > 0.0f ? 1.0f / scaleY : 1.0f;
  constants.pixelGen[2] = static_cast<float>((state.contextMisc >> 8) & 0xFFu);
  constants.pixelGen[3] = ((state.programControl >> 18) & 1u) ? 1.0f : 0.0f;
  for (uint32_t i = 0; i < 4; ++i) {
    int32_t eb = static_cast<int32_t>((state.colorInfo[i] >> 20) & 0x3Fu);
    if (eb >= 32) eb -= 64;
    constants.colorExpBias[i] = std::ldexp(1.0f, eb);
  }

  SetupTextures(pm4, vsPtr, psPtr, out, constants);
  if (!SetupBuffers(pm4, vsPtr, out)) return false;
  if (!SetupConstants(pm4, vsPtr, psPtr, out, constants)) return false;
  if (!SetupIndices(state, convertQuads, convertFan, convertLineLoop, out)) return false;
  out.baseVertex = state.indexOffset;
  return true;
}

bool RenderCommands::PrepareResolve(const Pm4Processor& pm4, RenderState::ResolveCommand& out) {
  out = RenderState::ResolveCommand();
  RenderState::ResolveSnapshot state;
  RenderState::ReadResolveSnapshot(pm4, state);
  const uint32_t pitch = state.pitch & 0x3FFFu;
  if (pitch == 0) return false;

  const uint32_t x0 = state.scissorTopLeft & 0x7FFFu;
  const uint32_t y0 = (state.scissorTopLeft >> 16) & 0x7FFFu;
  uint32_t x1 = state.scissorBottomRight & 0x7FFFu;
  const uint32_t y1 = (state.scissorBottomRight >> 16) & 0x7FFFu;
  // Clamp right edge safely to EDRAM surface pitch (BioShock 1 occasionally leaves scissor at 1280
  // while resolving a 640-pitch buffer).
  if (x1 > pitch) x1 = pitch;
  if (x1 <= x0 || y1 <= y0 || x1 - x0 > 4096 || y1 - y0 > 4096) return false;

  const uint32_t width = x1 - x0;
  const uint32_t height = y1 - y0;
  const uint32_t sourceSelect = state.control & 7u;
  const uint32_t command = (state.control >> 20) & 3u;
  const bool colorClear = ((state.control >> 8) & 1u) != 0;
  const bool depthClear = ((state.control >> 9) & 1u) != 0;

  resources_->EnsureRows(state.surfaceInfo, y1);
  uint32_t targetWidth = 0;
  uint32_t targetHeight = 0;
  float scaleX = 1.0f;
  float scaleY = 1.0f;
  resources_->TargetSize(state.surfaceInfo, 4, targetWidth, targetHeight, scaleX, scaleY);

  const float hostX0 = static_cast<float>(x0) * scaleX;
  const float hostY0 = static_cast<float>(y0) * scaleY;
  const uint32_t hostWidth = std::max(1u, static_cast<uint32_t>(std::lround(width * scaleX)));
  const uint32_t hostHeight = std::max(1u, static_cast<uint32_t>(std::lround(height * scaleY)));

  if (command != 3u && state.destBase != 0) {
    const bool fromDepth = sourceSelect == 4u;
    DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
    if (fromDepth) {
      const RenderResources::DepthTarget* depth =
          resources_->DepthTargetOf(resources_->DepthTargetFor(state.depthInfo, state.surfaceInfo));
      if (!depth || !depth->resource) return false;
      out.source = depth->resource;
      out.sourceWidth = depth->width;
      out.sourceHeight = depth->height;
      out.sourceSamples = depth->samples;
      format = DXGI_FORMAT_R32_FLOAT;
    } else {
      const uint32_t info = state.colorInfo[sourceSelect < 4u ? sourceSelect : 0u];
      const RenderResources::ColorTarget* color =
          resources_->ColorTargetOf(resources_->ColorTargetFor(info, state.surfaceInfo));
      if (!color || !color->resource) return false;
      out.source = color->resource;
      out.sourceWidth = color->width;
      out.sourceHeight = color->height;
      out.sourceSamples = color->samples;
      format = color->format;
    }

    RenderResources::Resolved* surface =
        resources_->ResolvedFor(state.destBase, hostWidth, hostHeight, format, fromDepth);
    if (!surface || !surface->view) return false;
    surface->guestWidth = width;
    surface->guestHeight = height;
    surface->rbSwapped = !fromDepth && (((state.destInfo >> 24) & 1u) != 0);

    out.copy = true;
    out.fromDepth = fromDepth;
    if (fromDepth) {
      out.colorScale = 1.0f;
      out.clampMode = 0.0f;
    } else {
      int32_t eb = static_cast<int32_t>((state.destInfo >> 16) & 0x3Fu);
      if (eb >= 32) eb -= 64;
      out.colorScale = std::ldexp(1.0f, eb);
      const uint32_t destFmt = (state.destInfo >> 7) & 0x3Fu;
      const uint32_t destNum = (state.destInfo >> 13) & 7u;
      const bool isFloatDest = (destNum == 7u) || destFmt == 30u || destFmt == 31u ||
                               destFmt == 32u || destFmt == 36u || destFmt == 37u || destFmt == 38u;
      out.clampMode = isFloatDest ? 0.0f : (destNum == 0u ? 1.0f : (destNum == 1u ? 2.0f : 0.0f));
    }
    out.guestWidth = width;
    out.guestHeight = height;
    out.destinationBase = state.destBase;
    out.destination = surface->view;
    out.destinationResource = surface->resource;
    out.x0 = hostX0;
    out.y0 = hostY0;
    out.width = hostWidth;
    out.height = hostHeight;
  }

  if (colorClear) {
    if (const RenderResources::ColorTarget* color = resources_->ColorTargetOf(
            resources_->ColorTargetFor(state.colorInfo[0], state.surfaceInfo));
        color && color->view) {
      const uint32_t v = state.colorClearValue;
      out.colorClear = true;
      out.clearTarget = color->view;
      out.clearColor[0] = ((v >> 16) & 0xFFu) / 255.0f;
      out.clearColor[1] = ((v >> 8) & 0xFFu) / 255.0f;
      out.clearColor[2] = (v & 0xFFu) / 255.0f;
      out.clearColor[3] = (v >> 24) / 255.0f;
      out.clearRect[0] = static_cast<int32_t>(std::lround(hostX0));
      out.clearRect[1] = static_cast<int32_t>(std::lround(hostY0));
      out.clearRect[2] = static_cast<int32_t>(std::lround(static_cast<float>(x1) * scaleX));
      out.clearRect[3] = static_cast<int32_t>(std::lround(static_cast<float>(y1) * scaleY));
    }
  }

  if (depthClear) {
    if (const RenderResources::DepthTarget* depth = resources_->DepthTargetOf(
            resources_->DepthTargetFor(state.depthInfo, state.surfaceInfo));
        depth && depth->view) {
      const uint32_t v = state.depthClearValue;
      out.depthClear = true;
      out.clearDepth = depth->view;
      const bool isFloat24Depth = ((state.depthInfo >> 16) & 1u) != 0;
      const uint32_t raw24 = v >> 8;
      if (isFloat24Depth) {
        if (raw24 == 0) {
          out.clearDepthValue = 0.0f;
        } else if (raw24 >= 0xFFFFFFu) {
          out.clearDepthValue = 1.0f;
        } else {
          const uint32_t exp = raw24 >> 20;
          const uint32_t mant = raw24 & 0xFFFFFu;
          if (exp == 0) {
            out.clearDepthValue = static_cast<float>(mant) / 17179869184.0f;
          } else {
            const uint32_t fbits = ((exp + 112u) << 23) | (mant << 3);
            std::memcpy(&out.clearDepthValue, &fbits, sizeof(out.clearDepthValue));
          }
        }
      } else {
        out.clearDepthValue = static_cast<float>(raw24) / 16777215.0f;
      }
      out.clearStencilValue = static_cast<uint8_t>(v & 0xFFu);
    }
  }

  return out.copy || out.colorClear || out.depthClear;
}

}  // namespace bs1::renderer
