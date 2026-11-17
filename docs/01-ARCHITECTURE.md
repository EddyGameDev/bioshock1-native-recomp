# Architecture

## 1. The big picture

```
                    BUILD MACHINE (Windows host)                      ANDROID DEVICE
 ┌────────────┐   ┌──────────────────────┐   ┌──────────────┐      ┌────────────────────────┐
 │ game.iso   │──▶│ xdvdfs.py            │   │ ReXGlue      │      │ Kotlin launcher        │
 │ (yours)    │   │ -> default.xex       │──▶│ codegen      │      │  - SAF ISO import      │
 └────────────┘   │ -> xex_analysis.json │   │ (or          │      │  - XDVDFS extract      │
                  └──────────────────────┘   │  XenonRecomp)│      │  - GLES3 surface       │
                             │               └──────┬───────┘      └───────────┬────────────┘
                             ▼                      ▼                          │ JNI
                  config/bioshock1.toml      recomp/generated/*.cpp            ▼
                                                    │                 ┌────────────────────────┐
                                                    └────────────────▶│ libbioshock1-native.so │
                                                       Clang/NDK      │  PPC context           │
                                                                      │  HLE kernel (Xboxkrnl) │
                                                                      │  GLES3 GPU layer       │
                                                                      │  AUDIO (XMA) - TODO    │
                                                                      └────────────────────────┘
```

Two distinct worlds:

* **Host (offline):** decrypt/decompress the XEX, analyse function boundaries,
  translate every PPC basic block into C++, compile it. This runs once per
  game build.
* **Device (online):** the generated C++ is just code. It needs a runtime that
  implements the Xbox 360 kernel APIs (file, thread, memory, input, audio) and
  a graphics layer that turns Xenos GPU commands into something GLES3 can do.

That split is exactly the one described in the ReXGlue SDK README and the
`rexglue-knowledges` overviews vendored under `third_party/`.

## 2. Why static recompilation (and why it is hard here)

PowerPC → C++ translation is *block-local*: each basic block becomes a C++
function that reads/writes a register struct. The good news:

* The translated code runs at native speed — no bytecode dispatch per instruction.
* Endianness is handled in the generated loads/stores (the Xenon is big-endian).
* Clang can optimise the result, especially with the ABI-based "local register"
  optimisations described in XenonRecomp's README.

The hard parts, all of which live in the **runtime**, not the translator:

1. **Kernel HLE.** The XEX imports ~427 ordinal slots across `xboxkrnl.exe`
   and `xam.xex` (see `03-XEX-ANALYSIS.md`). Each needs a native reimplementation.
   ReXGlue ships most of this, but its runtime targets Windows/D3D12/Vulkan.
2. **GPU.** The Xenos is a unified shader GPU with EDRAM. Two strategies exist:
   * *Emulate* PM4 command buffers (Xenia-style) — accurate, expensive.
   * *Translate* the renderer at the API level (UnleashedRecomp-style) — fast,
     but game-specific. XenosRecomp, vendored here, generates HLSL from shader
     microcode and is written around Sonic Unleashed; it is a **starting point**,
     not a drop-in solution for BioShock.
3. **Analysis quality.** Indirect calls, jump tables (`mtctr`/`bctr` idiom),
   RTTI/vtables and exception data must be resolved before codegen or the
   output will not boot. ReXGlue v0.8.0 automates much of this; XenonRecomp
   needs manual TOML hints.
4. **Content formats.** BioShock stores its data in Unreal packages (`.u`),
   FMOD sound banks (`.fsb`), and Xenon-specific containers. The runtime must
   expose them through the guest filesystem API with correct paths.

## 3. Layering in this repository

### Host tools (`tools/`)

| File | Responsibility |
|---|---|
| `xdvdfs.py` | Locate XGD2 game partition, parse XDVDFS B-tree, extract files |
| `xex_inspect.py` | Parse cleartext XEX2 headers: title, XDK build, imports, image base |
| `configure`/`pipeline.py` | Generate the recompiler TOML from the analysis JSON |
| `vendor_extract.py` | Reproducibly unpack the 12 toolchain archives |

### Native runtime (`native/`)

| Layer | Files | Current state |
|---|---|---|
| Guest CPU state | `include/ppc_context.h` | Complete struct definition (32 GPR, 32 FPR, 128 VMX, CR/LR/CTR/XER/MSR) |
| HLE kernel | `include/hle_kernel.h`, `src/hle_kernel.cpp` | Dispatch table + 8 bootstrap imports; 512 MB guest memory; missing-import tracking |
| GPU | `include/gpu_layer.h`, `src/gpu_layer.cpp` | Working GLES3 test-pattern pipeline, texture upload stub, frame stats |
| Engine API | `include/engine.h`, `src/engine.cpp` | Flat C ABI used by JNI; frame loop and control state |
| JNI | `src/jni_bridge.cpp` | Full bridge to Kotlin launcher |
| Codegen output | `recomp/generated/` | Empty until host recompiler runs |

### Launcher (`android/`)

A Kotlin `Activity` plus:

* `Xdvdfs.kt` — on-device mirror of the host ISO reader; extracts `default.xex`
  and the full game tree into app-private storage.
* `GameSurfaceView.kt` — a GLES3 `GLSurfaceView`; initialises the native engine
  when the context is ready and drives `nativeRender()` on the GL thread.
* `NativeBridge.kt` — JNI declarations, `System.loadLibrary`.

No `.iso`, `.xex`, or BIOS bytes are compiled into the APK. This matches the
"launcher not embed" pattern from the MGS3 reference project.

## 4. Memory model

The generated PPC code assumes a 32-bit guest address space mapped at the
original XEX base. For this game that base is **`0x82000000`** (measured from
the actual XEX). The runtime allocates a 512 MB byte buffer and maps the
decompressed image at `base`. Two consequences:

* Every guest pointer is a 32-bit offset into this buffer; host code must
  bounds-check it (`hle::Kernel::map_image` already does).
* The guest **register context** has two layouts: a compact one for the
  standalone launcher/selftest and ReXGlue's real `struct PPCContext` for the
  generated-code build (`BS_USE_REX_CONTEXT`). Runtime code must use the
  `gpr_read`/`gpr_write` accessors so it works in both modes — see
  `20-REXGLUE-V011.md` §3e.
* The recompiler's indirect-call hash table lives *after* the valid image
  region, as described in XenonRecomp's README. The TOML generator reserves
  the base address for this reason.

## 5. Data flow at runtime (target design)

1. Launcher extracts `default.xex` and the game tree into app-private storage.
2. `bs_engine_init()` initialises HLE, maps the image, creates the GLES3 context.
3. Generated `sub_XXXXXXXX(ctx, base)` entry point starts the guest CPU.
4. Guest calls into XBOXKRNL/XAM ordinals → HLE handlers.
5. Guest submits Xenos command buffers → GPU layer → GLES3 draw calls.
6. Audio requests → XMA decoder (FFmpeg) → AAudio/OpenSL ES. **Not implemented.**
7. Input bridge writes into the guest XINPUT state structure.

## 6. Design decisions inherited from the reference ports

* **Launcher, not embedded** (MGS3 pattern): fonts/BIOS/ISO never ship.
* **Host-only codegen** (all reference ports): the device never recompiles.
* **Patches directory** (MGS3 pattern): runtime modifications are overlays on
  the vendored upstream toolchain, keeping `third_party/` pristine.
* **Deterministic configs** (GBA/PS2 ports): JSON/TOML generated from the game
  image, checked in as text, so a rebuild is reproducible.
