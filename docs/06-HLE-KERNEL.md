# HLE kernel design (Xboxkrnl / XAM)

The recompiled game code calls the Xbox 360 kernel through **import thunks**.
At link time those thunks are patched to native functions; those native
functions *are* the high-level emulation layer. This document describes what
exists now and how to grow it.

## 1. Why HLE instead of running the real kernel

The Xbox 360 kernel and XAM live in the console's BIOS/flash. Static
recompilation translates only the game's XEX; the kernel is not part of it.
Two options:

* **LLE**: emulate the PowerPC kernel itself (would require the BIOS and a
  full interpreter/JIT — explicitly out of scope here).
* **HLE**: reimplement the *API surface* natively. This is what ReXGlue,
  Xenia and every recompilation project do.

Your `xbox360-bios-v10.bin` (256 KB, standard X360 bootrom size) is copied to
`_reference/` and is useful for identifying the console generation and for
Xenia-based experiments. It is **not** used at runtime by this project and is
never packaged.

## 2. Current implementation

`native/include/hle_kernel.h` + `native/src/hle_kernel.cpp` provide:

| Facility | Implementation |
|---|---|
| Import registry | `unordered_map<hash(library)<<16 \| ordinal, Handler>`; `register_import()` / `call()` |
| Guest memory | 512 MB `std::vector<uint8_t>`, exposed via `guest_memory()`; `map_image()` bounds-checks base+size |
| Bootstrap imports | 8 handlers registered on `initialize()`: `DbgPrint`, `ExAllocatePoolWithTag`, `KeDelayExecutionThread`, `KeGetCurrentProcessType`, `KeQueryPerformanceFrequency`, `KeStallExecutionProcessor`, `XexGetModuleHandle`, `XInputGetState` |
| Missing-import tracking | `call()` records unknown imports once; `missing_imports()` list feeds the launcher debug overlay |
| Diagnostics | `implemented_imports()`, `ticks()` |

The host self-test verifies that an unknown ordinal is recorded without a
crash. This is the fail-soft behaviour a port needs during early boot: a
missing API returns a safe default instead of taking down the whole process.

### Name-based dispatch for generated code (added in Phase 1)

`hle::Kernel::register_named()` / `call_named()` maintain a second registry
keyed by the **exact kernel function name**. This is required because
ReXGlue-generated code calls Windows-style symbols `__imp__DbgPrint`,
`__imp__NtCreateFile`, etc. directly — there is no ordinal indirection at this
layer. `tools/gen_import_stubs.py` reads `config/hle_imports.json` (216 names
measured from the generated object files) and emits `native/src/import_stubs.cpp`
with one `extern "C" uint64_t __imp__<Name>()` per import, each forwarding to
`call_named()`. Unimplemented imports are counted and listed in
`missing_named_imports()` for the launcher overlay — fail-soft by design.

See `21-HLE-IMPORT-MATRIX.md` for the full measured list and priority order.

### Guest ABI convention

Xbox 360 uses the PowerPC ABI: arguments in `r3..r10`, return value in `r3`,
link register in `LR`. A handler therefore reads `ctx.r[2]` (r3) for the first
argument, etc. The `PPCContext` struct in `include/ppc_context.h` exposes all
32 GPRs, 32 FPRs, 128 VMX registers, CR/LR/CTR/XER/MSR and FPSCR, matching the
layout expected by ReXGlue/XenonRecomp generated code.

Example stub pattern (from the vendored rexdex README) looks like this in the
generated world:

```cpp
uint64 __fastcall XboxThreads_KeDelayExecutionThread(uint64 ip, cpu::CpuRegs& regs)
{
    const uint32 processor_mode = regs.R3;
    const uint32 alertable      = regs.R4;
    const uint64 interval_ptr   = regs.R5;
    // ... read guest memory, perform native wait, RETURN_ARG(result);
}
```

Our `Kernel::call()` is the dispatch point that replaces the generated
`__fastcall` wrapper once the recompiler's import stubs are wired in.

## 3. The real scope: 427 import records

The XEX imports (measured, see `03-XEX-ANALYSIS.md`):

| Module | Records | Dominant API families |
|---|---|---|
| `xboxkrnl.exe` | 325 | memory management, threads/events/semaphores, NT file I/O, CRT, D3D9/Vd* video, XAudio, XMA, XInput |
| `xam.xex` | 102 | sign-in, user/profile, content, UI dialogs, input, notifications, tasks |

The ordinal numbers live inside the compressed PE descriptors, so the
authoritative list comes from ReXGlue codegen. `360tools` includes
`parse_xex_imports.py`, but its ordinal extraction is unreliable (it treats
thunk addresses as ordinals) — do not use its output for stub prioritisation.
The intended source of truth is:

```
rexglue codegen  ->  generated import list  ->  HLE stub manifest
```

## 4. Stub priority playbook

ReXGlue v0.8.0 already implements most of the standard XDK surface. The work
is therefore:

1. **Build ReXGlue for the host** and run `rexglue init`/`codegen` on
   `default.xex`. Let its `UnresolvedCall` errors tell you which functions are
   missing.
2. **Use the 360tools harness** (`recomp_harness.py`) to harvest a
   cross-title import frequency table. It gives a ready-made stub-priority
   list based on what real games call most.
3. **Port the runtime's kernel/HLE to Android**:
   * Replace Windows-specific threading/sync with `pthread`/`std::thread`.
   * Replace Win32 file APIs with POSIX/Android asset-management calls rooted
     at the launcher's app-private `game/` directory.
   * Replace D3D12/Vulkan presentation with the GLES3 layer
     (`docs/07-GPU-XENOS-GLES3.md`).
   * Replace XAudio2/FFmpeg output with AAudio (API 26+) and OpenSL ES fallback.
4. **Fill BioShock-specific gaps** in `patches/` rather than editing
   `third_party/`.

The `360tools/templates/STUBS.md` playbook and `templates/overlay/src/stubs.cpp`
are the reference for step 3's "missing stub" shape.

## 5. Guest filesystem mapping

BioShock's guest code opens paths like `game:\ShockGame.u` or
`Sounds_Xenon\streams_0_audio.fsb`. The HLE file layer must translate those
to real paths under the app-private directory created by the launcher:

```
game:\ShockGame.u                    -> <filesDir>/game/ShockGame.u
game:\Sounds_Xenon\streams_0_audio.fsb -> <filesDir>/game/Sounds_Xenon/streams_0_audio.fsb
```

The current launcher extracts only `default.xex`. Before running the full
game, extend `Xdvdfs.kt` (or add a native extractor) to stream the whole
XDVDFS tree into `game/`. Keep this on-device extraction incremental and
report progress; the game tree is several GB.

## 6. Memory and endianness rules

* The Xenon is **big-endian**; Android ARM64 is little-endian. Generated load
  instructions byte-swap on read and store instructions swap on write. Any HLE
  handler that reads guest structures (e.g. `XINPUT_STATE`, `LARGE_INTEGER`)
  must do the same.
* Guest pointers are 32-bit. Always validate `ptr >= base` and
  `ptr + size <= base + image_size` before dereferencing. `hle::Kernel::call()`
  intentionally does not expose an unbounded guest-memory window — follow the
  bounded read/write pattern from `x360port`'s `RuntimeContext`.
* The indirect-call hash table lives past the end of the valid image region;
  never let guest allocations overlap it.

## 7. Testing strategy

1. Host self-test: import registration and fail-soft dispatch (`selftest.cpp`).
2. Unit tests per HLE family with synthetic guest memory (CRT string functions
   are a good start — easy to verify).
3. Boot-to-title-screen milestone: log every missing import during the first
   real run, then implement them in frequency order.
4. Keep a `docs/09-RE-FINDINGS.md` entry for each non-obvious override or
   patch so the reason survives later toolchain upgrades.

## 8. What is explicitly out of scope for HLE

* Xbox Live / XONLINE services. BioShock is single-player; stub these as
  "not signed in" and never contact a network.
* Achievements, marketplace, gamer pictures, sign-in UI — safe no-ops.
* Dashboard/XAM overlays — return defaults.
* Any Microsoft proprietary code. All handlers are clean-room reimplementations
  based on public documentation and the vendored open-source projects.
