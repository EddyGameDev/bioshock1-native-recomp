#pragma once

// Minimal Xenos register-file state for Phase 3.
// Register indices come from the SDK's generated register table
// (third_party/rexglue-sdk/include/rex/graphics/register_table.inc).

#include <array>
#include <cstdint>

namespace xenos {

class RegisterFile {
 public:
    // Indices observed in Xenos PM4 type-0 writes.
    static constexpr uint32_t kVgtIndxOffset = 0x2102;
    static constexpr uint32_t kVgtDmaSize = 0x21FB;
    static constexpr uint32_t kVgtDrawInitiator = 0x21FC;

    void write(uint32_t index, uint32_t value) {
        if (index < regs_.size()) regs_[index] = value;
    }

    uint32_t read(uint32_t index) const {
        return index < regs_.size() ? regs_[index] : 0;
    }

    uint32_t index_offset() const { return read(kVgtIndxOffset); }
    uint32_t dma_size() const { return read(kVgtDmaSize); }
    uint32_t draw_initiator() const { return read(kVgtDrawInitiator); }

    void reset() { regs_.fill(0); }

 private:
    std::array<uint32_t, 0x4000> regs_{};
};

struct DrawCommand {
    uint32_t index_count{};
    uint32_t index_offset{};
    uint32_t draw_initiator{};
};

}  // namespace xenos
