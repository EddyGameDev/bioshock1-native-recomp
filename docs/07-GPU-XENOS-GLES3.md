# GPU: Xenos → OpenGLES 3.0

This is the largest single piece of remaining work. This document records the
design, why GLES3 was chosen over Vulkan for this port, and the concrete plan.

## 1. What the device currently does

`native/src/gpu_layer.cpp` implements a working, verifiable GLES3 pipeline:

* GLES 3.0 (`#version 300 es`) vertex + fragment shaders.
* A fullscreen triangle generated from `gl_VertexID` (no vertex buffer needed).
* Uniforms for time and resolution, producing a diagnostic animated pattern.
* `upload_texture()` stub for BCn/ASTC data with frame/draw/texture counters.
* `present()` called every frame from the JNI/GLSurfaceView loop.

This proves: the Android EGL context is created correctly, shaders compile on
device, the frame loop runs, and the launcher-to-native path works. It is
**not** the game renderer — see `10-STATUS.md`.

## 2. Why GLES3 instead of the vendored Vulkan paths

The two vendored Android-capable graphics stacks both target Vulkan:

* `Xbox360RecompAndroid` uses a Xenos→Vulkan layer (and is a dynamic JIT,
  Phase 2-5 incomplete).
* ReXGlue ships D3D12 and Vulkan backends derived from Xenia.

Your reference ports, however, are explicitly **OpenGLES3** Android ports, and
you asked for OpenGLES3. That decision has consequences:

| Aspect | Vulkan (upstream) | GLES3 (this port) |
|---|---|---|
| Shader translation target | SPIR-V / DXIL | GLSL ES 3.00 |
| Descriptor management | explicit | implicit uniform bindings |
| Xenos EDRAM emulation | render passes / subpasses | FBOs + `glBlitFramebuffer`, or tiled render to texture |
| Bindless textures | descriptor indexing | texture arrays / sampler arrays / binding loops |
| Driver control | high (Adreno custom drivers) | lower, but much simpler packaging |
| Porting effort from Xenia code | moderate | substantial |

The upside: GLES3 is universally available on Android 9+ arm64 devices, needs
no custom driver installation, and matches the reference-port architecture.

## 3. The two rendering strategies

### A. Command-buffer emulation (Xenia-style)

Parse Xenos **PM4** command buffers emitted by the game and translate each
packet to GLES3 state/draw calls. Advantages: generic across games; ReXGlue and
Xenia contain the parser you would port. Disadvantages: you must emulate the
Xenos fixed-function state machine, EDRAM tiling, and shader microcode.

### B. API-level translation (UnleashedRecomp-style)

Patch the game's renderer at known addresses (mid-asm hooks) so it calls your
native `DrawIndexed`/`SetRenderTarget` functions directly with high-level
parameters. Advantages: fast, simple GLES3 state handling. Disadvantage:
**game-specific** — every title needs its own reverse engineering.

`XenosRecomp` (vendored) is built around strategy B for Sonic Unleashed. Its
README is explicit that the shader container, constants, vertex fetch and
instruction coverage are tailored to that game. For BioShock it is a **design
reference**, not a finished tool. A hybrid is the pragmatic path: use ReXGlue's
Xenia-derived PM4 parser as the base, and use XenosRecomp's shader translator
concepts (constant buffers, vertex declarations, cube instructions) where
BioShock's shader binaries permit.

## 4. Work breakdown

### 4.1 PM4 command stream

1. Port/enable the command-buffer parser from ReXGlue's Vulkan backend.
2. Map Xenos registers (the `0x2000_0000`-range MMIO registers) to GLES state:
   viewport, scissor, blend, depth/stencil, rasteriser, vertex declarations.
3. Translate draw packets (`SET_DRAW_INITIALIZE`, `DRAW_INDEXED`, etc.) to
   `glDrawElements*` with the correct primitive topology and index type.
4. Handle the `VdSwap`/present path: the game's `xboxkrnl.exe` import 430
   (`VdSwap`) flips the front buffer. Hook it to `gpu::present()`.

### 4.2 EDRAM

The Xenos keeps render targets in ~10 MB of on-chip EDRAM, tiled into large
rectangles. On GLES3 there is no exposed EDRAM, so:

* Allocate a set of OpenGL textures as "EDRAM surfaces".
* Resolve/untile when the game calls a resolve operation, using a fullscreen
  blit shader that converts the Xenon tiled layout to linear GLES layout.
* Track render-target binds so a resolve knows which surface to flush.

The vendored `rexdex` recompiler implemented a "very simple EDRAM simulator";
read its README section on EDRAM for the basic model before designing the GLES
variant.

### 4.3 Shaders

Xenos shader binaries are microcode, not HLSL. Path:

1. Detect shader binaries in the extracted game data / XEX (XenosRecomp's
   directory scanner does this via XXH3 hashing).
2. Use XenosRecomp to produce GLSL ES 3.00 (it currently emits HLSL; the
   translation layer must be adapted for GLSL output). Key adaptations:
   * root/push constants → uniform blocks or plain uniforms;
   * `Texture2D`/`TextureCube` → `sampler2D`/`samplerCube` with GLSL syntax;
   * control flow via `while(pc) switch(...)` → GLSL loops;
   * specialization constants → `#define` variants baked at codegen time;
   * vertex fetch → GLES vertex attribute declarations built from the game's
     vertex declaration (XenosRecomp currently hardcodes Vulkan locations).
3. Cache compiled programs by hash; compile asynchronously to avoid hitches.

`R11G11B10` normals, 16-bit texcoord swizzling (`g_SwappedTexcoords`), alpha
test and instanced draws all need explicit handling as documented in
`XenosRecomp/README.md`.

### 4.4 Textures

Xbox 360 textures use tiled/swizzled layouts and BC1-BC3 (and sometimes BC4/5)
compression. GLES3 on Android supports `GL_COMPRESSED_RGBA_S3TC_DXT1_EXT` etc.
via `EXT_texture_compression_s3tc` on many GPUs, but not universally:

* Preferred: detile + transcode to ASTC or RGBA8 at load time (slower, universal).
* Fast path: upload BCn directly when the extension is advertised.

`gpu::upload_texture()` is where this logic belongs; it currently accepts raw
RGBA bytes and returns a GL texture name.

### 4.5 Audio (separate but related)

BioShock's audio lives in FMOD `.fsb` banks. The XMA2 decoder must be ported
from ReXGlue's FFmpeg-based path to Android (FFmpeg build for arm64, or
platform `MediaCodec` where possible), with AAudio output. This is tracked in
`11-ROADMAP.md` phase 4 and is a boot blocker only if the game waits on audio
initialisation — many games do.

## 5. Verification milestones

| Milestone | What you see |
|---|---|
| M0 (current) | animated diagnostic pattern, counters in status JSON |
| M1 | PM4 parser traces command buffers without drawing (log packet names) |
| M2 | first textured triangle from a real Xenos draw call |
| M3 | title screen / menu renders |
| M4 | in-game geometry with correct shaders and textures |
| M5 | full game with audio and input |

Never skip M1/M2: a PM4 trace is the fastest way to find a wrong register
mapping before debugging "black screen" symptoms.

## 6. Files involved

| File | Role |
|---|---|
| `native/include/gpu_layer.h` | Public GPU interface (`initialize`, `resize`, `present`, `upload_texture`, `stats`) |
| `native/src/gpu_layer.cpp` | Current GLES3 diagnostic implementation; future PM4/shader/texture code |
| `native/src/engine.cpp` | Calls `gpu::present()` each frame |
| `android/.../GameSurfaceView.kt` | Owns the EGL3 context and GL thread |
| `third_party/XenosRecomp/` | Shader translator reference |
| `third_party/rexglue-sdk/` | Xenia-derived command buffer + texture code to port |
| `third_party/Xbox360RecompAndroid/native/` | Another Xenos→Vulkan implementation to study |
