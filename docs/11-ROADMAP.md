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

## Phase 1 — Host codegen toolchain

- [ ] Clone ReXGlue SDK recursively; install Clang 20+/CMake 3.25+
- [ ] Build `rexglue` host binary
- [ ] `rexglue init` with a BioShock project manifest
- [ ] `rexglue codegen` on the real `default.xex`
- [ ] Capture and triage `UnresolvedCall` / analysis errors
- [ ] Fill function boundaries, jump tables, ABI helper addresses in TOML
- [ ] Produce a complete `recomp/generated/` C++ set that compiles for host

**Gate:** generated PPC code compiles on the host without unresolved symbols.

## Phase 2 — First boot on device

- [ ] Add generated C++ to `native/CMakeLists.txt` Android target
- [ ] Port guest filesystem HLE to the app-private tree already extracted by the launcher
- [ ] Implement top-N import handlers by frequency (memory, threads, sync, CRT)
- [ ] Wire `VdSwap` → `gpu::present()` (M1)
- [ ] Debug to title screen / main menu

**Gate:** the game reaches its title screen without a fatal HLE gap.

## Phase 3 — Rendering to first geometry

- [ ] Port/enable Xenos PM4 command-buffer parser (M1 trace)
- [ ] EDRAM surface allocation + tile resolve shaders
- [ ] Adapt XenosRecomp shader translator to GLSL ES 3.00
- [ ] Texture detiler + BCn→ASTC/RGBA8 upload
- [ ] First real textured draw (M2), then menus (M3), then gameplay (M4)
- [ ] Wire the existing touch overlay to guest XINPUT via `HookXInputState`; add gamepad priority

**Gate:** in-game 3D geometry renders with correct shaders and textures.

## Phase 4 — Audio and completeness

- [ ] FFmpeg arm64 XMA2 decoder (or alternative)
- [ ] AAudio output with OpenSL ES fallback
- [ ] FMOD `.fsb` bank loading through guest filesystem
- [ ] Save-game persistence (guest `game:\` paths → app storage)
- [ ] Cutscene/video path (Bink decoder) if used
- [ ] Performance pass: XenonRecomp local-register optimisations, frame pacing

**Gate:** game is playable start-to-finish with audio.

## Phase 5 — Polish and distribution

- [ ] Full touch control overlay with plasmid/weapon wheel
- [ ] Settings UI (resolution scale, audio, controls, debug overlay)
- [ ] Compatibility database entry in `compat/games.json` style
- [ ] Release signing, reproducible build script
- [ ] Final documentation pass with screenshots and boot logs

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
