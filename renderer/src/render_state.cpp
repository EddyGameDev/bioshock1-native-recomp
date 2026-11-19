#include "render_state.h"
#include "pm4_processor.h"

#include <cstring>

namespace bs1::renderer::RenderState {

namespace {

inline float RegFloat(const Pm4Processor& pm4, uint32_t index) {
  const uint32_t bits = pm4.ReadReg(index);
  float value = 0.0f;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

}  // namespace

void ReadSnapshot(const Pm4Processor& pm4, uint32_t initiator, uint32_t indexBase,
                  uint64_t vsHash, uint64_t psHash, Snapshot& out) {
  out.primitive = initiator & 0x3F;
  out.sourceSelect = (initiator >> 6) & 3;
  out.indexCount = initiator >> 16;
  out.wideIndices = ((initiator >> 11) & 1) != 0;
  out.indexBase = indexBase;

  out.surfaceInfo = pm4.ReadReg(SurfaceInfo);
  out.pitch = out.surfaceInfo & 0x3FFF;
  out.colorInfo[0] = pm4.ReadReg(ColorInfo0);
  out.colorInfo[1] = pm4.ReadReg(ColorInfo1);
  out.colorInfo[2] = pm4.ReadReg(ColorInfo2);
  out.colorInfo[3] = pm4.ReadReg(ColorInfo3);
  out.depthInfo = pm4.ReadReg(DepthInfo);
  out.modeControl = pm4.ReadReg(ModeControl) & 7;
  out.colorMask = pm4.ReadReg(ColorMask);
  out.depthControl = pm4.ReadReg(DepthControl);
  out.stencilRefMask = pm4.ReadReg(StencilRefMask);
  out.stencilRefMaskBF = pm4.ReadReg(StencilRefMaskBF);

  out.clipControl = pm4.ReadReg(ClipControl);
  out.suScModeControl = pm4.ReadReg(SuScModeControl);
  for (uint32_t i = 0; i < 4; ++i) out.polyOffset[i] = RegFloat(pm4, PolyOffsetFrontScale + i);
  out.scissorTopLeft = pm4.ReadReg(ScissorTopLeft);
  out.scissorBottomRight = pm4.ReadReg(ScissorBottomRight);
  out.windowOffset = pm4.ReadReg(WindowOffset);
  out.indexOffset = static_cast<int32_t>(pm4.ReadReg(IndexOffset));
  out.minVertexIndex = pm4.ReadReg(MinVertexIndex);
  out.maxVertexIndex = pm4.ReadReg(MaxVertexIndex);
  out.resetIndex = pm4.ReadReg(ResetIndex);
  out.pointSize = pm4.ReadReg(PointSize);
  out.vertexControl = pm4.ReadReg(VertexControl);

  out.blendControl[0] = pm4.ReadReg(BlendControl0);
  out.blendControl[1] = pm4.ReadReg(BlendControl1);
  out.blendControl[2] = pm4.ReadReg(BlendControl2);
  out.blendControl[3] = pm4.ReadReg(BlendControl3);
  for (uint32_t i = 0; i < 4; ++i) out.blendFactor[i] = RegFloat(pm4, BlendRed + i);

  out.vteControl = pm4.ReadReg(VteControl);
  out.colorControl = pm4.ReadReg(ColorControl);
  out.alphaReference = pm4.ReadReg(AlphaReference);
  out.programControl = pm4.ReadReg(ProgramControl);
  out.contextMisc = pm4.ReadReg(ContextMisc);
  for (uint32_t i = 0; i < 6; ++i) out.viewport[i] = RegFloat(pm4, ViewportXScale + i);

  out.vertexProgram = vsHash;
  out.pixelProgram = psHash;
}

void ReadResolveSnapshot(const Pm4Processor& pm4, ResolveSnapshot& out) {
  out.control = pm4.ReadReg(CopyControl);
  out.destBase = pm4.ReadReg(CopyDestBase);
  out.destInfo = pm4.ReadReg(CopyDestInfo);
  out.destPitch = pm4.ReadReg(CopyDestPitch);
  out.surfaceInfo = pm4.ReadReg(SurfaceInfo);
  out.pitch = out.surfaceInfo & 0x3FFF;
  out.scissorTopLeft = pm4.ReadReg(ScissorTopLeft);
  out.scissorBottomRight = pm4.ReadReg(ScissorBottomRight);
  out.colorInfo[0] = pm4.ReadReg(ColorInfo0);
  out.colorInfo[1] = pm4.ReadReg(ColorInfo1);
  out.colorInfo[2] = pm4.ReadReg(ColorInfo2);
  out.colorInfo[3] = pm4.ReadReg(ColorInfo3);
  out.depthInfo = pm4.ReadReg(DepthInfo);
  out.colorClearValue = pm4.ReadReg(CopyColorClear);
  out.depthClearValue = pm4.ReadReg(CopyDepthClear);
}

void ReadTextureFetch(const Pm4Processor& pm4, uint32_t slot, uint32_t words[6]) {
  for (uint32_t i = 0; i < 6; ++i) words[i] = pm4.ReadReg(FetchConstants + slot * 6 + i);
}

void ReadVertexFetch(const Pm4Processor& pm4, uint32_t slot, uint32_t& word0, uint32_t& word1) {
  word0 = pm4.ReadReg(FetchConstants + slot * 2);
  word1 = pm4.ReadReg(FetchConstants + slot * 2 + 1);
}

}  // namespace bs1::renderer::RenderState
