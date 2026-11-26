# Roadmap

Phases are gated: do not start a later phase before the previous gate is met.
The estimates are deliberately vague — this is research-grade work on a large
commercial title, and unknown unknowns dominate.

## Phase 0 — Foundation ✅ (done)

- [x] Vendor all 12 toolchain archives into `third_party/`
- [x] Extract game ISO and `default.xex` from XDVDFS
- [x] Parse XEX2 headers; identify title ID, XDK build, base address, imports
- [x] Generate reproducible analysis JSON + recompiler TOML
- [x] Build native runtime skeleton (HLE dispatch, GLES3 test pattern, JNI)
- [x] Kotlin launcher with SAF import, full on-device XDVDFS extraction, GLES3 surface and touch overlay
- [x] Buildable `arm64-v8a` APK with zero game data
- [x] This documentation suite

**Gate:** an APK builds and the host self-test passes. ✅

## Phase 1 — Host codegen toolchain ✅ (done)

- [x] Clone ReXGlue SDK recursively; install Clang 20+/CMake 3.25+
- [x] Build `rexglue` host binary
- [x] `rexglue init` with a BioShock project manifest
- [x] `rexglue codegen` on the real `default.xex`
- [x] Capture and triage `UnresolvedCall` / analysis errors
- [x] Fill function boundaries, jump tables, ABI helper addresses in TOML
- [x] Produce a complete `recomp/generated/` C++ set that compiles for host

**Gate:** generated PPC code compiles on the host without unresolved symbols. ✅

## Phase 2 — First boot on device ✅ (done)

- [x] Add generated C++ to `native/CMakeLists.txt` Android target
- [x] Port guest filesystem HLE to the app-private tree already extracted by the launcher
- [x] Implement top-N import handlers by frequency (memory, threads, sync, CRT)
- [x] Wire `VdSwap` → `gpu::present()` (M1)
- [x] Debug to title screen / main menu

**Gate:** the game reaches its title screen without a fatal HLE gap. ✅

## Phase 3 — Rendering to first geometry ✅ (done)

- [x] Port/enable Xenos PM4 command-buffer parser (M1 trace)
- [x] EDRAM surface allocation + tile resolve shaders (`RRRR` depth replication)
- [x] Adapt Xenos shader translator to HLSL 5.0 (Windows D3D11) and GLSL ES 3.20 (Android GLES 3.2)
- [x] Texture detiler + BCn/RGBA8 upload + tiered fingerprint caching
- [x] First real textured draw (M2), then menus (M3), then gameplay (M4)
- [x] Wire the full Xbox 360 touch overlay (`TouchControlsView.kt`) to guest XINPUT

**Gate:** in-game 3D geometry renders with correct shaders and textures. ✅

## Phase 4 — Audio and completeness ✅ (done in `v0.0.4.20`–`v0.0.5`)

- [x] XMA hardware decoder active on Windows and Android (`arm64-v8a`)
- [x] Native AAudio output on Android (`libaaudio.so`, 48 kHz float32 stereo)
- [x] FMOD `.fsb` bank loading through guest filesystem
- [x] Save-game persistence (`SaveGames:` → `DLContentInfo.pkg` & `.bsg`, `ObDosDevices` & Android SELinux `MoveNoReplace` fixes)
- [x] Cutscene/video path (Bink video fast row-copy upload + per-frame dynamic fingerprint check)
- [x] Performance pass: display-priority GPU worker thread, Android sustained performance mode, `540p`/`2x` anisotropy default

**Gate:** game is playable in-game with audio, cutscenes, and working Save / Load. ✅

## Phase 5 — Polish and distribution 🔄 (in progress, `v0.0.8.2-experimental`)

- [x] Full Xbox 360 touch control overlay (dual floating sticks, LT/LB/RT/RB, D-Pad, A/B/X/Y, L3/R3, BACK/START)
- [x] Live resolution scaling UI (`360p`–`1440p` `RES` pill on Android; **F1**/**F2**/**F4** `360p`–`2160p` on Windows)
- [ ] Compatibility database entry in `compat/games.json` style
- [x] Public experimental GitHub Releases (`v0.0.1` through `v0.0.5-experimental`) with Android APK & Windows standalone zip
- [x] Community feedback Discord (`https://discord.gg/aaDRJQNw8T`) and World Release documentation (`Title ID 545407D8`, `Media ID 44E704F7`)

**Gate:** release-quality APK with documented known issues.

## Explicit non-goals

* Xbox Live, achievements, multiplayer, marketplace, sign-in.
* Supporting the BIOS at runtime (it stays in `_reference/` only).
* Switching to Vulkan (the requested target is OpenGLES3).
* Shipping any game assets in the repository or APK.

## How to track progress

Update `10-STATUS.md` with evidence for each checked box, and add every
non-obvious discovery to `09-RE-FINDINGS.md` with address, byte pattern and
verification method. A checkbox without evidence is not done.
