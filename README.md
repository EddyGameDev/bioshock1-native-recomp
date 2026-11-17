# BioShock 1 — Native ReComp (Windows + Android)

> **Release `v0.0.1-experimental`** — the genuine BioShock 1 title screen renders
> on Android through a **static recompilation** pipeline and the **real ReXGlue
> runtime**, presented by the guest `VdSwap` path. This is an **experimental
> proof-of-concept**: the title screen and intro are verified; the game is **not
> yet fully playable**.

| Platform | Status | Evidence |
|---|---|---|
| Windows (host recompiler) | ✅ Real title screen rendered via ReXGlue v0.11.0 + D3D12 | [`docs/screenshots/host-title.png`](docs/screenshots/host-title.png) |
| Android (arm64-v8a, Vulkan) | ✅ Real title screen rendered via ReXGlue + Vulkan + `VdSwap` | [`docs/screenshots/android-title.png`](docs/screenshots/android-title.png) |

![BioShock title screen rendered on Android](docs/screenshots/android-title.png)

*The on-device result: the genuine "BIOSHOCK" title sign, "PRESS START", and the
animated underwater-city background — produced by guest-presented frames, **not**
a painted or faked image. The round overlay buttons are the optional SDL touch
controls.*

---

## What this is

A host-side **static recompilation** pipeline plus launchers for **Windows** and
**Android** that turn *your legally-owned* Xbox 360 BioShock disc into native
code:

- **No emulator loop, no JIT.** PPC guest code is translated ahead-of-time into
  C++ (`recomp/generated/`), compiled for the target, and executed natively.
- **The real runtime.** Rendering, kernel, HLE imports and the filesystem come
  from [ReXGlue SDK v0.11.0](https://github.com/rexglue/rexglue-sdk), not a
  hand-written approximation.
- **Guest-presented frames.** The title appears through the real Xenos GPU
  command processor → `VdSwap` → `vkQueuePresentKHR` path.
- **No game data in the APK.** The launcher asks you to import your own ISO;
  the XEX and full game tree are extracted at runtime into app-private storage.
  Nothing proprietary is committed or shipped.

## What this is *not*

- Not a full game — expect the title screen, intro/legal screens and early boot
  only. Gameplay is unverified.
- Not an emulator — there is no interpreter or JIT fallback.
- Not a distribution of BioShock, its assets, the Xbox 360 BIOS, or any
  Microsoft material. **You must own a legal copy of the game.**

---

## How it works

```
  Xbox 360 ISO (yours)
        │  tools/pipeline.py inspect
        ▼
  default.xex  →  XEX analysis JSON  →  recompiler config (config/)
        │  ReXGlue recompiler (rexglue.exe)
        ▼
  recomp/generated/  (PPC → C++ translation units)
        │
        ├──────── Windows ────────►  native/ + ReXGlue D3D12 runtime
        │                              → bioshock1.exe  → real title screen
        │
        └──────── Android ────────►  JNI bridge (_work/rexglue-android-app/)
                                     + ReXGlue Vulkan runtime (arm64)
                                     → app-debug.apk  → real title screen
```

The Android launcher is a Kotlin `SurfaceView` app that forwards the native
surface to SDL3 and pumps frames from JNI. The native side hosts the real
ReXGlue `TitleApp`: kernel, 4 GB guest address space, Vulkan GPU backend, and
the generated PPC→C++ code. Game files are imported via the Android document
picker and extracted into `files/game/`.

Key technical fixes required to reach the title screen (all documented and
shipped as patches in [`patches/`](patches/)):

1. **SPIR-V translator** — glslang's `createAccessChain(storage, base, offsets)`
   derived its result type from internal state the SDK never initializes,
   producing invalid SPIR-V that crashed the Adreno driver. Patched to derive
   the type from the actual `base`/`offsets` arguments.
2. **Presenter surface connection** — under the headless SDL bootstrap the
   resize event never reached the UI layer, so presents were silently skipped.
   Added `App::NotifySurfaceResized()` and called from the JNI `nativeResize`.
3. **Host allocator stability** — a `std::string` free in `PosixThread::SetName`
   corrupted the heap on a guest thread; the Android path now skips the
   non-essential thread-name surgery.

See [`docs/32-GOAL-ANDROID-TITLE.md`](docs/32-GOAL-ANDROID-TITLE.md) for the
full debugging story and evidence.

---

## Quick start

### Prerequisites

- **Windows:** PowerShell 7, Python 3.11+, CMake 3.25+, MinGW or MSVC, Java 17+
  (for the Android build), Android SDK + NDK r30.
- **Android device:** arm64-v8a, Android 9+, a Vulkan-capable GPU. Tested on a
  Samsung SM-A366B (Adreno, Android 16).
- **Game:** your own BioShock 1 Xbox 360 ISO. `default.xex` is 13,021,184 bytes
  (title `0x545407D8`, XDK 5426).

### 1. Fetch dependencies

The large third-party toolchains and the ReXGlue SDK are **not vendored in this
repository** (see [`third_party/README.md`](third_party/README.md)). Run the
provided installers:

```powershell
python tools/pipeline.py inspect        # ISO → default.xex → analysis JSON
python tools/pipeline.py configure      # → config/bioshock1.toml
./scripts/install-toolchain.ps1         # fetch codegen toolchain into tools-bin/
./scripts/install-rexglue.ps1           # fetch ReXGlue v0.11.0 SDK
```

Apply the Android port patches from [`patches/`](patches/PATCHES.md) to a clean
`third_party/rexglue-git` checkout before building for Android.

### 2. Generate the recompiled code and build the Windows launcher

```powershell
python tools/pipeline.py build-native   # host selftest + native build
build\bs1-real\bioshock1.exe            # renders the real title screen
```

### 3. Build and install the Android launcher

```powershell
python tools/pipeline.py build-apk
adb install -r android\app\build\outputs\apk\debug\app-debug.apk
```

On first launch, tap **IMPORT ISO**, pick your BioShock ISO, wait for the full
game tree to extract, then tap **START GAME**. The title sequence plays shortly
after. Game data never leaves your device.

---

## Repository layout

| Path | Contents |
|---|---|
| `native/` | Windows host launcher: PPC context, HLE kernel, Xenos PM4, JNI bridge helpers |
| `recomp/` | Generated PPC→C++ output (generated files are gitignored) |
| `android/` | Android Kotlin launcher (`app/src/main/java`), manifest, resources, Gradle build |
| `tools/` | Host pipeline: ISO/XEX parsing, config generation, build orchestration |
| `scripts/` | Toolchain installers, verification, docs indexer |
| `config/` | Real XEX analysis JSON + generated recompiler TOML |
| `patches/` | ReXGlue SDK + glslang patches required for Android rendering |
| `docs/` | 32 detailed documents, including the goal/worklog and this page's evidence |
| `third_party/` | Dependency notes only — toolchains are fetched at build time |

---

## Documentation

Start at [`docs/00-START-HERE.md`](docs/00-START-HERE.md). Highlights:

| Doc | Topic |
|---|---|
| [`01-ARCHITECTURE.md`](docs/01-ARCHITECTURE.md) | How the pieces fit; why static recompilation |
| [`02-TOOLCHAIN.md`](docs/02-TOOLCHAIN.md) | Every tool and its role |
| [`03-XEX-ANALYSIS.md`](docs/03-XEX-ANALYSIS.md) | Real facts from the game dump |
| [`05-BUILDING.md`](docs/05-BUILDING.md) | Exact build commands |
| [`10-STATUS.md`](docs/10-STATUS.md) | Honest, verified status |
| [`19-WORKLOG.md`](docs/19-WORKLOG.md) | Full incremental journal (sessions 1–16) |
| [`31-GOAL-TITLE-SCREEN.md`](docs/31-GOAL-TITLE-SCREEN.md) | Windows title-screen breakthrough |
| [`32-GOAL-ANDROID-TITLE.md`](docs/32-GOAL-ANDROID-TITLE.md) | Android title-screen breakthrough |
| [`13-LEGAL.md`](docs/13-LEGAL.md) | Legal notes |

Verification is green: `python scripts/verify-all.py` → **12/12 checks passed**.

---

## Releases

Releases follow an **experimental** pre-1.0 scheme:

- **`v0.0.1-experimental`** — First verified guest-rendered title screen on both
  Windows and Android. Source snapshot; binaries are built locally from source.

See the [Releases page](../../releases) for notes.

---

## Legal

No game data, BIOS, ISO, XEX, or proprietary Microsoft material is included or
shipped in this repository or its releases. The included screenshots are
documentation evidence of the port, not game assets. You must own a legal copy
of BioShock 1 to use this project. See [`docs/13-LEGAL.md`](docs/13-LEGAL.md).

ReXGlue SDK and Xenia-derived runtime code are BSD-3-Clause licensed (see the
upstream project); this project's original code follows the same license unless
noted otherwise.
