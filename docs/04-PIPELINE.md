# End-to-end pipeline

This document describes every stage from your game dump to an installable APK,
what commands run it, and what artefacts each stage produces.

## Stage 0 — One-time environment

```
scripts/install-toolchain.ps1
```

Installs (no admin rights, into `F:\tools\android-sdk`):
Android command-line tools, platform 34, build-tools 34.0.0, platform-tools,
NDK 26.3.11579264, CMake 3.22.1. It also writes the accepted SDK licence
hashes so Gradle can auto-provision anything it still needs.

Python 3.14, Git, Java 21, Clang 23 and Ninja were already present. `py7zr`
reads the `.7z` game dump.

## Stage 1 — Game data preparation

```
python tools/pipeline.py inspect
python tools/pipeline.py extract-game        # list all files in the ISO
```

What happens:

1. `py7zr` decompresses `BioShock (World)....7z` once into `_game/BioShock....iso`.
2. `tools/xdvdfs.py` finds the XGD2 game partition at `0x0FD90000`, parses the
   XDVDFS root B-tree, and streams `default.xex` (13,021,184 bytes) to
   `_game/default.xex`.
3. `tools/xex_inspect.py` reads the cleartext XEX2 headers and writes
   `config/xex_analysis.json` (title `0x545407D8`, XDK 5426, base `0x82000000`,
   427 import records).

This stage never decrypts or ships game code; it only reads container metadata
and copies the XEX bytes you already own into a gitignored working folder.

**Outputs:** `_game/default.xex`, `config/xex_analysis.json`.

## Stage 2 — Configuration generation

```
python tools/pipeline.py configure
```

Combines the analysis JSON with the recompiler templates to write
`config/bioshock1.toml`: base address, XDK build, import-record count and a
`TODO`-marked skeleton for the game-specific analysis values (function
boundaries, ABI helper addresses, mid-asm hooks). Keeping this file generated
and text-based makes a rebuild reproducible and reviewable.

**Outputs:** `config/bioshock1.toml`.

## Stage 3 — Reverse engineering (manual, the real work)

Fill the `TODO` values in the TOML and, for ReXGlue, the project manifest:

* decompressed PE section ranges and entry point;
* `.text` function boundaries and jump table TOML from XenonAnalyse;
* `__restgprlr*`/`__savegprlr*`/VMX helper addresses (byte patterns in
  `09-RE-FINDINGS.md`);
* import ordinal → HLE handler bindings;
* mid-asm hooks for the renderer and input.

The `rexglue-knowledges` docs and `360tools` harness (`recomp_harness.py`)
automate the bulk of the mechanical parts and produce a compatibility matrix
and failure catalogue. See `02-TOOLCHAIN.md`.

## Stage 4 — Code generation (host only)

```
python tools/pipeline.py codegen
```

`pipeline.py` looks for a built recompiler in this order:

1. `third_party/rexglue-sdk/build/rexglue` → `rexglue codegen`
2. `third_party/XenonRecomp/build/XenonRecomp/XenonRecomp` →
   `XenonRecomp <config.toml> <ppc_context.h>`

If neither binary exists, it prints the exact commands instead of pretending
to succeed. ReXGlue is the recommended path: it decrypts/decompresses the XEX
itself and resolves imports. XenonRecomp needs an already-decompressed XEX and
a finished TOML.

**Outputs:** `recomp/generated/*.cpp`, `*.h` (PPC blocks, function table,
import stubs, jump tables). These files are not checked in and are gitignored.

## Stage 5 — Native build

### Host smoke test (no device needed)

```
python tools/pipeline.py build-native
```

Configures `native/` with CMake + Ninja, builds `bioshock1-selftest`, and runs
it. It verifies: the engine API initialises, the HLE dispatch table registers
bootstrap imports, an unknown import is recorded without crashing, and the
frame counter advances. This ran successfully during project setup.

### Android native library

Gradle invokes CMake with the NDK toolchain while assembling the APK:

```
native/CMakeLists.txt  ->  libbioshock1-native.so (arm64-v8a)
```

`BS_GLES3` enables the real GLES3 renderer; once codegen output exists, add the
generated `.cpp` files to the `bioshock1-native` target (documented inline in
`native/CMakeLists.txt`).

## Stage 6 — APK assembly

```
python tools/pipeline.py build-apk
```

Equivalent to:

```
cd android
.\gradlew.bat assembleDebug --no-daemon
```

Produces `android/app/build/outputs/apk/debug/app-debug.apk`. The APK contains
the launcher, the native library and standard resources. **No ISO, XEX or
BIOS bytes.** Install with:

```
adb install -r android\app\build\outputs\apk\debug\app-debug.apk
```

## Stage 7 — Runtime on the device

1. Launcher opens; tap **Import ISO**.
2. Android SAF file picker selects your BioShock ISO.
3. Kotlin `Xdvdfs.kt` copies it into `/data/data/.../files/game/BioShock.iso`,
   extracts `default.xex`, then streams the **whole XDVDFS tree** (`.u` packages,
   `.fsb` banks) into `files/game/` so guest file reads resolve.
4. Tap **Start Game**: a GLES3 `GLSurfaceView` is created, `nativeInit()` maps
   the image and creates the renderer, and `nativeRender()` runs each frame.
5. Touch/gamepad input is queued through `nativeSetControls()` into the guest
   XINPUT state.

Currently step 4 shows the GLES3 diagnostic pattern, because the generated PPC
code and GPU command translator are not yet integrated. That is the intentional
verification point described in `10-STATUS.md`.

## Reproducibility checklist

- [ ] Toolchain installer re-run on a clean machine
- [ ] ISO present in `_game/`
- [ ] `pipeline.py inspect` succeeds, title ID `0x545407D8`
- [ ] `pipeline.py configure` writes a TOML with no silent value changes
- [ ] `pipeline.py build-native` passes the self-test
- [ ] `pipeline.py build-apk` produces an APK
- [ ] On-device: ISO import extracts `default.xex`, GLES3 test pattern renders
