# Glossary

| Term | Meaning in this project |
|---|---|
| **XEX / XEX2** | Xbox 360 executable container. Encrypted/compressed PE plus headers. Magic `XEX2`. |
| **XDVDFS** | Xbox DVD File System; the B-tree filesystem on XGD1/XGD2 discs. |
| **XGD2** | Second-generation Xbox 360 disc format. Game partition at `0x0FD90000`. |
| **PDI** | "Decompiled image" term used by the old rexdex recompiler for the unpacked XEX payload. |
| **Xenon** | The Xbox 360 CPU (3-core PowerPC, big-endian, 128 VMX registers). |
| **Xenos** | The Xbox 360 GPU (ATI/AMD R600-derived, unified shaders, ~10 MB EDRAM). |
| **VMX128** | Xenon's 128-vector-register SIMD extension; mapped to NEON/SIMDE on ARM64. |
| **PM4** | Command-buffer format used to drive the Xenos GPU. |
| **EDRAM** | On-chip embedded DRAM used as render target storage on the Xenos. |
| **HLE** | High-Level Emulation: reimplementing an API natively instead of executing its original code. |
| **LLE** | Low-Level Emulation: executing the original code (CPU/kernel) by interpretation/JIT. |
| **Static recompilation** | Translating guest machine code into native source code ahead of time, once. |
| **Dynarec / JIT** | Translating guest code to host code at runtime. Not used by our final design. |
| **Basic block** | Straight-line code ending in a branch; the unit most PPC→C++ recompilers translate. |
| **Thunk** | Small stub in the guest image that jumps to an imported function; patched by HLE. |
| **Ordinal** | Numeric import identifier (e.g. `XInputGetState = 641`); stored in the compressed PE descriptors. |
| **Cross-module import** | API imported from `xboxkrnl.exe` or `xam.xex`. |
| **Jump table** | Compiler-generated switch dispatch via `mtctr`/`bctr`; hard to recover statically. |
| **ABI helper** | Compiler-emitted `__restgprlr*`/`__savevmx*` routines for non-volatile registers. |
| **Mid-asm hook** | ReXGlue/XenonRecomp feature: call native code at a guest instruction address. |
| **Manifest** | ReXGlue's project model describing the image, imports, hooks and patches. |
| **ReXGlue** | Primary vendored static recompiler + runtime (XSP). |
| **XenonRecomp / XenonAnalyse** | Alternative PPC→C++ recompiler and its analysis tool. |
| **XenosRecomp** | Shader microcode → HLSL translator (reference for a GLES3 backend). |
| **XDK** | Xbox Development Kit. Version numbers (e.g. 5426) identify linked XDK libraries. |
| **XAM** | Xbox Application Manager (`xam.xex`): UI, profiles, content, input helpers. |
| **XMA / XMA2** | Xbox audio codec; needs a decoder (FFmpeg) in the runtime. |
| **FSB** | FMOD sound-bank container used by BioShock's audio. |
| **GLES3** | OpenGL ES 3.0, the requested Android rendering target. |
| **EGL** | Native platform interface used to create the GLES3 context. |
| **arm64-v8a** | 64-bit ARM Android ABI; the only ABI this project targets. |
| **AAudio** | Modern Android low-latency audio API (API 26+). |
| **SAF** | Storage Access Framework; `ACTION_OPEN_DOCUMENT` used for ISO import. |
| **Launcher, not embed** | Design where the APK holds no game data; user imports their own ISO at runtime. |
| **Guest memory** | The 512 MB native buffer emulating the Xenon address space, based at `0x82000000`. |
| **Base address** | Guest address at which the XEX image is mapped (`0x82000000` for this game). |
| **Fail-soft** | Returning a safe default for an unimplemented import instead of crashing. |
