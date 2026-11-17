// Host smoke test for the engine/HLE skeleton (no GLES required).
#include "engine.h"
#include "gpu_layer.h"
#include "hle_kernel.h"
#include "ppc_accessors.h"

#include <cassert>
#include <cstdio>
#include <cstdint>
#include <vector>

extern "C" void __savegprlr_14(PPCContext& ctx, uint8_t* base);
extern "C" void __restgprlr_14(PPCContext& ctx, uint8_t* base);
extern "C" void __savevmx_64(PPCContext& ctx, uint8_t* base);
extern "C" void __restvmx_64(PPCContext& ctx, uint8_t* base);

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    assert(bs_engine_init(".", 640, 480) == 1);
    assert(bs_engine_hle_import_count() > 0);

    // An unregistered import must be reported, not crash.
    PPCContext ctx{};
    uint8_t base = 0;
    assert(!hle::Kernel::instance().call("xboxkrnl.exe", 9999, ctx, &base));
    assert(hle::Kernel::instance().missing_imports().size() == 1);

    // Generated-style __imp__ stub must forward to name-based dispatch.
    std::vector<uint8_t> hle_mem(4096, 0);
    assert(hle::Kernel::instance().call_named("DbgPrint", ctx, hle_mem.data()) == 0);
    assert(hle::Kernel::instance().named_calls() == 1);
    // All 216 measured imports have a registered handler at load.
    assert(hle::Kernel::instance().missing_named_imports().empty());

    // Kernel HLE handlers write their return value into guest r3.
    gpr_write(ctx, 3, 99);
    hle::Kernel::instance().call_named("KeGetCurrentProcessType", ctx, &base);
    assert(gpr_read(ctx, 3) == 2);
    // RtlCompareMemory: first differing byte count (registered import).
    hle_mem[0] = 'a'; hle_mem[1] = 'b'; hle_mem[2] = 'c';
    hle_mem[16] = 'a'; hle_mem[17] = 'b'; hle_mem[18] = 'd';
    gpr_write(ctx, 3, 0);
    gpr_write(ctx, 4, 16);
    gpr_write(ctx, 5, 3);
    hle::Kernel::instance().call_named("RtlCompareMemoryUlong", ctx, hle_mem.data());
    assert(gpr_read(ctx, 3) == 1);

    // Phase 3: synthetic PM4 buffer — one register write, one draw, one swap.
    auto push_be = [](std::vector<uint8_t>& v, uint32_t w) {
        v.push_back(static_cast<uint8_t>(w >> 24));
        v.push_back(static_cast<uint8_t>(w >> 16));
        v.push_back(static_cast<uint8_t>(w >> 8));
        v.push_back(static_cast<uint8_t>(w));
    };
    std::vector<uint8_t> pm4;
    push_be(pm4, 0x00001234u); // type 0, 1 register write, base 0x1234
    push_be(pm4, 0xDEADBEEFu);
    push_be(pm4, 0x000021FBu); // VGT_DMA_SIZE = 36 indices
    push_be(pm4, 0x00000024u);
    push_be(pm4, 0x00002102u); // VGT_INDX_OFFSET = 8
    push_be(pm4, 0x00000008u);
    push_be(pm4, 0xC0002200u); // type 3, PM4_DRAW_INDX, 1 word
    push_be(pm4, 0x00000003u); // index count
    push_be(pm4, 0xC0006400u); // type 3, PM4_XE_SWAP, 1 word
    push_be(pm4, 0x00000000u);
    assert(bs_engine_submit_cmds(pm4.data(), static_cast<uint32_t>(pm4.size())) == 2);
    assert(gpu::pm4_stats().packets == 5);
    assert(gpu::pm4_stats().register_writes == 3);
    assert(gpu::pm4_stats().draws == 1);
    assert(gpu::pm4_stats().swaps == 1);
    assert(gpu::pm4_stats().malformed == 0);
    assert(gpu::register_file().read(0x1234) == 0xDEADBEEFu);
    assert(gpu::register_file().dma_size() == 36);
    assert(gpu::register_file().index_offset() == 8);
    assert(gpu::draw_commands().size() == 1);
    assert(gpu::draw_commands()[0].index_count == 36);
    assert(gpu::draw_commands()[0].index_offset == 8);

    // Phase 2: XDK ABI helpers round-trip through big-endian guest memory.
    std::vector<uint8_t> guest(0x10000, 0);
    PPCContext guest_ctx{};
    gpr_write(guest_ctx, 1, 0x1000);
    gpr_write(guest_ctx, 14, 0x1122334455667788ull);
    set_link_register(guest_ctx, 0xDEADBEEFu);
    __savegprlr_14(guest_ctx, guest.data());
    assert(guest[0x1000 - 0x98] == 0x11 && guest[0x1000 - 0x91] == 0x88);
    gpr_write(guest_ctx, 14, 0);
    set_link_register(guest_ctx, 0);
    __restgprlr_14(guest_ctx, guest.data());
    assert(gpr_read(guest_ctx, 14) == 0x1122334455667788ull);
    assert(link_register(guest_ctx) == 0xDEADBEEFu);

    // VMX helper: save/restore one 16-byte register through guest memory.
    gpr_write(guest_ctx, 12, 0x2000);
    uint8_t* v = vmx_ptr(guest_ctx, 64);
    for (int i = 0; i < 16; ++i) v[i] = static_cast<uint8_t>(i + 1);
    __savevmx_64(guest_ctx, guest.data());
    for (int i = 0; i < 16; ++i) v[i] = 0;
    __restvmx_64(guest_ctx, guest.data());
    for (int i = 0; i < 16; ++i) assert(v[i] == static_cast<uint8_t>(i + 1));

    for (int i = 0; i < 10; ++i) bs_engine_render();
    assert(gpu::stats().frames == 10); // frame counter works without GLES3

    std::printf("status: %s\n", bs_engine_status_json());
    bs_engine_shutdown();
    std::puts("selftest OK");
    return 0;
}
