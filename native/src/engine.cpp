#include "engine.h"

#include "gpu_layer.h"
#include "hle_kernel.h"
#include "ppc_context.h"

#include <array>
#include <cstdio>
#include <string>

namespace {

bool g_initialized = false;
std::array<float, 8> g_axes{};
std::array<uint8_t, 32> g_buttons{};
PPCContext g_context;

std::string status_json() {
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "{\"initialized\":%s,\"hleImports\":%zu,"
                  "\"missingImports\":%zu,\"frames\":%llu,\"draws\":%llu,"
                  "\"textures\":%llu,\"pm4Draws\":%llu,\"pm4Swaps\":%llu}",
                  g_initialized ? "true" : "false",
                  hle::Kernel::instance().implemented_imports(),
                  hle::Kernel::instance().missing_imports().size(),
                  static_cast<unsigned long long>(gpu::stats().frames),
                  static_cast<unsigned long long>(gpu::stats().draws),
                  static_cast<unsigned long long>(gpu::stats().textures),
                  static_cast<unsigned long long>(gpu::pm4_stats().draws),
                  static_cast<unsigned long long>(gpu::pm4_stats().swaps));
    return buf;
}

} // namespace

extern "C" {

int bs_engine_init(const char* game_root, int width, int height) {
    if (!game_root) return 0;

    auto& kernel = hle::Kernel::instance();
    if (!kernel.initialize(game_root)) return 0;

    // The recompiled image is mapped at 0x82000000 by ReXGlue/XenonRecomp
    // conventions. Until codegen is wired in we simply reserve the memory;
    // a real build maps the decompressed PE image here.
    kernel.map_image(std::string(game_root) + "/default.xex", 0x82000000u,
                     16u * 1024u * 1024u);

    if (!gpu::initialize(width, height)) return 0;

    g_context = PPCContext{};
    g_initialized = true;
    return 1;
}

void bs_engine_shutdown() {
    gpu::shutdown();
    hle::Kernel::instance().shutdown();
    g_initialized = false;
}

void bs_engine_resize(int width, int height) {
    gpu::resize(width, height);
}

void bs_engine_render() {
    gpu::present();
}

void bs_engine_set_controls(const float* axes, int axis_count,
                            const uint8_t* buttons, int button_count) {
    for (int i = 0; i < axis_count && i < static_cast<int>(g_axes.size()); ++i) {
        g_axes[i] = axes[i];
    }
    for (int i = 0; i < button_count && i < static_cast<int>(g_buttons.size()); ++i) {
        g_buttons[i] = buttons[i];
    }
    // Bridge to XInput HLE: g_context.r[3] = user index, r[4] = state pointer.
}

uint32_t bs_engine_submit_cmds(const void* data, uint32_t size_bytes) {
    const auto stats = gpu::submit_command_buffer(data, size_bytes);
    return static_cast<uint32_t>(stats.draws + stats.swaps);
}

const char* bs_engine_status_json() {
    static std::string cache = status_json();
    cache = status_json();
    return cache.c_str();
}

uint32_t bs_engine_hle_import_count() {
    return static_cast<uint32_t>(hle::Kernel::instance().implemented_imports());
}

} // extern "C"
