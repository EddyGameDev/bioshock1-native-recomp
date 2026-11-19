#pragma once

#include "render_state.h"

#include <cstdint>
#include <vector>

namespace bs1::renderer {

class Pm4Processor;
class RenderPipeline;
class RenderResources;
class RenderShaders;

/**
 * Translates snapshots of Xenos GPU registers into self-contained D3D11
 * DrawCommand / ResolveCommand records without performing D3D context draws.
 */
class RenderCommands {
 public:
  void Initialize(RenderResources* resources, RenderPipeline* pipeline, RenderShaders* shaders);
  void Reset();

  bool PrepareDraw(Pm4Processor& pm4, uint32_t initiator, uint32_t indexBase,
                   RenderState::DrawCommand& out);
  bool PrepareResolve(const Pm4Processor& pm4, RenderState::ResolveCommand& out);

  /** Updates observed guest main-frame height (e.g. 360 or 720) for named resolution presets. */
  void ObserveSwapHeight(uint32_t width, uint32_t height);
  uint32_t ObservedMainHeight() const { return observedMainHeight_; }

 private:
  bool ResolvePrograms(RenderShaders* shaders, bool allowAsync, RenderState::DrawCommand& out,
                       const void*& vsOut, const void*& psOut);
  bool SetupTargets(const RenderState::Snapshot& state, uint32_t psColourTargets,
                    RenderState::DrawCommand& out, float& scaleXOut, float& scaleYOut);
  void SetupTextures(const Pm4Processor& pm4, const void* vsPtr, const void* psPtr,
                     RenderState::DrawCommand& out, RenderState::DrawConstants& constants);
  bool SetupBuffers(const Pm4Processor& pm4, const void* vsPtr, RenderState::DrawCommand& out);
  bool SetupConstants(Pm4Processor& pm4, const void* vsPtr, const void* psPtr,
                      RenderState::DrawCommand& out, const RenderState::DrawConstants& constants);
  bool SetupIndices(const RenderState::Snapshot& state, bool convertQuads, bool convertFan,
                    bool convertLineLoop, RenderState::DrawCommand& out);

  RenderResources* resources_ = nullptr;
  RenderPipeline* pipeline_ = nullptr;
  RenderShaders* shaders_ = nullptr;

  RenderState::DrawConstants constants_{};
  RenderState::DrawConstants lastConstants_{};

  struct UploadedConstants {
    uint32_t floatAt[2] = {0xFFFFFFFFu, 0xFFFFFFFFu};
    uint32_t floatCount[2] = {0, 0};
    RenderState::Handle floatProgram[2] = {0, 0};
    uint32_t drawAt = 0xFFFFFFFFu;
  };
  UploadedConstants uploaded_{};
  uint32_t pendingFirst_[2] = {0, 0};
  uint32_t pendingEnd_[2] = {256, 256};
  uint32_t floatFileFallback_[2][1024] = {};
  uint32_t bools_[40] = {};
  bool boolsUploaded_ = false;

  std::vector<uint32_t> scratchIndices_;
  std::vector<uint32_t> expandedIndices_;
  uint32_t observedMainHeight_ = 360;
};

}  // namespace bs1::renderer
