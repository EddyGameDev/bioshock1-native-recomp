# Android launcher

The launcher is a native Kotlin app (`android/`), deliberately **separate**
from the game data — the "launcher, not embed" pattern used by the MGS3
reference port. It contains no ISO, XEX, BIOS or recompiled game code beyond
the generic engine library it loads at runtime.

## 1. Components

| File | Responsibility |
|---|---|
| `AndroidManifest.xml` | GLES 3.0 requirement, landscape, gamepad optional, main activity |
| `MainActivity.kt` | Menu UI, SAF ISO import, on-device XDVDFS extraction, start game |
| `Xdvdfs.kt` | Kotlin XDVDFS reader; locates game partition, extracts `default.xex` and the full game tree |
| `GameSurfaceView.kt` | `GLSurfaceView` with EGL 3.0; initialises native engine on context creation; renders each frame |
| `NativeBridge.kt` | JNI declarations; `System.loadLibrary("bioshock1-native")` |
| `TouchControlsView.kt` | Translucent multi-touch overlay: left stick, face buttons, START → axes/buttons arrays |
| `res/layout/activity_main.xml` | Overlay menu: status text, Import ISO, Start Game |
| `app/build.gradle` | AGP 8.4.2, Kotlin 1.9.24, SDK 34, NDK 26.3.11579264, `arm64-v8a` only |

## 2. First-run flow

```
MainActivity.onCreate
  ├─ status: engine library present? XEX extracted? HLE import count?
  ├─ [Import ISO] -> ACTION_OPEN_DOCUMENT
  │    ├─ copy selected ISO to <filesDir>/game/BioShock.iso
  │    ├─ Xdvdfs.find("default.xex")  (XGD2 partition 0x0FD90000)
  │    ├─ Xdvdfs.extract -> <filesDir>/game/default.xex
  │    └─ Xdvdfs.extractAll -> <filesDir>/game/  (whole tree, with progress)
  └─ [Start Game]
       ├─ add GameSurfaceView to the container
       ├─ GLSurfaceView creates an EGL3 context
       ├─ NativeBridge.nativeInit(gameDir, 1280, 720)
       └─ per frame: NativeBridge.nativeRender()
```

The file copy and extraction run on `Dispatchers.IO` via a coroutine; the UI
stays responsive even though the ISO is ~7.8 GB. Both `default.xex` and the
**whole XDVDFS tree** (Unreal `.u` packages, FMOD `.fsb` banks, etc.) are
extracted into `<filesDir>/game/`, with file/byte progress shown in the status
text. This satisfies the guest-filesystem requirement in
`docs/06-HLE-KERNEL.md`.

## 3. JNI contract

`NativeBridge.kt` ↔ `native/src/jni_bridge.cpp`:

| Kotlin | Native C API | Purpose |
|---|---|---|
| `nativeInit(root, w, h)` | `bs_engine_init` | init HLE, map image, init GLES3 |
| `nativeShutdown()` | `bs_engine_shutdown` | release GPU + guest memory |
| `nativeResize(w, h)` | `bs_engine_resize` | glViewport on rotation |
| `nativeRender()` | `bs_engine_render` | one frame (`gpu::present`) |
| `nativeSetControls(axes, buttons)` | `bs_engine_set_controls` | input bridge |
| `nativeStatus()` | `bs_engine_status_json` | JSON stats for debug overlay |
| `nativeHleImportCount()` | `bs_engine_hle_import_count` | menu status line |

JNI names follow `Java_com_bioshock1_recomp_NativeBridge_*`; keep the Kotlin
package and the C function names in sync when refactoring.

## 4. Why SAF instead of a storage permission

`ACTION_OPEN_DOCUMENT` grants a temporary URI grant to a user-selected file.
On API 28+ this needs no `READ_EXTERNAL_STORAGE` permission. The launcher copies
the selected ISO into its own private storage, so subsequent launches do not
need the URI again. This avoids the scoped-storage problems that plague
emulators on modern Android.

## 5. Graphics context details

* `setEGLContextClientVersion(3)` requests GLES 3.0; the manifest declares
  `android:glEsVersion="0x00030002"`.
* `RENDERMODE_CONTINUOUSLY` drives the frame loop. Once the recompiled game is
  integrated, this should become `RENDERMODE_WHEN_DIRTY` driven by the guest's
  `VdSwap` present signal to avoid rendering faster than the game.
* `onDetachedFromWindow()` calls `nativeShutdown()` so guest memory is freed.

## 6. Touch controls — current and planned

`TouchControlsView.kt` implements a translucent multi-touch overlay layered
over the GLES3 surface: a left analogue stick (axes 0/1), four face buttons,
and START, sending `FloatArray`/`ByteArray` to `nativeSetControls()`. This is
the same pattern as the reference ports (MGS3, Sonic, DBZ). The target mapping is:

| Control | Mapping |
|---|---|
| Left stick | move |
| Right stick | look / aim |
| D-pad | weapon / inventory selection |
| Face buttons | jump, use, reload, melee |
| L1/R1, L2/R2 | shoulder / trigger analogues |
| START | pause / menu |

BioShock also needs plasmid selection and a weapon wheel; map those to a
long-press or a swipe on the touch overlay. Physical gamepads should take
priority via `InputDevice` when connected (`android.hardware.gamepad` is
declared optional in the manifest).

## 7. Debug overlay

`bs_engine_status_json()` returns live counters:

```json
{"initialized":true,"hleImports":8,"missingImports":0,"frames":1234,"draws":1234,"textures":12}
```

A future build can render this as a translucent `TextView` over the surface
(the `Xbox360RecompAndroid` `metrics_overlay.dart` is a good UI reference).
During early boot, also surface `missing_imports()` names — they are the most
valuable debugging signal available.

## 8. Packaging guarantees

Verified with the procedure in `05-BUILDING.md` §8: the APK contains no
`.iso`, `.xex`, `.bin` (BIOS), `.fsb` or `.u` files. The only binary payload is
`libbioshock1-native.so`, which is our own engine code. Game data stays on the
user's device under app-private storage.
