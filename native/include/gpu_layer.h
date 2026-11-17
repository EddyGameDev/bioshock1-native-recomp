#pragma once

// GLES3 presentation layer. This replaces ReXGlue's D3D12/Vulkan backends for
// Android. The recompiled game submits Xenos command buffers through the
// render layer; until the Xenos->GLES3 command translator is implemented the
// layer exposes a diagnostic path that draws a test pattern to prove the
// Android graphics pipeline works end-to-end.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "xenos_pm4.h"
#include "xenos_register_file.h"

namespace gpu {

struct FrameStats {
    uint64_t frames{};
    uint64_t draws{};
    uint64_t textures{};
};

bool initialize(int width, int height);
void shutdown();
void resize(int width, int height);

// Main present path. Once the Xenos PM4 parser is ported this consumes the
// command buffer produced by the recompiled game.
void present();

// Phase 3: submit a Xenos PM4 command buffer. Currently parses and counts
// packets; later this will dispatch draws to GLES3.
xenos::pm4::CommandBufferStats submit_command_buffer(const void* data, size_t size_bytes);
const xenos::pm4::CommandBufferStats& pm4_stats();
const xenos::RegisterFile& register_file();
const std::vector<xenos::DrawCommand>& draw_commands();

// Diagnostic path used by the launcher's self-test.
void draw_test_pattern(float time_seconds);

// Texture helpers used by the Xenos texture detiler (BC1..BC7 upload).
uint32_t upload_texture(const void* data, int width, int height, uint32_t format);

const FrameStats& stats();

} // namespace gpu
