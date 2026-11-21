# Status — verified facts, not aspirations

Last updated: 2026-11-21 (`v0.0.6.6-audio-load-fix-experimental` — audio liveness watchdog; Adreno/Windows unchanged).

**Open issues — see [`docs/20-KNOWN-ISSUES.md`](20-KNOWN-ISSUES.md)** for detailed,
evidence-backed diagnosis of the missing skybox/dark scene, the random audio death +
load stall, the 30 FPS cap, and the parked cutscene bug.

## `v0.0.6.6-audio-load-fix-experimental` — current release

**Root cause of intermittent load stalls found (audio-related).** The guest's level-load progress
is coupled to the AAudio callback chain:
`AAudio callback → on_frame_played() → APU semaphore credit → guest XAudio2 render callback`.
When Samsung's audio HAL MMAP path dies (`audio_hw_proxy2: get_mmap_position invalid time stamp`),
AAudio stops calling back, APU credits drain (~160 ms), the guest XAudio2 render callback stops,
and the guest's level-load wait hangs forever.

**Fix:** a liveness watchdog in `audio_system.cpp` pumps the mixer on a timer when no fill
callback has arrived for 100 ms, keeping `on_frame_played()` credits flowing so the guest keeps
loading even if the audio device dies. AAudio error codes are now logged.

LOW_LATENCY audio mode was **kept** — switching to `NONE` avoided the dropout but caused
stretching/crackling (the guest 256-frame callback misaligns with the buffer-queue burst).

**Open:** skybox (outdoor screenshots confirm a sky renders, so likely area-specific), 30 FPS
cap (user reports a guest-side options toggle that is off by default), Bathysphere cutscene (parked).

---

## `v0.0.6.5-perf-skybox-experimental` — previous release

## `v0.0.6.5-perf-skybox-experimental` — current release

Performance work + skybox investigation build.

**Performance changes:**
- **Perf Mode is now wired into the custom GLES3 renderer.** It was previously a hidden
  no-op (only applied to the Vulkan path). With Perf Mode ON (the default), the effective
  internal resolution steps down one tier (e.g. 540p→360p, ~0.44x pixels), the present
  filter becomes bilinear (1 tap vs Lanczos 9 taps), and anisotropy drops to 1. The
  launcher button is now visible on the custom path and the HUD RES pill shows the
  *effective* resolution. Turn Perf Mode OFF to restore the full requested preset.
- The present-stage letterbox clear is now scissored to only the black bars instead of
  clearing the whole window.

**Skybox diagnostics (one-shot logs, no rendering change):**
- `SKY-DIAG cube fetch:` logs every cube-texture fetch with shader hashes, slot, fetch
  instruction vs fetch-constant dimensions, texture format/size, and whether the 1x1
  white fallback texture was substituted — the leading hypothesis for the missing sky.
- `SHADER-PERF-DIAG interpreter-fallback shader:` counts shaders that fell back to the
  65536-step GLSL interpreter instead of structured code; if any hot shader uses it,
  that would explain low FPS on Mali.

Known issues still open: no skybox (root cause under investigation), GPU-bound FPS
(Perf Mode should improve it), Bathysphere projector cutscene bug (parked).

## `v0.0.6.4-mali-hotfix-experimental` — current release

Published as a GitHub release. Mali boots through the intro into controllable gameplay with
no major visual issues. Includes the Mali portability fixes, the vertex-explosion fix, the
CPU DXT decoder correctness fixes, and the launcher EXPORT LOGS button.

**Known issues (targeted for v0.0.6.5):**
- **No skybox** — the sky/background is missing in-game.
- **Low FPS / performance** — GPU-bound (~22–30 FPS on tested hardware); performance work
  is the next version.
- Bathysphere projector cutscene bug remains parked (separate issue).

## `v0.0.6-mali-fix-experimental` — Mali-G78 support (FIXED)

The final missing piece was **texture-unit aliasing**: the texture-buffer fallback bound
vertex-buffer sampler objects on units 0..N-1, which clobbered the PS sampler2D bindings
(also 0..15) on Mali. Moving vertex-buffer samplers to units 16+ (translator v6) fixed it —
the title screen and in-game textured geometry now render correctly on the SM-G998B.
Verified on device: title screen, main menu and in-game 3D render with no GL/shader errors.

Test device: SM-G998B (Exynos 2100, Mali-G78, GLES 3.2, Android 15).

Fixes shipped in this build:
- **Vertex shader compile errors**: Mali rejects passing `readonly` SSBO elements to helper
  functions (`S0001: Function call discards 'readonly' access qualifier`). The translator now
  stages `.length()` and each loaded word into plain locals before calling `bswap()` etc.
- **Vertex SSBOs unsupported**: Mali reports `GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS = 0`, so
  program link failed with "number of vertex shader storage blocks (1) is greater than the
  maximum allowed (0)". Added a texture-buffer fallback (`usamplerBuffer` + `texelFetch`) that
  activates automatically when that limit is 0. Vertex data is uploaded into a dedicated
  `GL_TEXTURE_BUFFER` from the guest pointer (`glCopyBufferSubData` and `glTexBufferRange`
  both fail on Mali's persistently-mapped ring).
- **CPU DXT1/3/5 decoder** added for GPUs without S3TC (`s3tc=0` on Mali), transcoding to RGBA8.
  Note: no DXT textures were observed in the tested scenes, so this path is currently untested.

**Resolved.** The earlier "textured content missing" symptom was caused by the
vertex-buffer texture samplers aliasing the PS sampler units; see the v6 fix above.

**Vertex-explosion regression resolved (Mali, 2026-11-20):** the fixed 256 KB texture
buffer made `textureSize()` return 65536, so the shader's `vbLen` clamp no longer matched
the real vertex data. Reverted to exactly-sized buffers, but kept the Mali crash fix by
allocating a **fresh buffer object** whenever the size changes (never resizing a buffer
still attached to a texture buffer). Verified on device: title screen and cinematics render
correctly with no crash and no explosions.

## Loading screen investigation (Mali) — status

**Correction (2026-11-20):** an earlier note claimed Mali "never gets past the loading
screen." That conclusion was an artifact of over-eager automated ADB input — repeated synthetic
A/START taps during the intro/cutscenes were interpreted as menu/quit and sent the game back
to the title/loading screen. The game itself does progress through the intro into gameplay.
No genuine renderer-side load stall was found: DXT CPU decodes were modest (~600 uploads over
several minutes), `VdSwap` advanced normally, and there were no GL/shader errors. Automated
device driving should be avoided when judging game-state transitions; rely on the user or on
passive logs instead.

The centre loading video alternates between video frames and the dark "BIOSHOCK" logo card —
this is the game's normal loading-screen behaviour. The `LOADING...` text is pixel-static.

Cutscene bug (Bathysphere projector) remains parked and is a separate issue.

**Launcher log export:** the launcher now has an `EXPORT LOGS` button that writes a
timestamped text file (device info + native renderer log + filtered logcat) to the app's
external files and opens the system share sheet (Discord is a valid target). Native log
lines are mirrored to `bs1-runtime.log` via the `BS1_LOG_FILE` environment variable, set
by JNI, because third-party apps cannot read logcat on Android.

## On-device diagnosis (2026-11-19, SM-A366B / Adreno 710, 540p): 22–26 FPS in Rapture

A live ADB session loaded the `CONTINUE` save straight into the Bathysphere/Rapture
transition at 540p and captured the `PERF(10s)` counters. The 22 FPS report is
**reproduced and characterized**:

- `swaps=251–263 / 10 s` (25–26 FPS); `pres=6.5–6.7 s` of every 10 s. The GPU worker
  is blocked in `eglSwapBuffers()` waiting for GPU completion — the port is **GPU-bound**,
  not guest-CPU-bound (`wait=3 µs` in-game). Not thermal (37.5 °C, charging).
- Each in-game frame issues ~**620 draws** and ~**53 resolves** (`color≈47, depth≈5`).
- A live A/B of the present filter (Lanczos → Bilinear, same scene) had **no measurable
  FPS benefit** (both ~22 FPS, `pres≈6.9 s`) — the upscale shader is not the bottleneck.
- The dominant resolve cost is now identified: `reject[s=~11000]` of ~12,000 color
  resolves per 10 s fail `glCopyImageSubData` because **`colorScale != 1.0`**
  (`rejectClamp=0`, `rejectBounds=0`). ~90% of EDRAM→host color resolves therefore take
  the FBO + fullscreen-triangle path. This is the highest-value target for a future
  fast-path that can apply the scale without a fullscreen pass.
- Consecutive-identical resolve deduplication was implemented and measured: `skip=0`
  over a full session, so no redundant resolves exist — the optimization is harmless
  but ineffective.

### Cutscene brown-overlay state capture (accurate, post-`bindStageTextures`)

`CUTSCENE-DIAG` logging (one shot per suspect shader hash) confirmed both suspect
passes execute in the very first Rapture frames:

- `ps=38fc8859e11347ce` (fog): `bc0=0x07060706` (src=DST_ALPHA, dst=1-DST_ALPHA),
  `dc=0x1c718233` (stencil EQUAL 0, depth LEQUAL, no depth write), `colorMask=0xF`,
  1 fetch: depth texture fmt=22, 1280×720, `adj=0xB40` = **(R, R, 1, 1)** — RRRR
  replication is correct, so the depth sample itself is not obviously wrong.
- `ps=10ee2aad4ad76b55` (spotlight): 3 fetches, two depth textures (1280×720 and
  512×512), `bc0=0x04000400`, `dc=0x1c704533`.

Next step: compare the stencil reference/mask (`0x00ffff00` → ref=0, read=0xFF, write=0xFF)
and the destination-alpha content of the preceding pass against Xenia's trace, since
the fog blend is driven by destination alpha and the overwrite is stencil-keyed.

### 2026-11-19 follow-up: Bathysphere overlay largely diagnosed, not yet fixed

With the user parked on the actual projector scene, one-shot diagnostics captured:

- The covering passes use `bc0=0x07060706`. Verified against Xenia `xenos.h`
  `BlendFactor`: 6 = `kSrcAlpha`, 7 = `kOneMinusSrcAlpha` (our `GlBlendFactor` mapping
  is correct). So these are **source-alpha** blends, not destination-alpha.
- There are **two** successive fullscreen passes with an identical signature
  (`bc0=0x07060706`, `dc=0x00700772` = no stencil / depth-test LEQUAL / no depth write,
  `colorControl=0x8700000c`, `colorInfo=0x00000000`):
  1. `ps=ac33d4b4095218ec` — `fetches=0`, translated GLSL is a pure vertex-colour
     pass-through (`vec4 r0 = v_o0; oC0 = max(r0, r0)`), with `oC0.w = 1`.
  2. `ps=ab16fa44d9676bcf` — `fetches=1`, samples a 512×512 `fmt=2` guest texture.
- Pixel readback immediately before pass (1) returned **alpha=255 across the whole
  line** while RGB varied (9–69), so the source alpha of these passes is what makes
  them opaque. The colour clear is `0x00000000` (alpha=0), so alpha=1 comes from the
  PS/vertex path, not the clear.
- A prototype that suppressed both matching passes compiled, deployed and was confirmed
  to trigger (`CUTSCENE-FIX` fired for both shader hashes), but the screen **remained
  brown** — meaning the remaining overlay comes from yet another pass with a different
  blend signature. The prototype was **reverted** because it removed scene fog with no
  visual benefit.

Conclusion: the brown sheet is not a single depth-fog pass; it is a stack of Xenos
fullscreen passes whose alpha handling differs from the hardware. Resolving it needs a
frame-by-frame draw comparison against Xenia rather than another single-pass guess.

### Xenia reference comparison (2026-11-19)

The user supplied four Xenia screenshots of the same projector sequence. They show a
**bright, glowing movie rectangle centred on screen** with only a dark brown surround.
On Android the movie is present but barely visible — so the movie is being **darkened
after it is drawn**, not simply covered by a static overlay.

On-device diagnostics of the movie shader (`ps=7c4ba4eecf8393e0`) show it is correct:
- Three fetches: Y at 1280×720, Cb and Cr at 640×360, all `fmt=2` (`k_8`) with swizzle
  `(R,R,R,1)` — the standard planar Bink layout.
- Translated GLSL is a textbook YUV→RGB conversion, and its pixel constants are exact
  BT.601 limited-range coefficients: `(1.1641, 1.5958, 0, -0.8707)`,
  `(1.1641, -0.8135, -0.3914, 0.5297)`, `(1.1641, 0, 2.0178, -1.0817)`, alpha `(1,0,0,1)`.
- Final exported alpha is `pc[3].w = 1.0`, so the movie quad itself is fully opaque.

A prototype that suppressed the deferred multiplicative spotlight pass (`bc0=0x04000400`,
`dst *= srcColor`) only in movie frames was built and, using a Crash Site save that
reliably reaches the lever, A/B tested on device. It made **no difference** (33 FPS and
the same dim movie before and after; the suppression log never fired, so that pass is
not even issued in movie frames). The hypothesis is disproven and the prototype was
reverted.

The next step is a full draw-list diff against Xenia for one movie frame: we have now
ruled out the depth-fog pass, the two fullscreen src-alpha passes, and the multiplicative
spotlight pass. The remaining candidates are the resolved movie texture's path into the
3D projector quad (no fetch of a 1280x720 colour resolve was observed in the scene) or
the compositing order of the bathysphere background.

### Xenia shader dump comparison (2026-11-19)

Copied the user's Xenia Canary install into `_work/xenia-canary-windows` (with saves,
config and `xenia.log`) and enabled its `dump_shaders` option. After the user replayed
the projector scene, 806 shader dumps were written to `_work/xenia-shaderdump`.

By re-implementing `Gles3Shaders::HashWords` (FNV with seed
`0x6142681be8a800f1` for translator tag `v3-deferred-resolve-clamp`) over each dumped
`.ucode.bin` file, every suspect GLES shader was mapped to its Xenia ucode:

| GLES hash | Role | Xenia dump |
|---|---|---|
| `7c4ba4eecf8393e0` | Bink YUV→RGB movie | `shader_98587DA49B1A05A4` |
| `ac33d4b4095218ec` | fullscreen vertex-colour pass | `shader_2E372EA28CC404B7` |
| `ab16fa44d9676bcf` | fullscreen textured pass | `shader_FA282553060F038E` |
| `e1ff0bc5a7c8ff2c` | deferred spotlight | `shader_232F7598AED463AF` |

Comparing the readable ucode with our translated GLSL shows **all four translate
correctly** — including the movie shader's `tfetch2D`×3 + `dp3`/`cndeq`/`dp4` YUV math
and the fullscreen passes' `max`/`mul`/`rcp`/`mad` sequences. The vertex-colour pass is
literally `max oC0, r0, r0` in both. Its colour comes from a `vfetch_mini r0, Offset=4`
of `FMT_32_32_32_32_FLOAT` in the VS (`shader_612A9A5369E002DA`), which our translator
also handles (format 38, inherited slot/stride, DWORD offset).

**Status as of 2026-11-20 (parked):** the cutscene overlay is still NOT fixed. Work paused
to focus on Mali GPU support. A trace-capable Xenia was built from source
(`XENIA_ENABLE_GPU_TRACE=ON`, compression disabled) and a 9.4 GB stream trace of the exact
movie frame was captured in `_work/xenia-trace/`, with 407 draws extracted by
`_work/extract_xenia_frame.py`. Finishing the draw-state diff needs mapping Xenia's internal
register-enum indices to our raw Xenos addresses — left for a future session. Shader
translation is definitively ruled out.

Xenia's release binary has **no GPU trace support** (both `trace_gpu_stream` and F4 log
"GPU trace capture isn't built into this configuration"), so a draw-list diff is not
possible with this binary. With shaders now ruled out, the remaining difference lies in
per-frame state or input data — most likely the vertex colour buffer for the fullscreen
pass or the resolved-movie sampling path. Shader translation is no longer suspect.

### Implemented: deferred scaled-resolve fast path (`v0.0.5.4` work)

`RB_COPY_DEST_INFO::copy_dest_exp_bias` is bits 16–21 (confirmed against Xenia's
`registers.h`), so the `colorScale != 1.0` rejections were real — Xenia also needs a
shader for biased resolves. We avoid that shader instead of accepting it:

- `glCopyImageSubData` is always format-compatible here because the resolved texture
  is allocated with the **source** GL format (`dstFmt = ct->internalFormat`).
- The resolve's exponent bias and clamp mode are stored on `Gles3Resources::Resolved`
  instead of being baked into the pixels. `bindStageTextures()` folds them into the
  existing `textureAdjustment[slot][2]` exponent and the new `[3]` clamp field, so
  every shader consumer sees the same value it did before.
- `ExecutePresent()` passes the bias through `PresentParams::w`; both present shaders
  apply `exp2(u_params.w)` before the final clamp, so frontbuffer resolves are fast too.
- `XenosGlsl::Version()` bumped to `v3-deferred-resolve-clamp` so stale `.glesbin`
  binaries (which lack the clamp branch) cannot be loaded. One-time shader recompile.

**Measured on-device:** color resolves using `glCopyImageSubData` went from ~10% to
**100%** (`direct == color`), resolve CPU time fell ~18% (665 ms → 544 ms per 10 s),
with no visual regressions. Overall FPS improved only modestly (~25 → ~27 in
comparable scenes): the remaining GPU cost is the ~600 Xenos scene draws per frame,
not resolves. The cutscene `CUTSCENE-DIAG` gate was switched from shader hash to the
distinctive blend control (`0x07060706` / `0x04000400`) so it survives the version bump.

## ✅ `v0.0.5.3-experimental` (Android & Windows): Audio Survival Fix, Ahead-of-Time Shader Pre-Warming & Hardware Write-Watch

- **Audio Survival Fix (`third_party/rexglue-git/src/kernel/apu.cpp`):** Fixed a semaphore credit leak in `APU::WorkerMain()` where silent guest callbacks (e.g. 2K intro → Title Screen or level-load transitions) returned without calling `SubmitFrame()`, permanently draining credits until audio deadlocked. Silent callbacks now pace by 5 ms and refund their credit so audio survives all transitions without stretching.
- **AAudio & Threading Tuning (`sdl3_audio_driver.cpp`, `main.cpp`, `MainActivity.kt`, `android_custom_driver.cpp`):** Restored `AAUDIO_PERFORMANCE_MODE_LOW_LATENCY` with 256-frame callback alignment, automatic sample-rate resampling for non-48 kHz devices, `--apu.max_queued_frames=32`, and `--gpu.custom.vsync=true`; removed `sustainedPerformanceMode` clock throttling and GPU thread `-8` priority starvation.
- **Ahead-of-Time Shader Pre-Warming (`gles3_shaders.cpp`):** `PrewarmCache()` pre-loads all cached `.glesbin` program binaries at startup (`prewarmed=741` in ~0.66 s on Adreno 710) and `Intern()` defers `glCompileShader()` until a binary cache miss occurs, eliminating repeat load-in shader stutters.
- **Hardware Write-Watch (`gles3_resources.cpp`):** Wired `Gles3Resources` to `rex::IPhysicalWriteListener` / `WatchPhysicalWrites` for textures, VBs, and IBs, plus CPU-overwrite invalidation for resolved render targets.
- **Known Issue (Bathysphere Projector Cutscene):** A 1-frame draw dump (`946` draws) confirmed the projector video renders in-scene, but sits behind a dark brown overlay — pointing to `DUMP#853` (fullscreen depth-sampled fog pass `ps=38fc8859e11347ce`, `bc0=0x07060706`, `dc=0x18701231`, `t0=f22:1280x720(R)`) or `DUMP#833` (deferred spotlight volume `ps=10ee2aad4ad76b55`). Tracked for `v0.0.5.4`.

## ✅ `v0.0.5-experimental` (Android & Windows): In-Game Playable, Save/Load, Audio, Custom GLES 3.2 & D3D11 Renderers

Both **Android (`arm64-v8a`)** and **Windows (`x86_64`)** run in-game through the real ReXGlue runtime and our custom Xenos GPU renderers (`renderer/`, `rexgpu-xenos-custom`):

- **Required Retail Dump:** Xbox 360 **World Release** (`Title ID: 545407D8`, `Media ID: 44E704F7`).
- **Community & Feedback Discord:** `https://discord.gg/aaDRJQNw8T`
- **Custom OpenGL ES 3.2 Xenos Renderer on Android (`renderer/src/gles3/`):**
  - Translates Xenos microcode shaders to GLSL ES 3.20 (`xenos_glsl.cpp`), manages EDRAM render targets and resolves (`gles3_backend.cpp`), and supports live no-restart resolution scaling (`360p`, `540p`, `720p`, `1080p`, `1440p`) via the in-game **RES** pill (defaulting to `540p` / `2x` anisotropy on fresh installs).
  - Replicates resolved depth across all four channels (`RRRR`) so spotlights, depth maps, and underwater fog sample valid depth values.
  - Fast row-copy `memcpy` path in `LinearizeLevel` for untiled Bink video frames (Andrew Ryan elevator screen & cutscenes) plus tiered texture fingerprint checking (every frame for dynamic/video textures, throttled for static compressed textures) and capped per-frame texture cache eviction.
  - GPU worker thread elevated to display priority (`PRIO_PROCESS` `-8`/`-4`) paired with Android `sustainedPerformanceMode` and `appCategory="game"`.
- **Custom D3D11 Xenos Renderer on Windows (`renderer/src/`):**
  - Live no-restart internal resolution scaling (`360p`–`2160p` via **F1**), live Lanczos2 / Bilinear present filter (**F2**), full live `gpu.custom.*` overlay (**F4**), async FIFO FXC compilation + disk shader cache, and strict D3D11 hazard/binding hygiene.
- **In-Game Save / Load Working (Android & Windows):**
  - Fixed `NtSetInformationFile` (`XFileRenameInformation`) in `xboxkrnl_io_info.cpp` to handle `ObDosDevices` (`0xFFFFFFFD`) in `root_dir_handle` and strip `\??\` prefixes.
  - Fixed Android SELinux `EACCES` (`0xC0000022`) in `host_file_posix.cpp` (`MoveNoReplace`) by bypassing hard `link()` on `__ANDROID__` in favor of `lstat()` + `rename()`, allowing `Save.tmp` → `DLContentInfo.pkg` and `.bsg` level saves to complete cleanly.
  - Extended `XamUserWriteProfileSettings` in `xam_user.cpp` to persist `kInt32`, `kInt64`, `kFloat`, and `kDouble` settings.
- **Native Android Audio (`libaaudio.so`):**
  - Replaced the `#if defined(__ANDROID__)` stub in `sdl3_audio_driver.cpp` with a dynamic `dlopen("libaaudio.so")` 48 kHz float32 stereo AAudio stream and enabled `needs.audio = true` in the Android entry point.
- **Full Xbox 360 Touch Controller (`TouchControlsView.kt`):**
  - Dual floating analog sticks (MOVE + LOOK), `LT`, `LB`, `RT`, `RB`, 4-way `D-Pad`, `A`, `B`, `X`, `Y`, `L3`, `R3`, `BACK`, `START`, plus top-bar `RES` and `HUD` buttons.

## ✅ GOAL ACHIEVED (Android): real BioShock title screen renders on-device

The genuine BioShock title screen renders on the SM-A366B through the real
ReXGlue runtime + Vulkan + VdSwap — guest-presented frames, not painted/faked.
Evidence: `_work/android_title_now.png` ("BIOSHOCK" sign + "PRESS START" +
animated underwater scene), `_work/android_title_final.png` (live fade),
`_work/capframes/frame_010.png` (guest legal text). Device stayed alive for
minutes at ~25 fps (`BS1-VDSWAP #2551`), presents via `vkQueuePresentKHR`, no
crash. Three real root-cause fixes: (1) glslang `createAccessChain` derived type
from unset internal state → id-0/invalid SPIR-V crashing Adreno — patched to use
the `base`/`offsets` parameters; (2) presenter surface never connected under
the headless SDL bootstrap (paint mode `kNone`, no presents) — added
`App::NotifySurfaceResized` called from JNI `nativeResize`; (3) host heap
corruption in `PosixThread::SetName` on the guest "VertexUploadThr" — guarded
the Android path. Full 492-file game tree on-device in app-private `files/game`;
game data stays out of the APK. See `32-GOAL-ANDROID-TITLE.md` (Completion) and
`19-WORKLOG.md`. `verify-all.py` 12/12 PASS.

## Android (historical): guest runtime launches on-device (no frame yet)

On the SM-A366B the real-runtime APK now boots: `Init OK` → `LaunchTitle OK` →
4 GB guest memory mapped via memfd at 0x100000000 → guest "Main Game Thread"
starts. Current blocker: guest thread SIGSEGV at guest-base+0x10 (null page).
See `32-GOAL-ANDROID-TITLE.md` part 4 and `19-WORKLOG.md` Session 15.

## 🎉 GOAL ACHIEVED (host): real BioShock title screen renders

The genuine BioShock title screen renders via the real ReXGlue runtime + D3D12.
Evidence: `_work/title_capture2.png` (1920x1080). Built the full ReXGlue v0.11.0
runtime from source with MinGW; `build/bs1-real/bioshock1.exe` (97 MB) links all
152 recompiled TUs + real runtime, boots through guest init (~30-40s), and
presents frames through VdSwap. Final fix: MinGW `lib`-prefix module-loader bug
caused duplicate kernel DLLs; fixed in `src/base/library_name.cpp`. See
`31-GOAL-TITLE-SCREEN.md` and `19-WORKLOG.md` Session 14. `verify-all.py`
12/12 PASS. Note: the on-device Android APK still shows a GLES3 diagnostic
pattern; porting the host's guest-rendered title to Android is the next phase.


## 1. Summary

| Area | State | Evidence |
|---|---|---|
| Game dump ingestion | ✅ working | ISO extracted (7,835,492,352 bytes), XDVDFS parsed, `default.xex` (13,021,184 bytes) extracted |
| XEX analysis | ✅ working | Title `0x545407D8`, XDK 5426, base `0x82000000`, 427 import records in `config/xex_analysis.json` |
| Config generation | ✅ working | `pipeline.py configure` produces `config/bioshock1.toml` with real values + explicit TODOs |
| Host native runtime | ✅ working | CMake/Ninja build + `bioshock1-selftest` passes |
| Android native library | ✅ working | `libbioshock1-native.so` (arm64-v8a) built by NDK CMake during APK assembly |
| Android launcher | ✅ working | Kotlin compiles; GLES3 surface, SAF import, full XDVDFS tree extract, touch overlay, JNI bridge |
| APK assembly | ✅ working | `app-debug.apk` built, ~9.6 MB, no game data inside |
| M0 device test | ✅ PASSED | Samsung SM-A366B / Android 16: install, JNI load, GLES3 shaders, animated pattern, touch overlay — see `24-DEVICE-M0.md` |
| Full codegen (PPC→C++) | ✅ working | Prebuilt ReXGlue v0.11.0 produced 308 files / 156 MB in 61 s from the real XEX; no warnings |
| Generated C++ compiles (host) | ✅ working | All 152 TUs compiled with clang++ 23 / C++23 → 152 .obj, 137.6 MB |
| Generated C++ compiles (arm64) | ✅ working | NDK r28c + libc++ compat shim compiled all 152 TUs to arm64 objects |
| HLE scope | ✅ measured | 216 real kernel/XAM imports from `llvm-nm -u`; full matrix in `21-HLE-IMPORT-MATRIX.md` |
| Import stub layer | ✅ working | `config/hle_imports.json` → 216 generated `__imp__*` stubs forwarding to name-based HLE dispatch; host selftest verifies |
| Link of generated arm64 .so | ✅ working | `libbs1_link_test.so` (139 MB) links all 152 generated objects + engine/HLE/stubs with `-Wl,-z,defs`, zero unresolved symbols |
| Guest entry point | ✅ measured | PE entry VA `0x8219D698`, mapped to generated `__imp__xstart` (no runtime stub needed) |
| Verification harness | ✅ 12/12 PASS | `scripts/verify-all.py` — reproducible consistency gate |
| Phase 3 PM4 parser | ✅ working | PM4 type 0/1/2/3 parser + register file + draw queue; synthetic buffer selftest passes; JNI submit API added |
| Guest context alignment | ✅ working | `BS_USE_REX_CONTEXT` selects real ReXGlue `PPCContext`; portable GPR/FPR/VMX accessors; arm64 `.so` relinks with zero undefined symbols |
| XDK ABI helpers | ✅ working | 236 `__rest*`/`__save*` helpers implemented from measured XDK code; round-trip selftest passes; `.so` links cleanly |
| Kernel/XAM imports | ✅ registered | all 216 imports have handlers (32 real, rest safe defaults); selftest verifies r3 ABI and named dispatch |
| Runtime symbols | ✅ implemented | all 9 non-ABI symbols real; `.so` 139.66 MB links with zero undefined symbols |
| Host boot path | ✅ real runtime built | **Full ReXGlue v0.11.0 runtime built from source (MinGW)**; `bioshock1.exe` (97 MB) links all 152 recompiled TUs + real runtime; **launches to a window with D3D12 device initialized** (see `31-GOAL-TITLE-SCREEN.md`). Blocker: cross-DLL kernel module `Connect()` not called. |
| First boot attempt (old HLE harness) | 📦 superseded | full 4 GB map + 256 MB guest thread + HLE registration; reached 2,545 HLE calls, no title screen. Kept as reference, superseded by real-runtime host build above |
| HLE kernel completeness | ✅ working | Full ReXGlue `xboxkrnl` + `xam` runtime linked; critical-section cross-thread unlock, `SaveGames:` container rename (`ObDosDevices` + Android `MoveNoReplace`), and profile settings persistence fixed |
| Custom Xenos game rendering | ✅ working | Custom D3D11 (Windows `360p`–`2160p`) and Custom OpenGL ES 3.2 (Android `360p`–`1440p`) Xenos renderers with live scaling, async shader cache, Bink fast-path, and `RRRR` depth resolve |
| Audio (XMA → AAudio / SDL3) | ✅ working | XMA hardware decoder active; Windows outputs via SDL3 audio, Android outputs via native 48 kHz float32 `libaaudio.so` driver |
| Playable game | ✅ experimental (`v0.0.5`) | Playable in-game on Windows & Android with audio, full Xbox 360 touch controls, cutscenes, and working Save / Load |

## 2. What "working" means here

The ✅ items were actually executed during setup:

```
python tools/pipeline.py inspect      -> title 0x545407D8, XDK 5426
python tools/pipeline.py configure    -> config/bioshock1.toml
python tools/pipeline.py build-native -> selftest OK, frames=10
cd android && .\gradlew.bat assembleDebug  -> BUILD SUCCESSFUL in 25s
```

The APK was inspected with `ZipFile`: the only binary payloads are
`lib/arm64-v8a/libbioshock1-native.so` and `libc++_shared.so`. The sole `.bin`
entry is Kotlin's `DebugProbesKt.bin`, **not** a BIOS. No `.iso`, `.xex`, `.fsb`
or `.u` files are packaged.

## 3. Known-correct details that were corrected during setup

Documented so they do not regress:

1. **Title ID decoding.** The two XEX "version" fields contain ASCII hex
   (`"5454"` + `"07D8"`). The naive numeric field at `+0x0C` (`0x82C36D80`) is
   *not* the title ID. `tools/xex_inspect.py` decodes it correctly.
2. **Import ordinals.** XEX import records are
   `(thunk_address, descriptor_address)` pairs; the ordinal lives inside the
   compressed PE. `tools/xex_inspect.py` reports raw pairs instead of inventing
   ordinal names, and the vendored `parse_xex_imports.py` is explicitly untrusted.
3. **Static-library header ID.** Header `0x000200FF` is `STATIC_LIBRARIES`;
   the vendored `xex_info.py` labels it `BASE_REFERENCE`. Our tool uses Xenia IDs.
4. **Gradle/JDK/NDK.** A stale `JAVA_HOME` and AGP's default NDK 26.1 were
   fixed with a fallback `gradlew.bat` and a pinned `ndkVersion`.

## 4. Current runtime status JSON (host self-test)

```json
{"initialized":true,"hleImports":8,"missingImports":1,"frames":10,"draws":0,"textures":0}
```

`missingImports: 1` is expected in the self-test — it deliberately calls an
unregistered ordinal (9999) to verify fail-soft behaviour.

## 5. Immediate next actions (in order)

1. Finish the Android arm64 compile check with NDK r28c (NDK 26 lacks
   `std::atomic_ref` required by ReXGlue v0.11 headers).
2. Copy the validated generated subset into `recomp/generated/` and add it to
   the Android CMake target; align our `PPCContext` with ReXGlue's real layout.
3. Inventory the generated import shims (`bioshock1_register.cpp`) and bind
   them to `hle::Kernel::call()`; record its exact ABI in `09-RE-FINDINGS.md`.
4. Implement the HLE handlers by import frequency (memory, threads, file I/O,
   XInput) and the PM4→GLES3 path to first geometry (M1/M2).
5. Verify the full-tree extraction and touch overlay on a device, then wire
   face buttons to the guest XINPUT state (`HookXInputState`).

**Phase 1 gate update:** the missing *tool* problem is solved by the prebuilt
v0.11 SDK (`scripts/install-rexglue.ps1`); codegen itself succeeded on the first
run with no manual analysis hints. This is a much better starting position than
expected for a 2007 retail title.

## 6. What will NOT change without a deliberate decision

* Launcher remains "not embedded" — no game data in the APK.
* Renderer target remains OpenGLES3 as requested, not Vulkan.
* No proprietary Microsoft code, BIOS or game assets are committed.
* `third_party/` stays pristine; modifications go into `patches/`.

## 7. Risk register

| Risk | Impact | Mitigation |
|---|---|---|
| Static-linked XDK inline calls can't be HLE'd | boot failures with no import name | per-title patches in `patches/`; byte-pattern hooks |
| XenonRecomp jump-table patterns differ for 2007 MSVC | wrong switch dispatch | extend detector; manual TOML entries |
| XMA audio decoder unavailable on Android | no sound, possible boot hang | FFmpeg arm64 build; stub audio device initially |
| GLES3 lacks extension for BC textures | visual corruption | detile + transcode to RGBA8/ASTC |
| Large codegen output → slow NDK compile | long iteration | enable optimisations only after baseline boots |
| BIOS/XDK symbols not legally usable | naming gaps | derive only facts, never ship symbol files |
