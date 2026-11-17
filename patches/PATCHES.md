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
- Various CMake/platform files: Android surface support, `log` linkage, NDEBUG
  for the SPIR-V target.

After applying, build the SDK for Android arm64 and then the game library as
described in `docs/05-BUILDING.md`.
