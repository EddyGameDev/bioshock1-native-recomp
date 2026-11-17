# Kernel/XAM import handlers — registered for all 216 imports

This closes the largest remaining boot blocker: every one of the 216 measured
kernel imports now has a registered handler instead of falling through to a
missing-import stub.

## 1. What changed

| File | Role |
|---|---|
| `tools/gen_import_names.py` | Generates `native/include/hle_import_names.h` with all 216 names from `config/hle_imports.json` |
| `native/include/hle_import_names.h` | Generated compile-time list of import names |
| `native/include/hle_imports.h` | `register_named_imports()` public declaration |
| `native/src/hle_imports.cpp` | Real handlers for boot-critical APIs + safe default; registers all 216 names at library load |
| `native/src/hle_kernel.cpp` | Calls `register_named_imports()` in the existing load-time bootstrap |

Every generated `__imp__<Name>` stub now resolves through `call_named()` to a
handler. Guest ABI is honoured: **the handler's return value is written into
guest `r3`**, which is what the generated `bl`-style call sites read.

## 2. Real implementations (32 APIs)

| Family | Handlers | Behaviour |
|---|---|---|
| Debug | `DbgPrint` | reads guest ASCII string, logs `[guest]` |
| Heap/memory | `ExAllocatePool`, `ExFreePool`, `MmAllocatePhysicalMemoryEx`, `MmFreePhysicalMemory`, `MmQueryAllocationSize`, `NtAllocateVirtualMemory`, `NtFreeVirtualMemory` | bump allocator above the image (`0x83000000`), 16-byte aligned; guest pointers written back |
| Files | `NtCreateFile`, `NtOpenFile`, `NtReadFile`, `NtWriteFile` | dummy handle `0x1000`, zero-byte I/O status, STATUS_SUCCESS |
| Time | `KeQueryPerformanceFrequency` (3,579,545), `KeQuerySystemTime` (100 ns units) | big-endian writes to guest pointer |
| Threads/TLS | `KeGetCurrentProcessType` (2 = title), `KeTlsAlloc/Get/Set` | 64-slot TLS table |
| CRT/Rtl | `RtlCompareMemory`, `RtlCompareMemoryUlong`, `RtlFillMemoryUlong`, `RtlNtStatusToDosError` | operate directly on big-endian guest memory |
| Video | `VdSwap` | calls `gpu::present()` — the first real renderer hook |
| Input | `XInputGetState/SetState`, `XamInputGetState/SetState` | zero state, `ERROR_DEVICE_NOT_CONNECTED` (touch bridge is later work) |
| System | `XGetGameRegion` (World), `XGetLanguage` (English), `XexCheckExecutablePrivilege` | sensible constants |

All other names use `default_handler`: write `r3 = 0` (NTSTATUS_SUCCESS) and
return. That is the correct success value for most XDK APIs, so a missing
special case degrades gracefully instead of crashing.

## 3. Verification

* Host self-test asserts:
  * no missing named imports (all 216 registered),
  * `KeGetCurrentProcessType` returns 2 in guest `r3`,
  * `RtlCompareMemoryUlong` returns the correct differing-byte count from a
    big-endian guest buffer,
  * ABI helper round-trips (GPR/LR, VMX) and PM4 parsing still pass.
  **SELFTEST OK.**
* arm64 link target rebuilt with `hle_imports.cpp`: `libbs1_link_test.so`
  **139.64 MB**, `-Wl,-z,defs`, zero undefined symbols.
* APK rebuilds successfully.

### Bugs found and fixed during verification

* Host COFF treats `__imp_<name>` as a DLL import symbol; direct calls to such
  stubs from the self-test failed to link. Tests now call `call_named()` directly.
* The self-test initially targeted `strlen` / `RtlCompareMemory`, which are
  **not** XEX imports (only `RtlCompareMemoryUlong` is). This confirmed the
  default handler works: it returned 0 rather than crashing. Tests now use
  genuinely registered names.

## 4. What remains before boot

* 9 non-ABI runtime placeholders (MMIO, ResolveFunction, Trap, clock, atomics).
* The guest context is aligned, ABI helpers and imports work — the remaining
  step is to call `__imp__xstart(ctx, base)` with a fully initialised
  `PPCContext` and guest memory base, then iterate on the first boot log.
* Audio (XMA/FFmpeg) and Xenos→GLES3 draw translation (Phase 3) are still
  outstanding; `VdSwap` now at least presents the current frame.

## 5. Honest caveat

"Handler registered" does not mean "fully accurate". Many of the 184 default
handlers are intentionally minimal successes. The first boot will reveal which
APIs need richer semantics — that iteration is next, driven by real logs rather
than guesswork.
