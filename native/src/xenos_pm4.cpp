#include "xenos_pm4.h"

#include <cstdio>

#if defined(__ANDROID__)
#include <android/log.h>
#define BS_PM4_LOG(...) __android_log_print(ANDROID_LOG_INFO, "BS1-PM4", __VA_ARGS__)
#else
#define BS_PM4_LOG(...) do { std::printf(__VA_ARGS__); std::printf("\n"); } while (0)
#endif

namespace xenos::pm4 {
namespace {

bool g_trace = false;

uint32_t load_be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           (static_cast<uint32_t>(p[3]));
}

bool is_draw(Opcode op) {
    return op == Opcode::DrawIndx || op == Opcode::DrawIndx2 ||
           op == Opcode::DrawIndxBin || op == Opcode::DrawIndx2Bin;
}

void trace_packet(uint32_t type, Opcode op, uint32_t count) {
    if (!g_trace) return;
    BS_PM4_LOG("type=%u opcode=%s(0x%02X) words=%u", type, opcode_name(op),
               static_cast<unsigned>(op), count);
}

}  // namespace

void set_trace(bool enabled) { g_trace = enabled; }
bool trace_enabled() { return g_trace; }

const char* opcode_name(Opcode opcode) {
    switch (opcode) {
        case Opcode::MeInit: return "ME_INIT";
        case Opcode::Nop: return "NOP";
        case Opcode::DrawIndx: return "DRAW_INDX";
        case Opcode::DrawIndx2: return "DRAW_INDX_2";
        case Opcode::DrawIndxBin: return "DRAW_INDX_BIN";
        case Opcode::DrawIndx2Bin: return "DRAW_INDX_2_BIN";
        case Opcode::SetConstant: return "SET_CONSTANT";
        case Opcode::SetConstant2: return "SET_CONSTANT2";
        case Opcode::ImLoad: return "IM_LOAD";
        case Opcode::ImLoadImmediate: return "IM_LOAD_IMMEDIATE";
        case Opcode::Interrupt: return "INTERRUPT";
        case Opcode::XeSwap: return "XE_SWAP";
        default: return "UNKNOWN";
    }
}

CommandBufferStats parse(const void* data, size_t size_bytes,
                         RegisterFile* regs, std::vector<DrawCommand>* draws) {
    CommandBufferStats stats;
    if (!data || (size_bytes % 4) != 0) {
        ++stats.malformed;
        return stats;
    }

    const auto* bytes = static_cast<const uint8_t*>(data);
    size_t offset = 0;

    while (offset + 4 <= size_bytes) {
        const uint32_t packet = load_be32(bytes + offset);
        const uint32_t type = packet >> 30;
        uint32_t count = 0;  // number of words after the header
        Opcode op = Opcode::Unknown;

        switch (type) {
            case 0: {
                count = ((packet >> 16) & 0x3FFF) + 1;
                trace_packet(type, op, count);
                stats.register_writes += count;
                if (regs && offset + 4 + static_cast<size_t>(count) * 4 <= size_bytes) {
                    const uint32_t base_index = packet & 0x7FFF;
                    const bool write_one = ((packet >> 15) & 1u) != 0;
                    for (uint32_t m = 0; m < count; ++m) {
                        const uint32_t reg_data = load_be32(bytes + offset + 4 + m * 4);
                        regs->write(write_one ? base_index : base_index + m, reg_data);
                    }
                }
                break;
            }
            case 1: {
                count = 2;
                trace_packet(type, op, count);
                stats.register_writes += 2;
                if (regs && offset + 12 <= size_bytes) {
                    regs->write(packet & 0x7FF, load_be32(bytes + offset + 4));
                    regs->write((packet >> 11) & 0x7FF, load_be32(bytes + offset + 8));
                }
                break;
            }
            case 2: {
                count = 0;
                trace_packet(type, op, count);
                break;
            }
            case 3: {
                const uint32_t raw_op = (packet >> 8) & 0x7F;
                op = static_cast<Opcode>(raw_op);
                count = ((packet >> 16) & 0x3FFF) + 1;
                trace_packet(type, op, count);

                if (is_draw(op)) {
                    ++stats.draws;
                    if (draws && regs) {
                        draws->push_back({regs->dma_size(), regs->index_offset(),
                                          regs->draw_initiator()});
                    }
                }
                else if (op == Opcode::XeSwap) ++stats.swaps;
                else if (op == Opcode::Interrupt) ++stats.interrupts;
                else if (op == Opcode::ImLoad || op == Opcode::ImLoadImmediate) {
                    ++stats.shader_loads;
                }
                break;
            }
            default:
                ++stats.malformed;
                return stats;
        }

        const size_t packet_bytes = 4 + static_cast<size_t>(count) * 4;
        if (offset + packet_bytes > size_bytes) {
            ++stats.malformed;
            return stats;
        }
        offset += packet_bytes;
        ++stats.packets;
    }

    if (offset != size_bytes) ++stats.malformed;
    return stats;
}

}  // namespace xenos::pm4
