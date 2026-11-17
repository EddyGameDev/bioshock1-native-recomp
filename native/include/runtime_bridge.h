#pragma once

#include <cstdint>

// Publish the guest memory base used by MMIO runtime helpers.
extern "C" void bs_set_guest_base(uint8_t* base);
