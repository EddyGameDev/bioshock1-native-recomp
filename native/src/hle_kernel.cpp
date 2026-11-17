#include "hle_kernel.h"
#include "hle_imports.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>

namespace hle {

// Register the bootstrap import set as soon as the process starts, so the
// launcher can show a meaningful HLE count before a game is launched.
// initialize() re-registers idempotently when guest memory is allocated.
static const bool kBootstrapAtLoad = [] {
    register_bootstrap_imports(Kernel::instance());
    register_named_imports(Kernel::instance());
    return true;
}();

namespace {

uint64_t key_for(const std::string& library, uint16_t ordinal) {
    return (static_cast<uint64_t>(std::hash<std::string>{}(library)) << 16) | ordinal;
}

void log_missing(const std::string& name, std::vector<std::string>& missing) {
    if (std::find(missing.begin(), missing.end(), name) == missing.end()) {
        missing.push_back(name);
    }
}

// ---- Bootstrap implementations -------------------------------------------
// These are intentionally simple. They exist so the runtime can be exercised
// before the full XDK surface is ported. Each one records its guest ABI in
// comments; see docs/05-HLE-KERNEL.md before extending.

void dbg_print(PPCContext& ctx, uint8_t*) {
    // r3 = format string pointer (guest address), r4..r6 = varargs.
    (void)ctx;
    // Real implementation: read a NUL-terminated string from guest memory,
    // then format varargs. Return value unused.
}

void ex_allocate_pool(PPCContext& ctx, uint8_t*) {
    // r3 = size, r4 = tag. Store the allocation through the guest heap.
    (void)ctx;
}

void ke_stall_execution_processor(PPCContext& ctx, uint8_t*) {
    (void)ctx; // r3 = microseconds; native busy-wait not needed in the skeleton.
}

void ke_delay_execution_thread(PPCContext& ctx, uint8_t*) {
    (void)ctx; // r3 = processor mode, r4 = alertable, r5 = interval pointer.
}

void ke_get_current_process_type(PPCContext& ctx, uint8_t*) {
    gpr_write(ctx, 3, 2); // kXboxProcessTitle
}

void ke_query_performance_frequency(PPCContext& ctx, uint8_t*) {
    // r3 = pointer to LARGE_INTEGER (written big-endian by guest code).
    (void)ctx;
}

void xex_get_module_handle(PPCContext& ctx, uint8_t*) {
    gpr_write(ctx, 3, 0); // not found
}

void xinput_get_state(PPCContext& ctx, uint8_t*) {
    // r3 = user index, r4 = XINPUT_STATE pointer. Filled by the input bridge.
    (void)ctx;
    gpr_write(ctx, 3, 0); // ERROR_SUCCESS
}

} // namespace

Kernel& Kernel::instance() {
    static Kernel k;
    return k;
}

bool Kernel::initialize(const std::string& game_root) {
    game_root_ = game_root;
    // 512 MB mirror of the Xenon physical address space. ReXGlue-generated
    // code expects the image to be present at its original base address.
    memory_.assign(512u * 1024u * 1024u, 0);
    missing_.clear();
    missing_named_.clear();
    named_calls_ = 0;
    register_bootstrap_imports(*this);
    return true;
}

void Kernel::shutdown() {
    memory_.clear();
    handlers_.clear();
    imports_.clear();
    named_handlers_.clear();
    missing_named_.clear();
}

bool Kernel::map_image(const std::string& path, uint32_t base, size_t image_size) {
    if (base + image_size > memory_.size()) {
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    in.read(reinterpret_cast<char*>(memory_.data() + base),
            static_cast<std::streamsize>(image_size));
    return in.good() || in.eof();
}

void Kernel::register_import(const std::string& library, uint16_t ordinal,
                             std::string name, Handler handler) {
    const uint64_t k = key_for(library, ordinal);
    imports_[k] = ImportInfo{library, ordinal, std::move(name)};
    handlers_[k] = std::move(handler);
}

bool Kernel::call(const std::string& library, uint16_t ordinal,
                  PPCContext& ctx, uint8_t* base) {
    const uint64_t k = key_for(library, ordinal);
    auto it = handlers_.find(k);
    if (it == handlers_.end()) {
        auto name_it = imports_.find(k);
        std::string name = name_it == imports_.end()
            ? library + "!" + std::to_string(ordinal)
            : name_it->second.name;
        log_missing(name, missing_);
        return false;
    }
    it->second(ctx, base);
    return true;
}

void Kernel::register_named(const std::string& name, NamedHandler handler) {
    named_handlers_[name] = std::move(handler);
}

uint64_t Kernel::call_named(const std::string& name, PPCContext& ctx, uint8_t* base) {

    ++named_calls_;
    bump_hle_call_counter();
    ++call_counts_[name];
    {
        static std::vector<std::string> ring;
        ring.push_back(name);
        if (ring.size() > 40) ring.erase(ring.begin());
        latest_calls_ = ring;
    }
    auto it = named_handlers_.find(name);
    if (it == named_handlers_.end()) {
        log_missing(name, missing_named_);
        return 0;
    }
    return it->second(ctx, base);
}

uint64_t Kernel::ticks() const {
    return static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
}

void register_bootstrap_imports(Kernel& kernel) {
    kernel.register_import("xboxkrnl.exe", 3, "DbgPrint", dbg_print);
    kernel.register_import("xboxkrnl.exe", 9, "ExAllocatePoolWithTag", ex_allocate_pool);
    kernel.register_import("xboxkrnl.exe", 46, "KeDelayExecutionThread", ke_delay_execution_thread);
    kernel.register_import("xboxkrnl.exe", 52, "KeGetCurrentProcessType", ke_get_current_process_type);
    kernel.register_import("xboxkrnl.exe", 78, "KeQueryPerformanceFrequency", ke_query_performance_frequency);
    kernel.register_import("xboxkrnl.exe", 109, "KeStallExecutionProcessor", ke_stall_execution_processor);
    kernel.register_import("xboxkrnl.exe", 608, "XexGetModuleHandle", xex_get_module_handle);
    kernel.register_import("xboxkrnl.exe", 641, "XInputGetState", xinput_get_state);
}

} // namespace hle
