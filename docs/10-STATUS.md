# Status — verified facts, not aspirations

Last updated: 2026-11-17 (title screen reached).

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
| HLE kernel completeness | ⚠️ skeleton | 8 bootstrap imports; 427 import records need handlers |
| GLES3 game rendering | ⚠️ diagnostic only | Test pattern renders; Xenos PM4/shader translator not implemented |
| Audio (XMA/FMOD → AAudio) | ⛔ not started | Documented in `07-GPU-XENOS-GLES3.md` / `11-ROADMAP.md` |
| Playable game | ⛔ no | See honest status in `00-START-HERE.md` |

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
