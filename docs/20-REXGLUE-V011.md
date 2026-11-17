# ReXGlue v0.11 prebuilt toolkit — CLI and project model

The vendored `third_party/rexglue-sdk` is a v0.8-era source snapshot with 22
uninitialised submodules. For actual host codegen we use the **prebuilt v0.11.0
win-amd64 release**, installed by `scripts/install-rexglue.ps1` into
`tools-bin/rexglue-sdk-0.11.0/`. This document records the observed interface so
the pipeline does not depend on stale documentation.

## 1. CLI (observed from `rexglue --help`)

Global options: `--log-level`, `--log-file`, `-v/--verbose`, `-f/--force`.

Subcommands:

| Subcommand | Purpose |
|---|---|
| `init` | Scaffold a project: `--project-name`, `--xex-path`, `--game-root`, `--project-root`, optional `--scan-dll`, `--template-dir`; subcommands `module`, `achievements` |
| `codegen` | `rexglue codegen [manifest.toml]`; flags for iteration limits, jump-table/backward-scan limits, `--target NAME` (repeatable), `--ignore-stamp`, max file size |
| `recompile-tests` | Generate Catch2 tests from PPC assembly |
| `vfs dump` | Extract STFS / SVOD / GDFX containers |
| `list shim` | List functions a project can replace with a shim |

## 2. Generated project (`_work/rexglue-bioshock1/`)

`rexglue init --project-name bioshock1 --xex-path <default.xex>
--game-root <_game> --project-root <_work/rexglue-bioshock1>` produced:

```
bioshock1_manifest.toml
CMakeLists.txt
CMakePresets.json
src/main.cpp
src/bioshock1_hooks.cpp
```

Manifest (exact content, paths relative to the project root):

```toml
[project]
name = "bioshock1"
sdk_version = "0.11.0"
game_root = "../../_game"
hook_sources = ["src"]

[entrypoint]
file_path = "../../_game/default.xex"
out_directory_path = "generated/default"
includes = []
```

This is much simpler than the v0.8 manifest described in the vendored docs.
Additional DLL modules (if the game has any) would be added as `[[modules]]`
entries (`rexglue init module`). BioShock's code is in the single XEX.

## 3. Generated host scaffold

`src/main.cpp`:

```cpp
#include <rex/app/title_app.h>
#include "generated/default/bioshock1_init.h"

class Bioshock1App : public rex::app::TitleApp {
 public:
  Bioshock1App() : TitleApp("bioshock1", kPPCImageConfig) {}
};

REX_TITLE_APP(Bioshock1App)
```

`src/bioshock1_hooks.cpp` just includes `generated/default/bioshock1_funcs.h`.
`CMakeLists.txt` uses `rexglue_project()` / `rexglue_setup_target()` from the
SDK's CMake package and builds a Win32 GUI executable.

## 3b. Observed codegen output for BioShock (2026-11-17)

`rexglue codegen` completed in 61.0 s: **308 files written**, 311 files present,
156.5 MB total, no warnings or errors. ReXGlue's own header dump confirmed
Title `545407D8`, Media `44E704F7`, Version `0.0.0.1`, Filetime
`2007-06-22 23:21:30 UTC` — independent validation of our XEX analysis.

Inventory:

| Files | Count | Purpose |
|---|---|---|
| `bioshock1_recomp.*.cpp` | 151 | generated PPC→C++ function bodies |
| `bioshock1_funcs.*.h` | 151 | `DECLARE_REX_FUNC` declarations |
| `bioshock1_funcs.h` | 1 | aggregate declarations — **36,625 functions** (`sub_82160000` …) |
| `bioshock1_init.cpp/.h` | 2 | `kPPCImageConfig`, `kPPCFuncMappings`, `kPPCCallUsage` |
| `bioshock1_register.cpp` | 1 | `bioshock1_RegisterFunctions()` → `registrar->SetFunction(addr, fn)` |
| `bioshock1_pch.h` | 1 | image/code base macros and SDK includes |
| `codegen.partition.json`, `codegen.mmio.json`, `sources.cmake` | 3 | analysis metadata and build inputs |

Generated constants (`bioshock1_pch.h`):

```
REX_IMAGE_BASE          0x82000000
REX_IMAGE_SIZE          0x00CD0000
REX_CODE_BASE           0x82160000
REX_CODE_SIZE           0x008F195C
REX_THUNK_RESERVE_SIZE  0x00010000
```

**Important integration finding:** `bioshock1_register.cpp` registers 36,625
*recompiled guest functions*; there are no generated per-import shim files.
Import resolution is handled by ReXGlue's runtime function dispatcher
(`rex/kernel/function_dispatcher.h`) using the imported ordinal → guest thunk
address mapping already present in the XEX. Our Android HLE must therefore
integrate with that dispatcher model (or reimplement it) rather than bind
one generated shim per import as originally assumed in `06-HLE-KERNEL.md`.
This is recorded in `09-RE-FINDINGS.md` and will drive the next design pass.

## 3c. Android arm64 validation (2026-11-17)

All 152 generated translation units compile to arm64 objects with:

* NDK **r28c** (28.2.13676358, Clang 19.0.1), API 28, `arm64-v8a`;
* `-DREX_PHYSICAL_OFFSET_IN_CODE=0`;
* forced-include `native/include/compat/atomic_ref.h` with
  `-DBS_NEED_ATOMIC_REF_SHIM=1` (provides `std::atomic_ref`, `stop_token`,
  `stop_source`, `jthread` missing from Android libc++);
* SDK headers from the prebuilt v0.11 package.

NDK 26.3 (originally installed) is too old for ReXGlue's headers; it remains
the default for the hand-written engine/APK build, while NDK r28c in
`tools-bin/` is used for generated-code validation.

Link-time symbol inventory (`llvm-nm -u` on the arm64 objects):

| Category | Count | Status |
|---|---|---|
| `__imp__<KernelFunction>` | 216 | covered by `import_stubs.cpp` |
| `__rest*` / `__save*` ABI helpers | 236 | ReXGlue runtime responsibility |
| mangled `rex::` runtime functions | 6 | need clean-room implementations |
| compiler/atomic (`__main`, `__aarch64_cas*`) | 3 | toolchain runtime |
| weak `sub_*` aliases | ~36k | resolved inside generated set |

## 3d. Phase 2 link milestone (2026-11-17)

`libbs1_link_test.so` was linked successfully:

* command: CMake Android (NDK r28c, API 28, arm64-v8a), target
  `bs1_link_test` in `tools/codegen-validate/CMakeLists.txt`;
* inputs: all 152 generated arm64 objects (`POSITION_INDEPENDENT_CODE ON`)
  + `native/src/{engine,hle_kernel,gpu_layer,import_stubs,runtime_stubs}.cpp`;
* linker: `ld.lld` with `-Wl,-z,defs` → **zero unresolved symbols**;
* output: `build/codegen-validate-android/libbs1_link_test.so`, **139.14 MB**;
* `llvm-nm -D` confirms `__imp__xstart` comes from the generated code
  (`bioshock1_recomp.84.cpp`) and `__imp__DbgPrint` from our import stub.

Fixes needed along the way: native source path (`../../native`), PIC on the
generated OBJECT target, removal of `xstart` from the runtime-stub list because
the generated CRT body already defines it.

### Remaining before the guest can boot

1. ~~Replace the 245 no-op stubs~~ **DONE for 236 ABI helpers** — see
   `27-ABI-HELPERS.md`; 9 non-ABI placeholders remain.
2. ~~Use ReXGlue's `rex::ppc::PPCContext` layout~~ **DONE** — see §3e below.
3. Implement HLE handlers from `21-HLE-IMPORT-MATRIX.md` in priority order.
4. Switch the APK's CMake build to NDK r28c and enable
   `BS_USE_GENERATED_CODE`, then wire `engine.cpp` to start the guest thread.

## 3e. Guest context alignment (done)

`native/include/ppc_context.h` now has two modes:

* **standalone** ( launcher / selftest ): our compact `PPCContext` with
  `r[0..31]`, LR, CTR, CR, XER, FPRs and VMX registers;
* **`BS_USE_REX_CONTEXT=1`**: includes `<rex/ppc/context.h>` and uses the real
  global `struct PPCContext` declared there (`r3` first, then `r0,r1,r2,r4…r31`,
  `lr`, `ctr`, `xer`, `cr0..cr7`, FPRs, VMX, etc.). This is the type generated
  functions were compiled against — never substitute our struct in that build.

Runtime code accesses registers through `gpr_read(ctx,n)` / `gpr_write(ctx,n,v)`
and `link_register()` / `set_link_register()`, which compile in both modes.
All direct `ctx.r[3].value` uses in `hle_kernel.cpp` were replaced.

Verified: the host selftest passes in standalone mode, and the arm64 link
target rebuilt with `BS_USE_REX_CONTEXT=1` and `-Wl,-z,defs` still links
`libbs1_link_test.so` (139.22 MB) with zero unresolved symbols. Fixes along the
way: `PPCContext` is in the global namespace (not `rex::ppc`), the validation
target needed `xenos_pm4.cpp` added and `log` linked for `__android_log_print`.

## 4. Bridge from generated host code to our Android runtime

The generated output is intended for `rex::app::TitleApp`, which provides the
Windows/D3D12/Vulkan host services. Our Android port needs the generated
**PPC block functions and init tables**, not that host app. The plan:

1. Let codegen finish and inventory `generated/default/` (headers, function
   tables, import shims, jump tables, init config).
2. Keep the generated sources in `_work/` (gitignored) until validated.
3. Copy the *codegen subset* into `recomp/generated/`:
   - PPC function implementations (`*.cpp`),
   - `bioshock1_init.h` / config tables,
   - import thunk table (to bind to `hle::Kernel::call`).
4. Exclude SDK-specific includes (`rex/app/*`, D3D12/Vulkan GPU backends) —
   those are replaced by `native/include/{hle_kernel,gpu_layer}.h`.
5. Add the subset to the `bioshock1-native` CMake target with
   `BS_GLES3` enabled, then compile for arm64 to catch incompatibilities early.
6. Adapt `engine.cpp` to call the generated entry point with a `PPCContext`
   laid out exactly as ReXGlue expects.

This is why `09-RE-FINDINGS.md` and `native/include/ppc_context.h` must record
the exact runtime conventions (register struct layout, base pointer, indirect
call hash table placement).

## 5. Version-skew rules

* `rexglue <subcommand> --help` is authoritative for CLI syntax.
* The generated scaffold is authoritative for manifest keys and hook files.
* The vendored v0.8 docs are background reading only; every difference found
  is logged in `19-WORKLOG.md`.
* Do not hand-edit generated files — add hooks in `src/bioshock1_hooks.cpp`
  and manifest overrides, then re-run codegen.
