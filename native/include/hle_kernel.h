#pragma once

// High-level emulation boundary. ReXGlue already implements the Xboxkrnl/XAM
// surface on Windows; this header defines the subset the Android runtime must
// provide (or forward) for the recompiled BioShock image.

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "ppc_context.h"

namespace hle {

struct ImportInfo {
    std::string library;
    uint16_t ordinal{};
    std::string name;
};

using Handler = std::function<void(PPCContext&, uint8_t*)>;

class Kernel {
public:
    static Kernel& instance();

    bool initialize(const std::string& game_root);
    void shutdown();

    // Bind an XEX ordinal to a native implementation. Generated code calls
    // into the runtime through the import thunks patched by the recompiler.
    void register_import(const std::string& library, uint16_t ordinal,
                         std::string name, Handler handler);

    // Invoke a registered HLE import. Returns false when unimplemented; the
    // caller should log and return a safe default so the game keeps booting.
    bool call(const std::string& library, uint16_t ordinal,
              PPCContext& ctx, uint8_t* base);

    // Guest heap. XenonRecomp-style recompilation assumes the guest image is
    // mapped at its original base address inside this buffer.
    uint8_t* guest_memory() { return memory_.data(); }
    size_t guest_memory_size() const { return memory_.size(); }
    bool map_image(const std::string& path, uint32_t base, size_t image_size);

    const std::string& game_root() const { return game_root_; }
    uint64_t ticks() const;

    // Name-based dispatch used by ReXGlue-generated import stubs. Generated
    // code calls symbols named __imp__<KernelFunction> directly (no ordinal
    // indirection at this layer), so the runtime offers a parallel registry.
    using NamedHandler = std::function<uint64_t(PPCContext&, uint8_t*)>;
    void register_named(const std::string& name, NamedHandler handler);
    uint64_t call_named(const std::string& name, PPCContext& ctx, uint8_t* base);
    uint64_t hle_call_counter() const { return hle_call_counter_; }
    const std::vector<std::string>& latest_calls() const { return latest_calls_; }
    const std::map<std::string, uint64_t>& call_counts() const { return call_counts_; }
    void bump_hle_call_counter() { ++hle_call_counter_; }
    size_t named_calls() const { return named_calls_; }
    const std::vector<std::string>& missing_named_imports() const { return missing_named_; }

    // Statistics used by the launcher's debug overlay.
    size_t implemented_imports() const { return handlers_.size(); }
    const std::vector<std::string>& missing_imports() const { return missing_; }

private:
    Kernel() = default;
    std::string game_root_;
    std::vector<uint8_t> memory_;
    std::unordered_map<uint64_t, ImportInfo> imports_;
    std::unordered_map<uint64_t, Handler> handlers_;
    std::vector<std::string> missing_;
    std::unordered_map<std::string, NamedHandler> named_handlers_;
    std::vector<std::string> missing_named_;
    size_t named_calls_{};
    uint64_t hle_call_counter_{};
    std::vector<std::string> latest_calls_;
    std::map<std::string, uint64_t> call_counts_;
};

// Populate the minimum import set required for first boot. Each real kernel
// function is added here as it is implemented during the port.
void register_bootstrap_imports(Kernel& kernel);

} // namespace hle
