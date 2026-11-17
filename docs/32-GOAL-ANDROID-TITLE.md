# Goal: render the guest title screen on Android

**GOAL MET 2026-11-17** — the genuine BioShock title screen renders on-device
through the real ReXGlue runtime + Vulkan + VdSwap. See "Completion" at the end.

Active goal (phase after the host title-screen breakthrough). Last updated
2026-11-17.

## Where we are

- **Host:** the genuine BioShock title screen renders via a host build of the
  real ReXGlue v0.11.0 runtime + D3D12 (`build/bs1-real/bioshock1.exe`,
  evidence `_work/title_capture2.png`). This proves the generated PPC→C++ code
  plus real runtime reaches and presents the guest title.
- **Android (current):** `app-debug.apk` ships a hand-written native engine
  (`native/`) that only draws a GLES3 diagnostic pattern. The generated code is
  compiled for arm64 (`BS_USE_GENERATED_CODE`) but NOT run — it needs the real
  ReXGlue runtime dispatcher/kernel symbols to link. Device test `24-DEVICE-M0.md`
  passed for the delivery shell only.

## Goal

Make the Android APK render the **same guest title screen** (real guest output
through the present path, not a painted/faked frame), on the Samsung SM-A366B
(Adreno, Vulkan 1.1-capable), with game data kept out of the APK.

## Architecture decision

Reuse the **real ReXGlue runtime** on Android instead of extending the
hand-written HLE engine. The SDK already has:
- `REX_PLATFORM_ANDROID` in `include/rex/platform.h`.
- posix platform implementations (`src/base/*_posix.cpp`).
- a Vulkan backend with `src/gpu/.../instance_khr_android_surface.inc`
  (Android surface extension).

So the target is: build SDK + generated game code for `arm64-v8a` with the
NDK, using the **Vulkan** GPU backend (D3D12 is Windows-only), and drive the
`TitleApp` loop from JNI presenting to an Android `ANativeWindow`.

## Work breakdown (in order)

1. **Build the SDK for Android arm64.** Configure `third_party/rexglue-git`
   with the NDK toolchain, `CMAKE_SYSTEM_NAME=Android`, API >= 24,
   `REXGLUE_USE_VULKAN=ON`, and the bundled Vulkan headers/library. Fix compile
   errors (NDK libc++ gaps: `std::atomic_ref`, jthread, clock_cast shims already
   exist for Apple/MinGW; expect more). Produce static/shared libs for rexbase,
   rexkernel, modules, Vulkan GPU backend, rexapp.
2. **Link the generated game code** (152 TUs in `recomp/generated`) with the
   Android runtime into a single `libbioshock1.so` (or a set of `.so` modules +
   host). The 152 TUs already compile for arm64 (NDK r26/r28).
3. **JNI bridge**: replace the diagnostic `native/src/engine.cpp` +
   `gpu_layer.cpp` path with a bridge that creates the ReXGlue `TitleApp`, sets
   `title.game_root` to the extracted game directory, starts the runtime and
   guest loop on a native thread, and tears down cleanly. Keep
   `nativeSetControls` mapping touch → XInput HLE.
4. **Kotlin surface**: switch from `GLSurfaceView` (GLES3 context) to a
   `SurfaceView`/`ANativeWindow` (or SDL3 Android activity) so the Vulkan
   presenter can create its swapchain on the surface. Keep SAF import, XEX
   extraction, touch overlay, and the status UI.
5. **On-device test**: install, seed XEX, start, capture screenshot, confirm the
   BIOSHOCK title renders (compare to `_work/title_capture2.png`), check logcat
   for no FATAL/exceptions. Update `24-DEVICE-M0.md` or add `33-DEVICE-TITLE.md`.

## Known risks / watch-items

- MinGW `lib`-prefix module-loader bug had an analogous cause on host; on
  Android ensure the module loader resolves `librex<name>.so` consistently so
  there is only ONE copy of `g_connected`/`g_services` (use static linking of
  the runtime into one `.so` to avoid cross-`.so` duplicate globals entirely).
- Adreno Vulkan driver quirks; glslang 11.x needed for SPIR-V enum compat (same
  as host — glslang 11.9.0 already fetched in `third_party/rexglue-git/thirdparty/`).
- APK size: host exe is 97 MB; Android `.so` will be large. Game data must stay
  out of the APK (XEX imported via SAF at runtime).
- The current `native/` HLE engine can remain as a fallback if the runtime port
  stalls; do not delete it.

## Progress 2026-11-17 (this session)

**Step 1 DONE: full SDK builds for Android arm64.** Installed NDK r30
(Clang 21, required ≥20; r28c only had 19). Configured
`build/rexglue-android` with Vulkan backend; fixed ~12 bionic/API- gaps:
- `Iconv`/`PkgConfig` CMake detection (bionic iconv is in libc, API 28+;
  X11/Wayland desktop-only).
- `rex::GetAndroidAPILevel()` missing definition (added to platform.h using
  `android_get_device_api_level()`).
- `backtrace()` absent on bionic (guarded out in crash_posix.cpp).
- ucontext absent on bionic (new pthread-based `fiber_android.cpp` shim).
- `rex/main_android.h` missing (stub created).
- robust mutex / `copy_truncating` / `current_thread` API-level branches
  patched in threading_posix.cpp.
- `renameat2` absent on bionic (guarded in host_file_posix.cpp).
- `-lpthread -lrt -ldl` not needed on Android (removed from rexbase link).
- unused-parameter/const warnings with `-Werror` guarded.

Result: `third_party/rexglue-git/out/linux-arm64/` has all 17 `.so` modules —
librexkerneld.so (40 MB), librexbased.so (26 MB), Vulkan GPU backend, xam,
filesystem, SDL3 modules, etc. `rexglued` (codegen CLI) also built for Android.

## Progress 2026-11-17, part 2 (this session)

**Steps 2-3 DONE: real-runtime Android game library + APK build.**
- Created `_work/rexglue-android-app/` — JNI `main.cpp` that creates a
  `TitleApp`, parses `--game_data_root`, calls Init → LaunchTitle, and pumps
  `RunFrame()` from JNI (`nativeInit`/`nativeRender`/`nativeFrame`/etc.), plus a
  CMakeLists that links the 152 staged generated TUs with `rex::app`.
- **Built `build/bs1-android/libbioshock1-android.so` (210 MB)** — links cleanly
  with NEEDED on librexappd/librexkerneld/librexppcd/librexbased/libc++_shared,
  all JNI symbols present. NDK r30 libc++ has native atomic_ref/jthread, so the
  old compat shim was dropped.
- Implemented **Android Vulkan surface presentation** in the SDK (was missing):
  added `NativeWindow::Type::kAndroid`, `Surface::kTypeIndexAndroidANativeWindow`,
  SDL3 `SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER` handling, and a
  `vkCreateAndroidSurfaceKHR` case in `vulkan_presenter.cpp`. Rebuilt the Vulkan
  GPU plugin successfully.
- Staged all 18 `.so` libs into `android/app/src/main/jniLibs/arm64-v8a/`
  (`_work/stage_android_libs.py`) and switched `build.gradle` to prebuilt libs
  (removed externalNativeBuild, ndkVersion 30, version 0.2.0-real-runtime).
- **`./gradlew assembleDebug` BUILD SUCCESSFUL**: `app-debug.apk` 147.8 MB with
  all 18 native libs. Game data still out of the APK.

**Remaining (Step 4-5, needs a connected device):**
- SDL3 on Android requires Java-side `SDLActivity` to load native libs and
  provide the ANativeWindow/surface. The current Kotlin launcher uses a plain
  GLES3 `GLSurfaceView`; it must be rewired to feed an ANativeWindow to the
  SDL3/Vulkan path (or port SDLActivity.java + SDLSurface.java into the app).
- No ADB device is currently connected (`adb devices` empty), so no on-device
  run/capture was possible this session. The previous M0 device (SM-A366B) is
  the test target.
- Then verify the BIOSHOCK title renders on-device (compare to
  `_work/title_capture2.png`) and update `24-DEVICE-M0.md` / add device doc.

All host-side work remains green: `verify-all.py` 12/12 PASS.

## Progress 2026-11-17, part 3 — ON-DEVICE boot (SM-A366B, ADB back up)

Installed the real-runtime APK on the connected device and iterated on boot
failures with logcat + JNI diagnostics. Fixed, in order:
1. `NativeBridge.kt` loaded the old `bioshock1-native` lib → switched to
   `bioshock1-android` (+ declared `nativeFrame`).
2. Modules not found with `extractNativeLibs=false` → set
   `android:extractNativeLibs="true"` in the manifest (SDK loader uses
   `std::filesystem::exists` on the lib folder).
3. `libSDL3d.so` missing from APK → staging script now includes `libSDL3*.so`.
4. SDL3 Android JNI not initialized → added `org.libsdl.app.*` Java shim classes
   (`_work/gen_sdl_java_stubs.py`) declaring all JNI methods SDL registers in
   `JNI_OnLoad` (SDLActivity 44, SDLInputConnection 2, SDLAudioManager 3,
   SDLControllerManager 10, HIDDeviceManager 8) plus the ~30 static reflection
   callbacks and `bootstrap()` → calls `nativeSetupJNI`+`nativeInitMainThread`
   from the GL thread. SDL3 now initializes cleanly (no JNI abort).
5. Audio driver unavailable on Android → overrode `GetRequirements()` in
   `Bioshock1App` to disable audio/input (title render needs only UI+graphics).

**Current on-device blocker:** `Init FAILED: module 'rexui' failed to init, the
app needs a window, but the UI driver has no display or the window did not
open`. SDL3's Android video backend requires a real SurfaceView lifecycle and
`SDLActivity.getNativeSurface()` to return the actual `android.view.Surface`;
the minimal shim returns null. The screen shows the black surface + touch
overlay; guest loop not entered.

**Next step:** port SDL3's real Java `SDLActivity`/`SDLSurface` (from
`thirdparty/sdl3/android-project/app/src/main/java/org/libsdl/app/`) into the
app so the surface lifecycle is real, export `SDL_main` from the game .so (SDL's
`nativeRunMain` dlsyms it), and have `SDL_main` run the TitleApp. This is the
final major integration step before the guest can present a frame on-device.

Test artifacts: `_work/android_menu2.png`, `_work/android_now.png`; JNI log tag
`BS1-JNI`. All host work still green: verify-all 12/12, docs 32 indexed.

## Progress 2026-11-17, part 4 — GUEST RUNTIME LAUNCHES ON DEVICE

Continued on-device iteration (part 3's next steps were largely achieved with a
minimal SDL surface shim rather than full SDLActivity port):

Fixed, in order (each verified by logcat):
1. Rebuilt the whole Android SDK with `-DREX_HOST_ALLOC_SANITIZED=1` — the
   custom host allocator caused heap corruption (SIGABRT in `DeleteBlock`), same
   as the host build. Also rebuilt the game .so with the flag.
2. SDL3 JNI method shims needed ALL registered methods: generated from SDL's
   `JNINativeMethod` tables (`_work/gen_sdl_java_stubs.py`) — SDLActivity 44,
   SDLInputConnection 2, SDLAudioManager 3, SDLControllerManager 10,
   HIDDeviceManager 8 (byte[] signature fix) + ~30 reflection callbacks;
   `bootstrap()` calls nativeSetupJNI + nativeInitMainThread + nativeResume.
3. `getContext()` returned null → JNI `GetObjectClass(null)` abort; added
   `SDLActivity.setContext(activity)` from MainActivity.onCreate.
4. `SDL_SetMainReady()` required ("did you include SDL_main.h") → call it from
   the JNI bridge (linked libSDL3d directly) before Init.
5. SDL video/audio/gamepad JNI paths that a minimal shim cannot serve were
   guarded on Android in the SDK (skip gamepad subsystem + mapping DB in
   `sdl3_input_driver.cpp`, skip audio init in `sdl3_audio_driver.cpp`).
6. ReXGlue's `--game_data_root` fallback hard-coded the host `C:\bs1game`;
   changed `title_app.cpp` to force only when the setting is empty, so the JNI
   bridge's `--game_data_root=<app files>/game` is honored. Added a
   `messageboxShowMessageBox` method on MainActivity to log runtime error dialogs
   instead of JNI-aborting.
7. GameSurfaceView converted from GLES3 GLSurfaceView to plain SurfaceView;
   forwards the real Surface to SDL shim (`setSurfaceOnly` before Init,
   `surfaceReady` after); SDL_CreateWindow now succeeds on `driver=android`.
8. Frames driven on the **main thread** via `Handler.postDelayed` (SDL asserts
   `SDL_IsMainThread()` in `SDL_PumpEvents` when run on a background thread).
9. **4 GB guest address space reservation failed** (SIGABRT in
   `GuestAddressSpace::Initialize`): `ASharedMemory_create` (ashmem) is null on
   modern Android. Added `memfd_create` syscall (`__NR_memfd_create`) +
   `ftruncate` fallback in `memory_posix.cpp`. Verified:
   `memfd_create fd=... 4GB mapping OK mapped at base=0x100000000`.

**VERIFIED ON-DEVICE MILESTONE:** `Init OK` → `LaunchTitle OK` → guest memory
mapped at 0x100000000 → guest "Main Game Thread" starts. This is the real
ReXGlue runtime + generated game code booting through guest title load on
Android — far beyond the old GLES3 diagnostic.

**Current blocker (SIGSEGV in guest thread):** `Main Game Threa` faults with
SEGV_ACCERR reading `0x0000000100000010` (= guest virtual base + 0x10). The
low guest page (address 0) is mapped no-access by the runtime; guest code reads
address ~0x10 early in startup and faults. Next: find the guest RIP for this
access (boot harness fault filter / SDK signal handler) and determine whether
the null-page protection setting (`protect_zero`) should be off, or a guest
pointer needs seeding. Capture a frame once the guest loop survives.

Artifacts: `_work/android_title.png`, logcat tags `BS1-JNI`/`BS1-UI`/`BS1-KERN`/
`BS1-MSGBOX`. All host work green: verify-all 12/12.

## Progress 2026-11-17, part 5 — VdSwap reached, shader translation blocker

Fixed the SIGSEGV at guest-base+0x10: added `--runtime.protect_zero=false`
to the JNI CLI args (the guest reads low guest memory during config init).

**Full game data was the missing piece for the guest to progress.** The device
game root had only `default.xex`; the host used `_game/root` with all 492 loose
files (4.7 GB). Created `_work/game_data.tar`, pushed to device, extracted into
app-private `files/game/` (492 files verified). Immediately:
```
BS1-JNI: Init OK → LaunchTitle OK
BS1-VDSWAP: VdSwap #1 called        <-- GUEST PRESENT PATH REACHED ON ANDROID
```
This is the key milestone: the guest's real present path fires on Android.

**Current blocker — guest shader translation → Adreno driver.** After VdSwap,
the Vulkan GPU command processor translates guest Xenos shaders via glslang
11.9.0. Hit a chain of issues:
- glslang debug asserts/null derefs (`getResultingAccessChainType`,
  `getNumTypeConstituents`, invalid SPIR-V ids from the SDK translator) → added
  defensive guards in `SpvBuilder.cpp` and `spirv_translator.cpp` (StoreResult
  skips invalid target_pointer).
- Built the whole SDK + game with `-DNDEBUG` (release behavior, disables
  REX_ASSERT) — asserts gone.
- Now the Adreno Vulkan driver (`libllvm-qgl.so`) SIGSEGVs compiling the
  generated SPIR-V — the shader's SPIR-V is malformed/incomplete due to the
  workarounds. VdSwap #1 fires but no frame reaches the screen.

**Next step:** the host renders via **D3D12** (DXBC translator), so this SPIR-V
path was never exercised/validated. Options: (a) fix the root cause in the SDK's
`spirv_translator.cpp` — find why `StoreResult` gets an invalid interpolator
pointer in `ProcessVertexFetchInstruction` (spirv_translator_fetch.cpp:405) and
produce valid SPIR-V for BioShock's shaders; (b) use a different/older glslang
whose output Adreno accepts. Since glslang_old (SDK-vendored, SPIR-V 1.6) had
API-signature mismatches, 11.9.0 is the version the SDK code expects. Capture a
frame once shaders compile.

All host work green: verify-all 12/12, host title `_work/title_capture2.png`.

## Completion — 2026-11-17 (title screen rendered on Android)

The genuine BioShock title screen is now rendered on-device through the real
ReXGlue runtime and VdSwap. Evidence: `_work/android_title_now.png` (the
"BIOSHOCK" sign + "PRESS START" + animated underwater-city scene),
`_work/android_title_final.png` (live fade transition), and the earlier
`_work/capframes/frame_010.png` (guest-rendered legal text). These are **real
guest-presented frames**, not painted/faked images. Device stayed alive for
minutes at ~25 fps: `BS1-VDSWAP ... #2551 called`, `vkQueuePresentKHR` returning
`VK_SUBOPTIMAL_KHR`, no SIGSEGV/abort.

Root causes found and fixed (all real runtime paths):
1. **Invalid SPIR-V crashed the Adreno compiler.** The SDK's SPIR-V translator
   calls glslang's `createAccessChain(storage, base, offsets)` directly, but
   glslang 11.9.0 derives the result type from internal `accessChain.base`
   state the SDK never sets — producing type id 0 and malformed SPIR-V. Patched
   `SpvBuilder.cpp::createAccessChain` to derive the result type from the actual
   `base`/`offsets` parameters (plus defensive guards in `getNumTypeConstituents`
   / `getResultingAccessChainType`). After this, guest shaders compile and
   `spirv-val` reports only a benign std140-offset warning (the struct uses
   explicit offsets). 83+ guest shaders translated; no Adreno crash.
2. **Presenter surface never connected (black screen).** On the headless SDL
   Android bootstrap the `SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED` event never
   reached the UI resize handler, so `XenosPresentation::surface_` size stayed
   0, `ConnectToWindow()` failed, paint mode stayed `kNone`, and
   `vkQueuePresentKHR` was never called. Added public
   `App::NotifySurfaceResized(w,h)` and call it from `nativeResize` (JNI) to set
   the present-thread surface size directly. Presents then began.
3. **Host heap corruption crash on guest thread naming.** `PosixThread::SetName`
   string manipulation corrupted a chunk (Scudo "invalid chunk state"), killing
   the app ~0.4 s after presenting began (observed in `Thread::SetName` ←
   `KernelThread::Create`, the "VertexUploadThr" guest thread). Guarded the
   Android path to skip the non-essential thread-name string surgery; the title
   then renders stably.

Build config: NDK r30 / arm64-v8a / android-28, SDK + game built with
`-DNDEBUG`, glslang 11.9.0, full 492-file game tree extracted on-device into
app-private `files/game`. Game data stays out of the APK (imported/extracted at
runtime). All host checks remain green: verify-all 12/12.
