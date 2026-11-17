# Phase 2 link milestone — recompiled code becomes a library

This document records the exact state of the first successful arm64 link of the
recompiled BioShock code. Keep it as the reference point for future regressions.

## Artefact

```
build/codegen-validate-android/libbs1_link_test.so
  size: 145,895,800 bytes (~139 MiB)
  target: aarch64-linux-android28, arm64-v8a
```

## Inputs

* 152 generated `bioshock1_recomp.*.cpp` translation units (staged in
  `recomp/generated/`), compiled with NDK r28c / Clang 19.0.1, C++2b,
  `REX_PHYSICAL_OFFSET_IN_CODE=0`, `BS_NEED_ATOMIC_REF_SHIM=1`, forced-include
  `native/include/compat/atomic_ref.h`, and `POSITION_INDEPENDENT_CODE ON`.
* `bioshock1_init.cpp` and `bioshock1_register.cpp` (image config + function map).
* Our runtime: `engine.cpp`, `hle_kernel.cpp`, `gpu_layer.cpp`,
  `import_stubs.cpp` (216 `__imp__*` kernel stubs), `runtime_stubs.cpp`
  (245 link-symbol placeholders).
* Linker flags: `-Wl,-z,defs` — zero unresolved symbols allowed.

## Symbol verification

`llvm-nm -D libbs1_link_test.so` shows:

```
__imp__DbgPrint    (our import stub → hle::Kernel::call_named)
__imp__xstart      (generated CRT body from bioshock1_recomp.84.cpp)
```

`xstart` is deliberately **not** in `config/runtime_symbols.json`; our stub
generator excludes it so the generated implementation is used.

## Problems solved

| Problem | Fix |
|---|---|
| Native source path one level too shallow | `${CMAKE_CURRENT_LIST_DIR}/../../native` |
| `R_AARCH64_ADR_PREL_PG_HI21` relocation errors | `POSITION_INDEPENDENT_CODE ON` on generated OBJECT target |
| Android libc++ missing `std::atomic_ref` / `jthread` | forced-compat shim `native/include/compat/atomic_ref.h` |
| Import stubs discarded guest register arguments | `NamedHandler`/`call_named` now take `(PPCContext&, uint8_t*)` |
| `xstart` no-op stub would override generated CRT code | excluded from `runtime_symbols.json` + extractor |

## What this does and does not prove

**Proves:** the generated PPC→C++ output is valid, compiles for the real target
ABI, and can be linked into a loadable ELF shared object with our runtime.

**Does not prove:** correct execution. The 245 runtime stubs (ABI helpers etc.)
and 216 import stubs are placeholders. Booting requires the Phase 2 HLE work
described in `20-REXGLUE-V011.md` §3d and `21-HLE-IMPORT-MATRIX.md`.

## Reproduction

```powershell
python tools/extract_runtime_symbols.py
python tools/gen_runtime_stubs.py
python tools/gen_import_stubs.py
# configure build/codegen-validate-android with NDK r28c (see 20-REXGLUE-V011.md)
python scripts/verify-all.py
```
