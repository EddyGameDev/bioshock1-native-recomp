# Applying the Android port patches

These patches capture the modifications to the **upstream ReXGlue SDK source**
that were required to reach the real BioShock title screen on Android. They are
diffs against the vendored checkout, not against this repository.

## Prerequisites

Clone the SDK source (the host recompiler CLI is fetched separately by
`scripts/install-rexglue.ps1`):

```powershell
git clone https://github.com/rexglue/rexglue-sdk.git third_party/rexglue-git
git -C third_party/rexglue-git submodule update --init --recursive
```

Check out the same base revisions this project used (the submodule pointers in
the SDK repo's history) before applying the patches.

## Patches

1. **`rexglue-sdk-android-port.patch`** — modifications to the SDK source tree
   (kernel, Vulkan GPU backend, presenter, SDL3 UI, app, platform/Cmake files):

   ```powershell
   git -C third_party/rexglue-git apply --3way patches/rexglue-sdk-android-port.patch
   ```

2. **`glslang-android-port.patch`** — fixes SPIR-V generation in the glslang
   submodule (valid `OpAccessChain` result types + defensive type-id guards):

   ```powershell
   git -C third_party/rexglue-git/thirdparty/glslang apply --3way patches/glslang-android-port.patch
   ```

If `--3way` reports conflicts, apply with `git apply --reject` and resolve by
hand; the patch is small and each hunk is documented in
`docs/32-GOAL-ANDROID-TITLE.md`.

## What the patches do (summary)

- `SpvBuilder.cpp`: `createAccessChain` now derives the result type from its
  `base`/`offsets` parameters instead of unset internal access-chain state.
- `spirv_translator.cpp`: declare all interpolators by default (DXBC parity) and
  skip stores to invalid targets.
- `app.h` / `app.cpp`: add `App::NotifySurfaceResized(w,h)` so the JNI bridge can
  connect the presenter's surface under the headless SDL bootstrap.
- `vulkan_presenter.cpp` / `presenter.cpp` / `command_processor.cpp`: optional
  `BS1-*` logcat diagnostics on Android.
- `threading_posix.cpp`: skip non-essential `std::string` thread-name surgery on
  Android (host heap-corruption workaround).
- `user_profile.cpp`: swallow filesystem exceptions when saving Xbox profile
  settings, which otherwise throw an uncaught exception and kill the guest when
  advancing past the main menu on Android.
- `xboxkrnl_rtl.cpp`: warn instead of aborting (`REX_ASSERT`) on cross-thread
  `RtlLeaveCriticalSection` / contended `RtlEnterCriticalSection` so guest
  loading/streaming threads release locks cleanly instead of crashing.
- `xboxkrnl_io_info.cpp` (`NtSetInformationFile` / `XFileRenameInformation`):
  strip leading `\??\` prefixes and handle the special `ObDosDevices`
  (`0xFFFFFFFD`) `root_dir_handle` by resolving relative target names into the
  source file's parent directory. Required for BioShock's save-game commit step
  (`Save.tmp` -> `DLContentInfo.pkg`).
- `host_file_posix.cpp` (`MoveNoReplace`): bypass the POSIX `link()` hard-link
  path on `__ANDROID__` (which Android SELinux blocks with `EACCES` on app-private
  filesystems) and use `lstat()` + `rename()` instead so in-game saving succeeds
  on Android.
- `xam_user.cpp` (`XamUserWriteProfileSettings`): persist `kInt32`, `kInt64`,
  `kFloat`, and `kDouble` profile settings alongside binary/Unicode settings.
- `sdl3_audio_driver.cpp`: replace the Android no-op stub with a dynamic
  `dlopen("libaaudio.so")` native 48 kHz float32 stereo AAudio backend so in-game
  XMA audio plays on Android devices.
- **Custom renderer (Windows D3D11 + Android GLES 3.2):** a Lanczos2 guest-output
  upscale shader (`host/shaders/bytecode/d3d12_5_1/guest_output_lanczos_ps.h`,
  source under `renderer/shaders/`) plus the standalone `rexgpu-xenos-custom`
  module (`renderer/`) providing live no-restart internal resolution scaling on
  Windows (`360p`–`2160p`) and Android (`360p`–`1440p`).
- Various CMake/platform files: Android surface support, `log` linkage, NDEBUG
  for the SPIR-V target.

After applying, build the SDK for Android arm64 and then the game library as
described in `docs/05-BUILDING.md`.
