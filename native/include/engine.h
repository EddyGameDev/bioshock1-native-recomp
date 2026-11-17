#pragma once

// Public C API used by the Kotlin launcher through JNI. Kept flat (no C++
// types in signatures) so it can also be driven by a host-side test runner.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int  bs_engine_init(const char* game_root, int width, int height);
void bs_engine_shutdown(void);
void bs_engine_resize(int width, int height);
void bs_engine_render(void);
void bs_engine_set_controls(const float* axes, int axis_count,
                            const uint8_t* buttons, int button_count);
// Submit a Xenos PM4 command buffer (Phase 3 tracing path).
uint32_t bs_engine_submit_cmds(const void* data, uint32_t size_bytes);
const char* bs_engine_status_json(void);
uint32_t bs_engine_hle_import_count(void);

#ifdef __cplusplus
}
#endif
