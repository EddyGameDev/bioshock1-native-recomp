#pragma once

// Lightweight Xenos PM4 command-stream parser.
//
// PM4 packets are 32-bit big-endian words. Packet type is the top two bits;
// type 0 writes registers, type 1 writes two adjacent registers, type 2 is a
// no-op, and type 3 carries an opcode. This mirrors the packet layout used by
// the Xenia-derived disassembler in third_party/rexglue-sdk, but is a
// standalone implementation so it can run in our Android runtime.
//
// The first Phase 3 milestone is *tracing*: prove we can walk a real command
// buffer and count draws/swaps before translating anything to GLES3.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "xenos_register_file.h"

namespace xenos::pm4 {

enum class Opcode : uint32_t {
    MeInit = 0x48,
    Nop = 0x10,
    DrawIndx = 0x22,
    DrawIndx2 = 0x36,
    DrawIndxBin = 0x34,
    DrawIndx2Bin = 0x35,
    SetConstant = 0x2d,
    SetConstant2 = 0x55,
    ImLoad = 0x27,
    ImLoadImmediate = 0x2b,
    Interrupt = 0x54,
    XeSwap = 0x64,
    Unknown = 0,
};

struct CommandBufferStats {
    uint64_t packets{};
    uint64_t register_writes{};
    uint64_t draws{};
    uint64_t swaps{};
    uint64_t interrupts{};
    uint64_t shader_loads{};
    uint64_t malformed{};
};

// Parse a big-endian PM4 buffer. `size_bytes` must be a multiple of 4.
// If `regs` is non-null, register writes are applied to it and draw opcodes
// capture the current VGT state into `draws` (also optional).
CommandBufferStats parse(const void* data, size_t size_bytes,
                         RegisterFile* regs = nullptr,
                         std::vector<DrawCommand>* draws = nullptr);

// Enable/disable per-packet logcat/stdout tracing (off by default).
void set_trace(bool enabled);
bool trace_enabled();

const char* opcode_name(Opcode opcode);

}  // namespace xenos::pm4
