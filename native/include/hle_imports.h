#pragma once

#include "hle_kernel.h"

#include <cstddef>
#include <cstdint>
#include <map>

// Register all 216 measured kernel/XAM import names with the runtime.
// Special handlers implement real semantics; the rest use a safe default that
// returns NTSTATUS_SUCCESS (0) in r3 and records the call for diagnostics.
void register_named_imports(hle::Kernel& kernel);
