#pragma once

// Guest CPU state. Two modes:
//
// * Standalone engine (default): our compact PPCContext for the launcher/HLE
//   self-test.
// * Generated-code build (BS_USE_REX_CONTEXT): alias the real ReXGlue
//   rex::ppc::PPCContext so generated functions see the exact field layout
//   they were compiled against. Never substitute our hand-written struct for
//   generated code.
//
// Use gpr_read()/gpr_write() in runtime code so both modes work.

#include <array>
#include <cstdint>

#if defined(BS_USE_REX_CONTEXT)
// rex/ppc/context.h declares struct PPCContext in the global namespace.
#include <rex/ppc/context.h>
#else

struct PPCRegister {
    uint64_t full{}; // 64-bit guest register (ReXGlue Register::u64 equivalent)
};

struct PPCBoolRegister {
    uint32_t value{};
};

struct PPCContext {
    std::array<PPCRegister, 32> r{};
    PPCRegister lr{};
    PPCRegister ctr{};
    uint32_t cr{};
    uint32_t xer{};
    uint32_t msr{};
    std::array<uint64_t, 32> fpr{};
    uint32_t fpscr{};
    std::array<std::array<uint8_t, 16>, 128> vmx{};
    uint32_t vrsave{};
    uint32_t reserved{};
};

#endif

// ---- Portable GPR accessors ----------------------------------------------

inline uint64_t gpr_read(const PPCContext& ctx, int n) {
#if defined(BS_USE_REX_CONTEXT)
    switch (n) {
        case 0: return ctx.r0.u64;  case 1: return ctx.r1.u64;
        case 2: return ctx.r2.u64;  case 3: return ctx.r3.u64;
        case 4: return ctx.r4.u64;  case 5: return ctx.r5.u64;
        case 6: return ctx.r6.u64;  case 7: return ctx.r7.u64;
        case 8: return ctx.r8.u64;  case 9: return ctx.r9.u64;
        case 10: return ctx.r10.u64; case 11: return ctx.r11.u64;
        case 12: return ctx.r12.u64; case 13: return ctx.r13.u64;
        case 14: return ctx.r14.u64; case 15: return ctx.r15.u64;
        case 16: return ctx.r16.u64; case 17: return ctx.r17.u64;
        case 18: return ctx.r18.u64; case 19: return ctx.r19.u64;
        case 20: return ctx.r20.u64; case 21: return ctx.r21.u64;
        case 22: return ctx.r22.u64; case 23: return ctx.r23.u64;
        case 24: return ctx.r24.u64; case 25: return ctx.r25.u64;
        case 26: return ctx.r26.u64; case 27: return ctx.r27.u64;
        case 28: return ctx.r28.u64; case 29: return ctx.r29.u64;
        case 30: return ctx.r30.u64; case 31: return ctx.r31.u64;
        default: return 0;
    }
#else
    return (n >= 0 && n < 32) ? ctx.r[n].full : 0;
#endif
}

inline void gpr_write(PPCContext& ctx, int n, uint64_t value) {
#if defined(BS_USE_REX_CONTEXT)
    switch (n) {
        case 0: ctx.r0.u64 = value;  break;  case 1: ctx.r1.u64 = value;
        case 2: ctx.r2.u64 = value;  break;  case 3: ctx.r3.u64 = value;
        case 4: ctx.r4.u64 = value;  break;  case 5: ctx.r5.u64 = value;
        case 6: ctx.r6.u64 = value;  break;  case 7: ctx.r7.u64 = value;
        case 8: ctx.r8.u64 = value;  break;  case 9: ctx.r9.u64 = value;
        case 10: ctx.r10.u64 = value; break; case 11: ctx.r11.u64 = value;
        case 12: ctx.r12.u64 = value; break; case 13: ctx.r13.u64 = value;
        case 14: ctx.r14.u64 = value; break; case 15: ctx.r15.u64 = value;
        case 16: ctx.r16.u64 = value; break; case 17: ctx.r17.u64 = value;
        case 18: ctx.r18.u64 = value; break; case 19: ctx.r19.u64 = value;
        case 20: ctx.r20.u64 = value; break; case 21: ctx.r21.u64 = value;
        case 22: ctx.r22.u64 = value; break; case 23: ctx.r23.u64 = value;
        case 24: ctx.r24.u64 = value; break; case 25: ctx.r25.u64 = value;
        case 26: ctx.r26.u64 = value; break; case 27: ctx.r27.u64 = value;
        case 28: ctx.r28.u64 = value; break; case 29: ctx.r29.u64 = value;
        case 30: ctx.r30.u64 = value; break; case 31: ctx.r31.u64 = value;
        default: break;
    }
#else
    if (n >= 0 && n < 32) ctx.r[n].full = value;
#endif
}

inline uint64_t link_register(const PPCContext& ctx) {
#if defined(BS_USE_REX_CONTEXT)
    return ctx.lr;
#else
    return ctx.lr.full;
#endif
}

inline void set_link_register(PPCContext& ctx, uint64_t value) {
#if defined(BS_USE_REX_CONTEXT)
    ctx.lr = value;
#else
    ctx.lr.full = value;
#endif
}

using PPCOpcode = void (*)(PPCContext*, uint8_t*);
