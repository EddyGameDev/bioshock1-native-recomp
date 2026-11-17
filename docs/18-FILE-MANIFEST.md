# File manifest and change log

## 1. Project tree (setup-session result)

```
BioShock1-Android-Recomp/
├── README.md                         (project root readme)
├── .gitignore                        (ignores _game, _reference, build, recomp/generated)
├── docs/                             19 documents (00-START-HERE ... 18-FILE-MANIFEST)
│   ├── 00-START-HERE.md
│   ├── 01-ARCHITECTURE.md
│   ├── 02-TOOLCHAIN.md
│   ├── 03-XEX-ANALYSIS.md
│   ├── 04-PIPELINE.md
│   ├── 05-BUILDING.md
│   ├── 06-HLE-KERNEL.md
│   ├── 07-GPU-XENOS-GLES3.md
│   ├── 08-LAUNCHER.md
│   ├── 09-RE-FINDINGS.md
│   ├── 10-STATUS.md
│   ├── 11-ROADMAP.md
│   ├── 12-TROUBLESHOOTING.md
│   ├── 13-LEGAL.md
│   ├── 14-GLOSSARY.md
│   ├── 15-REFERENCES.md
│   ├── 16-BIOS-AND-XDK.md
│   ├── 17-TESTING.md
│   ├── 18-FILE-MANIFEST.md
│   ├── 19-WORKLOG.md
│   ├── 20-REXGLUE-V011.md
│   └── 21-HLE-IMPORT-MATRIX.md
├── tools/
│   ├── vendor_extract.py             unpack 12 toolchain archives -> third_party/
│   ├── xdvdfs.py                     ISO/XDVDFS list/find/extract
│   ├── xex_inspect.py                XEX2 header -> config/xex_analysis.json
│   ├── pipeline.py                   inspect/configure/codegen/build orchestration
│   ├── stage_generated.py            codegen output -> recomp/generated/
│   ├── gen_import_stubs.py           216 import names -> __imp__* stubs
│   └── codegen-validate/             CMake target: compile all generated TUs
├── config/
│   ├── xex_analysis.json             REAL analysis of your default.xex
│   ├── hle_imports.json              216 measured kernel/XAM import names
│   └── bioshock1.toml                generated recompiler config with TODOs
├── third_party/                      12 vendored toolchains (see 02-TOOLCHAIN.md)
├── native/
│   ├── CMakeLists.txt                host selftest + Android SHARED targets
│   ├── include/
│   │   ├── ppc_context.h             Xenon guest CPU state
│   │   ├── hle_kernel.h              HLE dispatch + guest memory API
│   │   ├── gpu_layer.h               GLES3 GPU interface
│   │   └── engine.h                  flat C API for JNI
│   └── src/
│       ├── engine.cpp                init/frame/control/status implementation
│       ├── hle_kernel.cpp            registry, 8 bootstrap imports, fail-soft dispatch
│       ├── gpu_layer.cpp             working GLES3 diagnostic renderer
│       ├── jni_bridge.cpp            JNI exports for Kotlin
│       └── selftest.cpp              host smoke test
├── android/
│   ├── build.gradle, settings.gradle, gradle.properties, local.properties
│   ├── gradlew.bat, gradle/wrapper/* Gradle 8.6 wrapper
│   └── app/
│       ├── build.gradle              AGP 8.4.2, SDK 34, NDK 26.3, arm64-v8a
│       └── src/main/
│           ├── AndroidManifest.xml
│           ├── java/com/bioshock1/recomp/
│           │   ├── MainActivity.kt       SAF import + menu + start
│           │   ├── Xdvdfs.kt             on-device ISO reader
│           │   ├── GameSurfaceView.kt    GLES3 surface + render loop
│           │   ├── TouchControlsView.kt  multi-touch overlay -> axes/buttons
│           │   └── NativeBridge.kt       JNI declarations
│           └── res/                      layout, strings, styles, launcher icons
├── patches/                          (empty; runtime overlays go here)
├── assets/                           (empty; future icons/shaders for the app)
├── recomp/generated/                 (empty; codegen output goes here, gitignored)
├── _game/                            YOUR ISO + extracted default.xex (gitignored)
├── _reference/                       xbox360-bios-v10.bin (gitignored, analysis only)
├── build/host/                       host CMake build (gitignored)
└── scripts/install-toolchain.ps1     reproducible Android SDK/NDK/CMake install
```

## 2. Change log — setup session (2026-11-17)

### Added

* Full project scaffold with 18 created directories.
* `vendor_extract.py`; vendored 12 toolchains into `third_party/`.
* `xdvdfs.py`; parsed XGD2 partition `0x0FD90000`, extracted `default.xex`.
* `xex_inspect.py`; XEX2 header parser with Xenia-compatible IDs.
* Corrected title-ID decoding (ASCII `5454`+`07D8` → `0x545407D8`).
* Corrected import parsing (raw thunk/descriptor pairs; no fake ordinals).
* `pipeline.py` with six stages plus `all`.
* Generated `config/xex_analysis.json` and `config/bioshock1.toml`.
* Native engine skeleton + GLES3 diagnostic renderer + JNI bridge + selftest.
* Kotlin launcher with SAF import, full on-device XDVDFS extraction, GLES3
  surface and a multi-touch control overlay.
* `scripts/install-toolchain.ps1`; installed Android SDK platform 34,
  build-tools 34.0.0, platform-tools, NDK 26.3.11579264, CMake 3.22.1.
* Accepted SDK licences via `sdkmanager --licenses`.
* This 31-file documentation suite.

### Fixed during verification

* Stale `JAVA_HOME` → `gradlew.bat` now falls back to `java` on PATH.
* Missing `gradle-wrapper.properties` copied from the vendored reference.
* AGP's default NDK 26.1 → pinned `ndkVersion '26.3.11579264'`.
* Kotlin type errors in `Xdvdfs.kt` (`Long`→`Int`, public API exposing a
  private `Entry` type).

### Phase 1 follow-up (same day, Session 2)

* Downloaded prebuilt ReXGlue v0.11.0 (`scripts/install-rexglue.ps1`).
* Scaffolded `_work/rexglue-bioshock1`, ran `rexglue codegen`: 308 files,
  156.5 MB, 36,625 functions, no warnings.
* Validated all 152 generated TUs on host (clang++ 23) and on Android arm64
  (NDK r28c + `native/include/compat/atomic_ref.h`).
* Added `tools/stage_generated.py`, `tools/gen_import_stubs.py`,
  `tools/codegen-validate/`, `config/hle_imports.json`,
  `config/runtime` symbol notes in `21-HLE-IMPORT-MATRIX.md`.
* Added name-based HLE dispatch + 216 generated `__imp__*` stubs.
* Docs grew from 19 to 29 files (`19-WORKLOG.md`, `20-REXGLUE-V011.md`,
  `21-HLE-IMPORT-MATRIX.md`, `22-PHASE2-LINK.md`, `23-VERIFICATION.md`,
  `24-DEVICE-M0.md`, `25-PHASE3-RENDERER.md`, `26-DOCS-WEBSITE.md`,
  `27-ABI-HELPERS.md`, `28-HLE-IMPORTS.md`).

### Phase 2 start (same day, Session 3)

* Generated `config/runtime_symbols.json` (245 symbols) via
  `tools/extract_runtime_symbols.py` and `native/src/runtime_stubs.cpp` via
  `tools/gen_runtime_stubs.py`.
* Fixed import-stub ABI: named handlers now receive `(PPCContext&, base)`.
* Decrypted/decompressed the PE with `extract_pe.py`; guest entry VA confirmed
  as `0x8219D698`, mapped to generated `__imp__xstart` (no runtime stub needed).
* Linked `libbs1_link_test.so` (139.14 MB) with `-Wl,-z,defs`: all 152 generated
  arm64 objects + engine/HLE/stubs, zero unresolved symbols.

### Verified

* `python tools/pipeline.py inspect` → real game data.
* `python tools/pipeline.py configure` → TOML written.
* `python tools/pipeline.py build-native` → `selftest OK`.
* `cd android && .\gradlew.bat assembleDebug` → `BUILD SUCCESSFUL`.
* APK is 8.77 MB and contains no `.iso`/`.xex`/`.fsb`/`.u` game data.

## 3. Versioning scheme

The project is pre-1.0. Version strings in `app/build.gradle`:
`versionCode 1`, `versionName "0.1.0-static-recomp"`.

Suggested milestones:

* `0.1.x` — scaffold + analysis + diagnostic renderer (current)
* `0.2.x` — codegen toolchain working, first host boot
* `0.3.x` — first Android boot to title screen
* `0.4.x` — in-game rendering
* `0.5.x` — audio + playable
* `1.0` — complete playthrough with documented issues

Update `10-STATUS.md` and this changelog together with each version bump.
