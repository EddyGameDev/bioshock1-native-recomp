# Work log — incremental progress toward a playable port

This is the running journal. Each session adds a dated entry with what was
attempted, what worked, what failed, and the exact next action. `10-STATUS.md`
holds the current snapshot; this file holds the history.

---

## Session 25 — 2026-11-20 — v0.0.6.4 Mali Hotfix release

- User confirmed the game reaches controllable gameplay with no major visual issues.
- Published GitHub release `v0.0.6.4-mali-hotfix-experimental` with the hotfix APK.
- Documented remaining issues for v0.0.6.5: no skybox, low FPS/performance (GPU-bound).
- Bathysphere projector cutscene bug remains parked.

---

## Session 23/24 — 2026-11-20 — Loading-screen "blocker" retracted

- **Retraction:** the earlier "Mali never gets past the loading screen" conclusion was wrong.
  It came from over-eager automated ADB input — repeated synthetic A/START taps during the
  intro/cutscenes were read as menu/quit and returned the game to the title/loading screen.
  The user confirmed the game was in-game and my live driving sent it back. Corrected
  `10-STATUS.md` and the GitHub release notes to remove the false blocker claim.
- Passive diagnostics found no genuine renderer stall: `VdSwap` advanced normally, DXT CPU
  decodes were modest (~600 uploads over minutes), no GL/shader errors. Lesson: do not judge
  game-state transitions from automated device driving.
- Legitimate DXT decoder correctness fixes were kept (DXT3 alpha out-of-bounds write and
  alpha indexing, packed-mip block offset). Debug DXT counters were removed.
- Version bumped to `0.0.6.3-mali-loading-fix-experimental`.

---

## Session 22 — 2026-11-20 — Vertex-explosion fix + logs-request release

- Reverted fixed 256 KB texture buffer (broke shader `vbLen` clamp -> giant triangles);
  kept Mali SIGSEGV fix via fresh buffer object on every size change. Verified title and
  cinematics clean. Committed `5bb110d`.
- Published GitHub release `v0.0.6-mali-fix-experimental` — "Player Logs Requested" — with
  EXPORT LOGS instructions and the refreshed APK.

---

## Session 21 — 2026-11-20 — Log export + Mali crash hardening

- Added `EXPORT LOGS` button to the launcher (layout, FileProvider, share intent with
  `setClipData` so the share sheet/Discord can read the file).
- Third-party apps cannot read logcat, so native renderer logs are mirrored to
  `bs1-runtime.log` (set via JNI `nativeSetLogFile` -> `BS1_LOG_FILE` env var, read by the
  GLES3 log macros). Verified: exported file contains 95 lines of real renderer logs.
- Release build marked `debuggable true` (experimental) — no longer needed for logs, but
  kept for diagnostics.
- Hardened Mali texture-buffer path with a fixed 256 KB buffer to stop a driver SIGSEGV
  (`gles_texturep_prepare_texture_buffers`). Crash gone, but in-game geometry now shows
  vertex explosions — documented as a known regression, still being investigated.

---

## Session 20 — 2026-11-20 — Mali-G78 portability pass (partial)

Device: SM-G998B / Mali-G78 / GLES 3.2 / Android 15. Reported symptom: audio only, no visuals.

- Found two hard failures in logcat: `CompileShader(VS_...) failed: S0001: Function call
  discards 'readonly' access qualifier` and
  `LinkProgram(...) failed: vertex shader storage blocks (1) > maximum allowed (0)`.
- Fixed shader compilation by staging readonly SSBO elements into locals (translator v4).
- Added automatic texture-buffer fallback for VS vertex fetch when
  `GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS == 0` (translator v5): `usamplerBuffer` + `texelFetch`
  with `GL_R32UI`; backend uploads guest vertex bytes to a dedicated GL_TEXTURE_BUFFER.
  `glTexBufferRange` and `glCopyBufferSubData` both fail on Mali, so a plain buffer + CPU
  source pointer is used.
- Added CPU DXT1/3/5 -> RGBA8 decoder for `hasS3tc == false` (untested in practice).
- **Root cause of missing textures found:** the texture-buffer fallback bound vertex-buffer
  samplers on units 0..N-1, clobbering the PS sampler2D bindings (0..15) on Mali. Moved
  vertex-buffer samplers to units 16+ (translator v6). Verified on device: title screen,
  main menu and in-game 3D all render correctly with no GL/shader errors. Released as
  `v0.0.6-mali-fix-experimental` (still an experimental release, per project convention).

---

## Session 19 — 2026-11-19 — On-device 540p perf & cutscene shader-state capture (ADB)

**Goal:** reproduce the reported 22 FPS drop in Rapture, identify the GPU bottleneck,
and capture the exact GL state of the two shaders implicated in the Bathysphere
projector brown overlay. **Status: perf bottleneck characterized; cutscene shader
state captured; no visual regression.**

Done:
  - Connected to SM-A366B via ADB (`com.bioshock1.recomp`, v0.0.5.3-experimental)
    and loaded the `CONTINUE` save at 540p (RES pill), reproducing 22–26 FPS.
  - Added `PERF(10s)` resolve breakdown counters (`color/depth/direct/skip/reject[s/c/b]/px`)
    to `gles3_backend.{h,cpp}` and `android_custom_driver.cpp`; rebuilt
    `librexgpu-xenos-custom.so`, repackaged `app-release.apk`, installed with `adb install -r`.
  - Added one-shot `CUTSCENE-DIAG` logging for `ps=38fc8859e11347ce` and
    `ps=10ee2aad4ad76b55`, placed **after** `bindStageTextures()` so
    `textureAdjustment[]` is current (an earlier placement logged stale values).
  - Measurements: GPU-bound (`pres≈6.6 s/10 s`), ~620 draws + ~53 resolves per frame,
    Lanczos vs Bilinear A/B showed no gain, ~90% of color resolves rejected from
    `glCopyImageSubData` due to `colorScale != 1.0`, no thermal throttling.
  - Implemented + measured consecutive-identical resolve dedup (`skip=0`, no effect).
  - Captured both suspect shaders' blend/depth/stencil/fetch state (see `10-STATUS.md`).
  - **Bathysphere overlay deep-dive (with user on the projector scene):** dumped the
    covering shader GLSL (pure vertex-colour pass-through, `oC0.w=1`), read back the
    destination RGBA (alpha=255 with varying RGB), verified the Xenos blend-factor
    enum against Xenia (`6=kSrcAlpha`, `7=kOneMinusSrcAlpha`), and found two stacked
    fullscreen passes with identical blend/depth/alpha-test state. A prototype that
    suppressed both was deployed and confirmed to trigger but did **not** remove the
    brown sheet, so it was reverted; the remaining overlay comes from another pass.
  - **Xenia reference frames supplied by user:** the projector movie is bright on
    Xenia, so Android is darkening it post-draw rather than hiding it. Verified the
    movie shader fetches (Y 1280x720 + Cb/Cr 640x360, fmt=2, swizzle RRR1), its GLSL
    YUV->RGB math, and its BT.601 pixel constants including alpha=1 — all correct.
    A multiplicative-spotlight suppression prototype was reverted because automated
    navigation loaded a newer autosave past the scene before it could be evaluated.
  - **Xenia shader dump comparison:** copied the user's working Xenia install to
    `_work/xenia-canary-windows`, enabled `dump_shaders`, backed up its shader cache,
    had the user replay the projector scene, captured 806 dumps in
    `_work/xenia-shaderdump`. Re-implemented our `HashWords` over the dumped microcode
    to map all four suspect GLES shaders to their Xenia ucode and confirmed every one
    translates correctly (movie YUV math, fullscreen passes, spotlight). Xenia's release
    binary cannot emit GPU traces (streaming and F4 both blocked), so shader
    translation is ruled out and the remaining fault is in per-frame state/input data.
  - **Implemented deferred scaled-resolve fast path:** verified `copy_dest_exp_bias`
    layout against Xenia `registers.h`; added `expBias`/`clampMode` to
    `Gles3Resources::Resolved`; `ExecuteResolve` now uses `glCopyImageSubData` for
    *all* color resolves and defers the bias/clamp to sample time;
    `bindStageTextures()` folds them into `textureAdjustment[].z/.w`;
    `ExecutePresent()` passes the bias via `PresentParams::w` and both present shaders
    apply `exp2(u_params.w)`; bumped `XenosGlsl::Version()` to v3 to invalidate stale
    binaries. Built, installed and A/B tested on device: `direct == color` (100%),
    resolve time 665→544 ms/10 s, no visual regressions, modest FPS gain (~25→~27).

---

## Session 18 — 2026-11-19 — `v0.0.5.3-experimental` (Audio Credit-Leak Fix, Ahead-of-Time Shader Pre-Warming, Write-Watch & Cutscene Frame Dump)

**Goal:** eliminate repeat load-in shader stutters on Android, fix audio stretching and audio dropout after the 2K intros, and diagnose the Bathysphere projector cutscene brown overlay.
**Status: ACHIEVED for shader pre-warming & audio (`v0.0.5.3-experimental`); cutscene brown overlay root-cause narrowed via 1-frame draw dump.**

Done:
  - **Audio Survival Fix (`third_party/rexglue-git/src/kernel/apu.h`, `apu.cpp`):**
    Found why audio played during the 2K intros and cut out permanently at the Title Screen: `APU::WorkerMain()` consumed a semaphore credit per guest callback, and `AudioSystem` only released credits inside `on_frame_played()` when a submitted frame played. During silent transitions where the guest callback returned without calling `SubmitFrame()`, credits leaked until the semaphore hit 0 and deadlocked forever. Added per-client `frames_submitted` tracking; silent callbacks now sleep 5 ms (one Xbox 360 audio period) and refund their credit. Verified on SM-A366B: audio now survives 2K intros -> Title Screen -> Gameplay cleanly with no stretching.
  - **AAudio & Scheduling Fixes (`sdl3_audio_driver.cpp`, `main.cpp`, `MainActivity.kt`, `android_custom_driver.cpp`):**
    - Removed `window.setSustainedPerformanceMode(true)` (which capped CPU/GPU clocks) and removed `setpriority(PRIO_PROCESS, 0, -8)` on the GPU worker thread (which starved the guest XMA audio thread).
    - Restored `AAUDIO_PERFORMANCE_MODE_LOW_LATENCY` (avoiding 1920-frame / 40 ms legacy bursts that outpaced XAudio2's submitted frames and stretched audio), locked `setFramesPerDataCallback(256)`, added automatic linear resampling if device rate != 48 kHz, raised `--apu.max_queued_frames=32`, and enabled `--gpu.custom.vsync=true`.
  - **Ahead-of-Time GLES 3.2 Shader Pre-Warming (`gles3_shaders.h`, `gles3_shaders.cpp`, `gles3_backend.cpp`):**
    - `Gles3Shaders::Intern()` now defers `glCompileShader()` until a binary cache miss instead of recompiling every VS/PS stage on every boot.
    - `PrewarmCache()` scans `cacheDir_` for `*.glesbin` files and pre-loads them at `Initialize()` (`prewarmed=741` in ~0.66 s on Adreno 710; Bathysphere/Rapture runs at 50–53 FPS with zero shader compilation hitches).
  - **Hardware Write-Watch (`gles3_resources.h`, `gles3_resources.cpp`):**
    Wired `Gles3Resources` to `rex::IPhysicalWriteListener` (`OnPhysicalWrite` / `WatchPhysicalWrites`) for textures, VBs, and IBs, plus CPU-overwrite invalidation for resolved render targets.
  - **Cutscene Brown Overlay Investigation:**
    Compared Xenia vs Android screenshots and captured a 1-frame draw dump (`946` draws) on SM-A366B during the Bathysphere cutscene. The projector video renders inside the 3D scene, followed by `DUMP#832–#833` (deferred spotlight volume `ps=10ee2aad4ad76b55`, `bc0=0x04000400`, `dc=0x18704543`) and `DUMP#853` (fullscreen depth-sampled fog pass `ps=38fc8859e11347ce`, `bc0=0x07060706`, `dc=0x18701231`, `t0=f22:1280x720(R)`). Next step for `v0.0.5.4`: inspect those two shaders' GLSL and depth/stencil behavior.

---

## Session 17 — 2026-11-19 — `v0.0.4` to `v0.0.5-experimental` (Custom GLES 3.2 Renderer, Full Touch Controls, AAudio, Save/Load Fix & Cutscene/Perf Pass)

**Goal:** bring the Android and Windows ports to in-game playability with custom
GLES 3.2 rendering, live resolution scaling, native Android audio, full Xbox 360
touch controls, working in-game Save / Load, and smooth Bink cutscenes.
**Status: ACHIEVED (`v0.0.5-experimental`).**

Done:
  - **Custom OpenGL ES 3.2 Xenos Renderer on Android (`renderer/src/gles3/`):**
    built `xenos_glsl.cpp`, `gles3_backend.cpp`, `gles3_resources.cpp`,
    `gles3_shaders.cpp`, and `android_custom_driver.cpp` with live no-restart
    internal resolution scaling (`360p`–`1440p`, defaulting to `540p` / `2x`
    anisotropy on fresh installs).
  - **Full Xbox 360 Touch Controller (`TouchControlsView.kt` + `main.cpp`):**
    added dual floating analog sticks, `LT`, `LB`, `RT`, `RB`, 4-way `D-Pad`,
    `A`/`B`/`X`/`Y`, `L3`/`R3`, `BACK`, `START`, live `RES` pill, and `HUD` toggle.
  - **Native Android AAudio Driver (`sdl3_audio_driver.cpp`):** replaced the
    `#if defined(__ANDROID__)` no-op stub with a dynamic `dlopen("libaaudio.so")`
    48 kHz float32 stereo stream and enabled `needs.audio = true`.
  - **In-Game Save / Load Fixed (`v0.0.4.20-experimental`):**
    1. Fixed `NtSetInformationFile` (`XFileRenameInformation`) in
       `xboxkrnl_io_info.cpp` to strip `\??\` prefixes and handle `ObDosDevices`
       (`0xFFFFFFFD`) in `root_dir_handle`.
    2. Fixed Android SELinux `EACCES` (`0xC0000022`) in `host_file_posix.cpp`
       (`MoveNoReplace`) by bypassing hard `link()` on `__ANDROID__` in favor of
       `lstat()` + `rename()` so `Save.tmp` -> `DLContentInfo.pkg` succeeds.
    3. Extended `XamUserWriteProfileSettings` in `xam_user.cpp` to persist
       numeric profile types (`kInt32`, `kInt64`, `kFloat`, `kDouble`).
  - **Cutscene, Depth & Performance Optimizations (`v0.0.5-experimental`):**
    - Fast row-by-row `memcpy` path in `LinearizeLevel` for untiled Bink video
      frames (fixing Andrew Ryan elevator projection stutter).
    - Tiered texture fingerprint change-detection (dynamic/video textures checked
      every frame; static compressed textures throttled) + capped per-frame GC.
    - Replicated resolved depth across `RRRR` channels (`gles3_backend.cpp`) for
      underwater fog and spotlights.
    - Elevated GPU worker thread to display priority (`PRIO_PROCESS` `-8`/`-4`)
      and enabled Android `sustainedPerformanceMode`.
  - **Community & Release Packaging:** added Discord feedback server
    (`https://discord.gg/aaDRJQNw8T`) and required **Xbox 360 World Release**
    details (`Title ID 545407D8`, `Media ID 44E704F7`) to docs and published
    `v0.0.5-experimental` for both Android (`.apk`) and Windows (`.zip`).

---

## Session 16 — 2026-11-17 — 🎉 GOAL MET: title screen renders on Android

**Goal:** render the genuine BioShock title screen on Android through the real
runtime + VdSwap. **Status: ACHIEVED.**

Done:
  - Fixed invalid SPIR-V that crashed the Adreno driver: glslang 11.9.0's
    `createAccessChain(storage, base, offsets)` derived the result type from
    internal `accessChain.base` state the SDK never sets → type id 0. Patched
    `SpvBuilder.cpp::createAccessChain` to derive the type from the real
    `base`/`offsets` params (+ defensive guards). Guest shaders now compile;
    83+ translated; `spirv-val` only warns about benign explicit-offset std140.
  - Fixed black screen: presenter surface never connected under the headless SDL
    bootstrap (`SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED` never reached the UI resize
    handler, surface size stayed 0, paint mode `kNone`, `vkQueuePresentKHR` never
    called). Added public `App::NotifySurfaceResized(w,h)` and call it from the
    JNI `nativeResize`. Presents then began (`VK_SUBOPTIMAL_KHR`).
  - Fixed host heap-corruption crash (Scudo "invalid chunk state" in
    `PosixThread::SetName` ← `KernelThread::Create`, guest "VertexUploadThr")
    by guarding the Android build to skip non-essential thread-name surgery.
  - Restored `--runtime.protect_zero=false`, full 492-file game tree on-device.

Evidence:
  - `_work/android_title_now.png` — real "BIOSHOCK" sign + "PRESS START" +
    animated underwater-city scene (guest-presented, not painted).
  - `_work/android_title_final.png` — live fade transition (proves animation).
  - `_work/capframes/frame_010.png` — guest-rendered legal text screen.
  - logcat: `BS1-VDSWAP #2551`, `BS1-VKPRESENT present_result=1000001003`,
    app alive for minutes, no SIGSEGV/abort.
  - `scripts/verify-all.py` 12/12 PASS.

Docs updated: `32-GOAL-ANDROID-TITLE.md` (Completion), `10-STATUS.md`, this file.
Next action: none — goal met. (Optional polish: hide the SDL touch overlay for
a clean screenshot; map START button to advance past the title.)

---

## Session 15 — 2026-11-17 — real runtime guest boot on Android

**Goal:** render the guest title on Android (phase 2). **Status: guest runtime
launches on-device; no frame yet.**

- [x] Full SDK + game built for Android arm64 (NDK r30/Clang 21), APK assembles
      (147.8 MB, 19 native libs). Android Vulkan surface support added to SDK.
- [x] On-device boot chain fixed step by step (logcat): JNI shim classes for all
      SDL3 native methods, SDL main-ready, real SurfaceView surface, main-thread
      frame pump, sanitized allocator, memfd_create for the 4 GB guest mapping.
- [x] **Verified on-device:** `Init OK` → `LaunchTitle OK` → guest memory mapped
      at 0x100000000 → "Main Game Thread" starts. Real ReXGlue runtime + generated
      code booting on Android, far beyond the GLES3 diagnostic pattern.
- [ ] **Current blocker:** guest thread SIGSEGV reading `0x100000010` (guest base
      + 0x10) — low null guest page protection. Need guest RIP, then fix
      `protect_zero` setting or seed the pointer. Then capture a real frame.
- Docs: `32-GOAL-ANDROID-TITLE.md` parts 3-4; host goal still achieved, all green.

---

## Session 14 — 2026-11-17 — 🎉 TITLE SCREEN REACHED

**Goal:** reach the title screen. **STATUS: ACHIEVED.**

**The breakthrough (cross-DLL duplicate globals):**
- Instrumented `GetKernelServices()`/`Connect()` in `rexkernel_module.cpp` with
  `&g_connected` address logging. Found two copies of the kernel DLL loaded:
  `librexkerneld.dll` (static import of the exe; `GetKernelServices` reads it)
  and `rexkerneld.dll` (dynamic module load; `Connect()` runs there).
  `Connect()` set `g_connected=1` in one copy, `GetKernelServices()` read `0`
  in the other → "kernel module is not running".
- **Fix** in `third_party/rexglue-git/src/base/library_name.cpp`:
  `ModuleFileName()` now prepends `lib` on MinGW so the loader loads the SAME
  `librexkerneld.dll` the exe imports. Verified: `Connect()` and
  `GetKernelServices()` report the same `&g`, `g_connected=1`, non-null `xam`.

**Result:** title runtime loads; guest memory mapped; guest loop runs; VdSwap
presents a real frame. Screenshot `_work/title_capture2.png` (1920x1080) shows
the BIOSHOCK logo + "PRESS START" + underwater scene + perf overlay. Debug
logging removed; clean rebuild re-verified with fresh capture.

Also this session: reverted the broken in-process-kernel CMake experiment to a
coherent link (rexapp imports kernel symbols from rexkernel.dll).

**Next phase:** port the host's guest-rendered title to the Android APK (which
currently shows only a GLES3 diagnostic pattern).

---

## Session 13 — 2026-11-17 (real runtime built; game launches)

**Goal:** reach the title screen.

**Done (evidence in `31-GOAL-TITLE-SCREEN.md`):**

- [x] Cloned upstream ReXGlue **v0.11.0** source (matching our generated code),
      fetched all submodules, applied ~12 MinGW compatibility patches
      (`_work/patch_sdk_011.py`), swapped glslang 11.9.0 for GPU SPIR-V compat.
- [x] **Built all 11 ReXGlue runtime DLLs from source** with MinGW (zero
      undefined-symbol link) — the complete real kernel/runtime, ABI-matching
      our generated code. This replaces the hand-written HLE harness.
- [x] **Built the full `bioshock1.exe` (97 MB)** linking the 152 generated
      recompiled TUs with the real runtime.
- [x] Disabled the custom host allocator (MinGW heap corruption 0xC0000374);
      fixed D3D12 COM GUID stubs and pessimizing-move warnings.
- [x] **Verified launch**: all modules load, SDL3 init, **D3D12 device created
      on NVIDIA GTX 1660 SUPER**, a `bioshock1` window opens and stays alive.
      Evidence: `_work/real15_stdout.txt`.
- [x] Forced `game_root` via a source patch in `title_app.cpp` (the CLI/config
      settings were not applied in this MinGW build).

**Not achieved yet:** no title screen. Blocker: `runtime load failed, the
kernel module is not running` — `rexkernel.dll` loads but `Connect()` is not
called (`GetKernelServices()` null across DLLs). Next: fix the cross-DLL module
connect/interface-factory wiring.

---

## Session 1 — 2026-11-17 (foundation, completed)

**Goal:** turn the supplied archives and game dump into a buildable project.

**Done:**

- Vendored 12 toolchain archives into `third_party/`.
- Installed Android SDK 34 / build-tools / NDK 26.3 / CMake 3.22 into
  `F:\tools\android-sdk` with `scripts/install-toolchain.ps1`; accepted licences.
- Extracted the 7.8 GB XGD2 ISO and `default.xex` into `_game/`.
- Built `tools/xdvdfs.py`, `tools/xex_inspect.py`, `tools/pipeline.py`;
  generated `config/xex_analysis.json` and `config/bioshock1.toml`.
- Corrected three parser bugs (title ID ASCII fields, import record layout,
  static-library header ID) — see `10-STATUS.md` §3.
- Built the native C++ skeleton: 8 bootstrap HLE imports, GLES3 test renderer,
  JNI bridge, host selftest (`selftest OK`).
- Built the Kotlin launcher: SAF import, full XDVDFS tree extraction, GLES3
  surface, multi-touch overlay. APK assembles (8.8 MB, no game data).
- Wrote 19 docs.

**Blocker identified:** ReXGlue's *host recompiler binary* was missing. The
vendored source snapshot has 22 uninitialised git submodules and needs
Clang 20+ / CMake 3.25+ to build from source.

**Next action:** find or obtain a prebuilt ReXGlue host tool.

---

## Session 2 — 2026-11-17 (Phase 1 started, in progress)

**Goal:** obtain a working host recompiler and run the first real codegen.

**Findings so far:**

- `third_party/rexglue-sdk/.gitmodules` lists 22 submodules (FFmpeg, glslang,
  spirv-tools, SDL3, Vulkan, imgui, etc.). Building from the plain source
  archive is possible but heavy.
- GitHub Releases has **prebuilt SDK v0.11.0** for win-amd64 (~105 MB), which
  should contain `rexglue` CLI + runtime headers/libs. Downloading it now into
  `tools-bin/`.

**Planned steps (update as completed):**

- [x] Unpacked the prebuilt SDK into `tools-bin/rexglue-sdk-0.11.0/`;
      `rexglue.exe` (33.9 MB) verified with `--help` and subcommand help.
- [x] Recorded a reproducible downloader: `scripts/install-rexglue.ps1`.
- [x] CLI differs from the v0.8-era snapshot docs: `init` takes
      `--project-name/--xex-path/--game-root/--project-root`, `codegen` takes an
      optional manifest path and many iteration-limit flags. Manifest is simple
      TOML with `[project]` and `[entrypoint]` sections. Use the v0.11 CLI help
      as authority.
- [x] `rexglue init` created `_work/rexglue-bioshock1/` with manifest,
      CMakeLists, CMakePresets, `src/main.cpp`, `src/bioshock1_hooks.cpp`.
- [x] Studied generated scaffold; documented in `20-REXGLUE-V011.md`.
- [x] **Ran `rexglue codegen` on the real XEX — SUCCESS.**
  - 61.0 s, "308 written, 0 unchanged, 0 deleted", 311 files, 156.5 MB.
  - ReXGlue independently confirmed Title `545407D8`, Media `44E704F7`,
    Version `0.0.0.1`, Filetime `2007-06-22 23:21:30 UTC`.
  - Phases: Register → Scan → Discover → GapFill → Merge → Validate → Write.
  - No warnings/errors in `codegen.log`.
  - Generated 151 `bioshock1_recomp.*.cpp` + 151 `bioshock1_funcs.*.h`,
    `bioshock1_init.cpp`, `bioshock1_register.cpp`, `bioshock1_pch.h`,
    `codegen.partition.json`, `codegen.mmio.json`, `sources.cmake`.
  - Key constants from `bioshock1_pch.h`: `REX_IMAGE_BASE=0x82000000`,
    `REX_IMAGE_SIZE=0xCD0000`, `REX_CODE_BASE=0x82160000`,
    `REX_CODE_SIZE=0x8F195C`, `REX_THUNK_RESERVE_SIZE=0x10000`.
- [x] Single generated TU compiles cleanly with clang++ 23, C++23,
      `-DREX_PHYSICAL_OFFSET_IN_CODE=0`, SDK include path.
- [x] **Compiled ALL 152 generated TUs in parallel — SUCCESS.**
  - `tools/codegen-validate/CMakeLists.txt` (with `file(TO_CMAKE_PATH ...)`
    to survive Windows backslashes), CMake 3.31.6 + Ninja + clang++ 23,
    C++23, `-DREX_PHYSICAL_OFFSET_IN_CODE=0`.
  - 152/152 targets built; 152 `.obj` files, 137.6 MB.
  - Fixed initial CMake path-escape error (`\G` in `\generated`) with
    `file(TO_CMAKE_PATH ...)`.
  - **Phase 1 gate achieved:** codegen output is valid, compilable C++.

- [x] **Android arm64 compile check of generated code — SUCCESS.**
  - NDK r28c + CMake Android mode + libc++ compat shim compiled **all
    152 generated TUs** to arm64 `.o` files (153 objects total including init).
  - Remaining link-time symbols inventoried with `llvm-nm -u`: 216 kernel
    imports (stubbed), 118 `__rest*` + 118 `__save*` ABI helpers (ReXGlue
    runtime), 6 mangled `rex::` runtime functions, 3 compiler/atomic symbols,
    and 36k weak `sub_*` aliases resolved within the generated set.
  - NDK 26.3 (Clang 16 / libc++) **fails**: `std::atomic_ref` missing,
    which ReXGlue v0.11's `rex/types.h` requires. Not a codegen problem —
    a toolchain version problem.
  - Downloaded **NDK r28c** (28.2.13676358, Clang 19.0.1) into `tools-bin/`.
    It still lacks `std::atomic_ref` **and** public `std::jthread`/
    `stop_token` in libc++.
  - Added `native/include/compat/atomic_ref.h`: forced-include compatibility
    shim implementing minimal `std::atomic_ref`, `stop_token`, `stop_source`
    and `jthread` on top of compiler atomics / `std::thread`. Enabled by
    `-DBS_NEED_ATOMIC_REF_SHIM=1 -include <shim>`.
  - One generated TU now compiles cleanly for `aarch64-linux-android28`.
  - Full 152-TU arm64 validation build is running via
    `build/codegen-validate-android` (CMake Android mode + NDK r28c).
  - `native/CMakeLists.txt` opt-in `BS_USE_GENERATED_CODE` now wires the
    staged sources, SDK includes and shim into the Android target.

- [x] Extracted the authoritative HLE scope from generated object files with
      `llvm-nm -u`: 216 real kernel/XAM imports (rest are generated aliases).
      Wrote `docs/21-HLE-IMPORT-MATRIX.md` with every name and family counts.
- [x] Staged the validated generated snapshot: `tools/stage_generated.py`
      copied 308 files (164.1 MB) to `recomp/generated/` with a manifest.
- [x] Added name-based HLE dispatch (`register_named`/`call_named`) to
      `hle::Kernel`, plus `tools/gen_import_stubs.py` which generated 216
      `__imp__*` stub functions in `native/src/import_stubs.cpp`. Host
      self-test now verifies one stub forwards correctly.
- [ ] Add a `source` changelog entry to `10-STATUS.md` and `18-FILE-MANIFEST.md`.

**Risks noted:** version skew between the supplied v0.8-era snapshot and the
v0.11 prebuilt (CLI flags, manifest schema). Mitigation: use the prebuilt's own
docs/help as authority and note differences rather than guessing.

---

## Session 3 — 2026-11-17 (Phase 2 started)

**Goal:** link the recompiled arm64 code into a `.so` and start real HLE work.

**Done so far:**

- [x] `tools/extract_runtime_symbols.py` reads `llvm-nm -u` from the 153 arm64
      objects and writes `config/runtime_symbols.json`: **246 remaining link
      symbols** (236 `__rest*`/`__save*` ABI helpers, 6 mangled `rex::`
      runtime functions, `xstart`, two ARM64 CAS builtins).
- [x] `tools/gen_runtime_stubs.py` generates 246 no-op stub functions with
      exact asm labels in `native/src/runtime_stubs.cpp` (link-time
      placeholders, clearly documented as not final implementations).
- [x] Extended `tools/codegen-validate/CMakeLists.txt` with opt-in
      `BS_BUILD_LINK_TEST` that links `$<TARGET_OBJECTS:generated_code>` plus
      our engine/HLE/import/runtime stub sources into a shared library with
      `-Wl,-z,defs` so every unresolved symbol fails the build.
- [x] **First real arm64 link SUCCEEDED** — `libbs1_link_test.so` was produced
      by linking all 152 generated objects with our engine/HLE/import/runtime
      stubs under `-Wl,-z,defs` (zero unresolved symbols). Fixed three setup issues:
      1. native source dir was `../native` instead of `../../native`;
      2. generated objects lacked `POSITION_INDEPENDENT_CODE ON` — `ld.lld`
         reported `R_AARCH64_ADR_PREL_PG_HI21`/`R_AARCH64_ADD_ABS_LO12_NC`
         relocation errors against `rex::ppc::simd::kVectorMaskL` etc.,
         fixed with `set_target_properties(... POSITION_INDEPENDENT_CODE ON)`.
      3. `-Wl,-z,defs` is enabled on the link so unresolved symbols fail loudly.

**Planned next:**

- [ ] Iterate until the link succeeds with zero undefined symbols.
- [ ] Replace no-op stubs with real ABI-helper implementations where needed.
- [ ] Find the guest entry point and call it from `engine.cpp`.
- [ ] Implement HLE handlers in the order from `21-HLE-IMPORT-MATRIX.md`.
- [ ] Get a boot log from a device (M1/M2 renderer milestones).

**Design note:** the `__imp__*` import stubs forward to `hle::Kernel::call_named`,
so once the `.so` links, the first boot will produce a precise list of missing
kernel functions via `missing_named_imports()` — that list, not guesswork,
drives implementation order.

**Design fix found during Phase 2:** the generated code calls
`__imp__<Name>(PPCContext&, base)`, but the first stub generator emitted
zero-argument functions. That discarded guest registers, making real HLE
impossible. Fixed: `NamedHandler` now takes `(PPCContext&, uint8_t*)`,
`call_named` forwards both, and the regenerated 216 stubs match the generated
calling convention. Host self-test updated and passes.

**Entry point resolved:** decrypted/decompressed the PE with
`third_party/360tools/tools/extract_pe.py` into `_work/default_unpacked`;
PE `AddressOfEntryPoint = 0x19D698` → guest VA **`0x8219D698`**. ReXGlue maps
that address to a function named `xstart`. Investigation showed the real CRT
startup body **is** generated (`bioshock1_recomp.84.cpp` → `__imp__xstart`),
so we removed the conflicting no-op stub and taught
`extract_runtime_symbols.py` to exclude `xstart`. No boot blocker here.
Recorded in `09-RE-FINDINGS.md` §0c.

---

## Session 4 — 2026-11-17 (hardening + Phase 3 start)

**Goal:** make everything rock-solid, run a device boot test, start Phase 3.

**Done:**

- [x] Added `scripts/verify-all.py` — 12 automated checks; all PASS. Documented
      in `23-VERIFICATION.md`. This is the new gate for every future change.
- [x] On-device M0 test on Samsung SM-A366B / Android 16: installed APK,
      launched, seeded XEX into app-private storage, tapped START, captured
      screenshots. **GLES3 animated pattern + touch overlay render correctly**;
      `glError=0x0`, shaders compiled, no crashes. Evidence in `24-DEVICE-M0.md`.
- [x] Fixed two real UI bugs found on device:
      - `extractNativeLibs=false` made the "engine library" file check wrong;
        now uses a JNI call as proof;
      - HLE bootstrap imports now register at library load so the menu shows 8.
- [x] Added `BS1`/`BS1-GPU` logcat diagnostics for shader/init/render.
- [x] Phase 3 started: implemented a standalone PM4 command-stream parser
      (`native/include/xenos_pm4.h`, `native/src/xenos_pm4.cpp`), wired into
      `gpu::submit_command_buffer()` / `bs_engine_submit_cmds()` / JNI
      `nativeSubmitCmds()`. Host self-test with a synthetic buffer passes:
      packets=3, register_writes=1, draws=1, swaps=1, malformed=0.
      Documented in `25-PHASE3-RENDERER.md`.

**Next:** implement real ABI helpers + HLE handlers (Phase 2 boot), then capture
a real command buffer via `VdSwap` and turn PM4 draws into GLES3 draw calls.

---

## Session 5 — 2026-11-17 (docs website + Phase 3 continued)

**Goal:** build a SQL-backed visualization of all docs, then continue Phase 3.

**Done:**

- [x] Built the docs-intelligence website:
  - `tools/docs_indexer.py` → `web/docs.db` (SQLite, 304 KB) + `web/docs.sql`
    (253 KB), indexing 26 docs, 76 facts, 98 cross-reference edges, 5 worklog
    sessions and 12 verification checks;
  - `web/server.py` stdlib JSON API + `web/index.html`/`app.js`/`style.css`
    SPA (overview charts, document reader, facts, checks, timeline, graph,
    ranked search);
  - `scripts/serve-docs.ps1` one-command rebuild + launch;
  - API smoke tests passed; `node --check web/app.js` passed. Documented in
    `26-DOCS-WEBSITE.md`. Browser automation driver was unavailable, so visual
    verification was not possible in this environment.
- [x] Phase 3 continued: added `xenos::RegisterFile` with real register indices
      (VGT_INDX_OFFSET 0x2102, VGT_DMA_SIZE 0x21FB, VGT_DRAW_INITIATOR 0x21FC),
      wired PM4 type-0/1 writes into it, and captured `DrawCommand` records on
      draw opcodes. Extended host self-test: register values and first draw
      command verified. Documented in `25-PHASE3-RENDERER.md` §4b.

**Next:** real ABI helpers + HLE handlers (Phase 2 boot), then capture a real
command buffer via `VdSwap` and turn queued draws into GLES3 draw calls.

---

## Session 6 — 2026-11-17 (guest context alignment)

**Goal:** remove the layout mismatch that would corrupt guest registers in HLE.

**Done:**

- [x] `native/include/ppc_context.h` now switches between the standalone
      context and the real global `struct PPCContext` from ReXGlue's
      `<rex/ppc/context.h>` via `BS_USE_REX_CONTEXT`.
- [x] Added portable `gpr_read`/`gpr_write`/`link_register` accessors and
      replaced every direct `ctx.r[n].value` in `hle_kernel.cpp`.
- [x] `native/CMakeLists.txt` and `tools/codegen-validate/CMakeLists.txt`
      define `BS_USE_REX_CONTEXT=1` for generated-code builds.
- [x] Rebuilt the arm64 link target: `libbs1_link_test.so` (139.22 MB) links
      with `-Wl,-z,defs`, zero unresolved symbols, using the real context.
      Fixed: global-namespace `PPCContext`, added `xenos_pm4.cpp` to link test,
      linked `log` for Android logging.

**Next:** implement real `__rest*`/`__save*` ABI helpers and boot-critical HLE
handlers, then call `__imp__xstart(ctx, base)` to reach the guest entry point.

---

## Session 7 — 2026-11-17 (real ABI helpers)

**Goal:** replace the 236 no-op register save/restore helpers with working code.

**Done:**

- [x] Disassembled the real XDK helper tables in the decrypted image and read
      ReXGlue's authoritative helper naming (`phase_register.cpp`) to derive
      exact layouts for GPR/LR, FPR and VMX (14..31, 64..127) helpers.
- [x] `tools/gen_ppc_accessors.py` → generated FPR/VMX accessors for both
      context layouts; `tools/gen_abi_helpers.py` → 236 real helpers with exact
      big-endian guest-memory semantics in `native/src/abi_helpers.cpp`.
- [x] Generated `config/runtime_non_abi.json`: only 9 non-ABI placeholders remain.
- [x] Host self-test now round-trips GPR+LR and a full 16-byte VMX register.
      Found and fixed a real bug: standalone registers were 32-bit; now 64-bit
      to match ReXGlue. SELFTEST OK.
- [x] arm64 `.so` relinked with real helpers + ReXGlue context: 139.5 MB,
      `-Wl,-z,defs`, zero undefined symbols. APK rebuilds. Documented in
      `27-ABI-HELPERS.md`.

**Next blocker:** the 216 kernel/XAM import handlers. After that, call the
guest entry `__imp__xstart(ctx, base)` and boot toward the title screen.

---

## Session 8 — 2026-11-17 (216 import handlers)

**Goal:** eliminate the missing-import blocker before guest boot.

**Done:**

- [x] `tools/gen_import_names.py` → `native/include/hle_import_names.h` with all
      216 names from `config/hle_imports.json`.
- [x] `native/src/hle_imports.cpp`: 32 real handlers (memory, files, time, TLS,
      Rtl/CRT, `VdSwap` → `gpu::present()`, input, system constants) plus a safe
      default that writes `r3 = NTSTATUS_SUCCESS`. Registers every name at load
      from `hle_kernel.cpp` bootstrap.
- [x] Host self-test verifies zero missing named imports, `r3` return ABI,
      `KeGetCurrentProcessType`, `RtlCompareMemoryUlong`, plus prior ABI/PM4
      tests. SELFTEST OK.
- [x] arm64 `.so` relinked: 139.64 MB, `-Wl,-z,defs`, zero undefined symbols;
      APK rebuilds. Documented in `28-HLE-IMPORTS.md`.
- [x] Debug notes: COFF `__imp_*` collides with DLL imports on host (tests use
      `call_named`); `strlen`/`RtlCompareMemory` are not XEX imports, which
      confirmed the default handler returns 0 safely.

**Next:** 9 non-ABI runtime placeholders, then call `__imp__xstart(ctx, base)`
and iterate on the first real guest boot log.

---

## Session 9 — 2026-11-17 (final runtime symbols + first boot)

**Goal:** finish the 9 runtime symbols, then attempt a real first boot.

**Done:**

- [x] `native/src/runtime_symbols.cpp` implements all 9 non-ABI symbols:
      MMIO load/store via thread-local guest base, `ResolveFunction` via the
      generated `kPPCFuncMappings`, Trap logging, `mmio_slots` data, guest tick
      count, CAS builtins and `__main`. Replaced the last no-op stubs.
- [x] arm64 `.so` relinks: **139.66 MB**, `-Wl,-z,defs`, zero undefined symbols.
- [x] Built a host first-boot harness (`tools/boot-harness/main.cpp`) that maps
      2.25 GB of guest memory, loads the decompressed PE at `0x82000000`, sets
      up a `PPCContext`, and calls `__imp__xstart`.
- [x] First run faulted in `sub_8219EF10` (indirect-call loop). Root cause:
      uninitialised per-module dispatch table. Fixed by installing all 36,625
      `kPPCFuncMappings` entries in the guest table. Second run progressed to
      `sub_821A10B0`, which dereferences an uninitialised guest structure.
      Both faults symbolised via `llvm-nm` RVA mapping. Documented in
      `29-FIRST-BOOT.md`.

**Next:** initialise the guest globals/thread state that XDK startup expects,
then re-run the harness and follow the HLE trace until `main` is reached.

---

## Session 10 — 2026-11-17 (boot debugging iteration 2)

**Goal:** follow the first fault to a concrete root cause and fix it.

**Done:**

- [x] Added stack capture to the boot fault filter and mapped RVAs via
      `llvm-nm`: call chain `main → xstart → sub_8219EF10 → sub_8262A288 →
      sub_8262A360 → sub_821A10B0`.
- [x] Overrode the weak alias `sub_821A10B0` in the harness to log guest
      arguments: `r3=0`, `r4=8`, `r5=0x80`.
- [x] Traced the null to the guest global read by `sub_821A2538`; corrected the
      `lis` constant (`-32077` → `0x82B3`) and seeded guest address `0x82B2E2FC`
      with a valid heap pointer. Trace now shows `r3=0x83400000`.
- [x] Next fault is an access at `guest_base - 3` (wrapped 32-bit guest
      address) inside `sub_821A10B0`, requiring deeper kernel-state
      initialisation. Documented in `29-FIRST-BOOT.md` §4/§5.

**Next:** continue tracing the wrapped guest address; port the corresponding
guest-global initialisation from ReXGlue's kernel runtime contract.

---

## Session 11 — 2026-11-17 (goal: title screen, structured work)

**Goal:** reach the title screen; structure the work and document progress.

**Done (evidence in `31-GOAL-TITLE-SCREEN.md`):**

- [x] Created the goal/plan document.
- [x] Mapped the full 32-bit guest space (4 GB + slack); wrapped-address fault gone.
- [x] Found that static HLE registration was not active in the harness; added
      explicit `register_named_imports()` call. Boot then executed ~19,000 HLE
      calls — real guest startup work.
- [x] Fixed invalid TLS slot (-1) returning null by returning guest scratch
      memory.
- [x] Added a vectored exception handler with raw `WriteFile` output; current
      fault identified as Windows `0xC00000FD` stack overflow in a
      `sub_8219C488` / `sub_82623480` / TLS loop.
- [x] Structured the remaining work: KPCR/r13 setup, real TLS slot allocation,
      critical-section state, then `VdSwap` present.

**Not achieved yet:** the game does not reach the title screen. The blockers
are kernel-state semantics, not toolchain or codegen issues. Removing all debug
wrappers from the harness did not fix the stack overflow, confirming the
recursion is in guest code.

**Later in this turn:** running guest code on a 256 MB-stack thread removed the
stack overflow; clearing the pre-seeded allocator global let the guest run to a
clean return (**RC 0**, `r3=0x83000150`, 2,545 HLE calls) instead of crashing.
The last-call ring buffer shows a spin on critical sections. ReXGlue prebuilt
host build failed at link (MSVC CRT libs unavailable under MinGW). Evidence and
next blocker recorded in `31-GOAL-TITLE-SCREEN.md`.

---

## Session 12 — 2026-11-17 (file/config subsystem reverse-engineered)

**Goal:** reach the title screen; diagnose why the guest spins after the
no-op-success fix reduced HLE calls from ~2M to ~7,000.

**Done (evidence in `31-GOAL-TITLE-SCREEN.md`):**

- [x] Instrumented the no-op indirect-call target: returning `r3=1` made callers
      use `1` as a pointer (hash table at `0x5`). Switched to stable, zeroed
      guest object pointers per call site; no more garbage-pointer faults.
- [x] Wrapped the wide-string comparator and identified the guest subsystem as
      Unreal's config loader (`contentMode.ini`, `User.ini`, ..., key `FilePath`).
- [x] Implemented the previously no-op HLE APIs: `RtlInitAnsiString`,
      `RtlInitUnicodeString`, `RtlUnicodeToMultiByteN`, real `NtCreateFile` /
      `NtOpenFile` / `NtReadFile` / `NtClose` backed by the 492 extracted disc
      files (indexed 962 name/path entries).
- [x] Confirmed SDK layout: `X_OBJECT_ATTRIBUTES.name_ptr` at +0x4 and
      `X_IO_STATUS_BLOCK.information` at +0x4.
- [x] Pinpointed current blocker: config string-hash table has a corrupt bucket
      pointer (`0x38`), causing a cyclic chain walk. `VdSwap` still not called.

**Not achieved yet:** no title screen/frame. The remaining work is supplying a
valid bucket array for the config hash table and continuing the kernel-runtime
port. Verified with `verify-all.py` 12/12 after each change.

---

## Template for future entries

```
## Session N — YYYY-MM-DD (phase)

Goal:
Done:
  - ...
Failed / blocked:
  - ...
Evidence:            (commands + key output, trimmed)
Docs updated:        (list)
Next action:         (single, concrete)
```
