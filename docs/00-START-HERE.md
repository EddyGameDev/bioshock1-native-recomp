# BioShock 1 — Native Android Static Recompilation

**START HERE.** This is the master index and honest status statement for the
project. Read this before touching any tool.

---

## What this project is

A host-side **static recompilation** pipeline plus an Android launcher that
turns an Xbox 360 game (BioShock, World region) into C++ once on a PC, then
builds that C++ into a native `arm64-v8a` library executed directly on Android.

There is **no emulator loop, no JIT, and no interpreter** in the final design.
The PowerPC code is translated ahead of time; only the runtime (kernel HLE,
memory, input, graphics) executes on the device. This is the same class of
project as the reference ports in the parent folder:

| Reference | Console | Recompiler | Launcher |
|---|---|---|---|
| Sonic Advance 2 | GBA | gbarecomp | Kotlin/Flutter-style launcher |
| DBZ: Legacy of Goku | GBA | gbarecomp | Kotlin launcher |
| Contra: Legacy of War | PS1 | psxrecomp | recomp-ui |
| Metal Gear Solid 3 | PS2 | PS2Recomp | Kotlin launcher (not embedded) |
| **BioShock 1 (this)** | **Xbox 360** | **ReXGlue / XenonRecomp** | **Kotlin launcher (not embedded)** |

---

## Honest status (read this)

This repository is a **complete, buildable engineering scaffold** with the
real game analysed and the full toolchain vendored. It is **not yet a playable
game**, and it is important to understand why:

1. **Codegen is host-only and needs analysis input.** ReXGlue/XenonRecomp need
   game-specific function boundaries, jump tables, ABI helper addresses, and
   import bindings. Those are produced by reverse engineering a *specific*
   XEX build. This project's `config/bioshock1.toml` is generated from the real
   XEX but deliberately leaves those values marked `TODO`.
2. **The Android runtime is a skeleton.** The HLE kernel implements a small
   bootstrap import set; the GLES3 layer renders a diagnostic pattern. The
   Xenos→GLES3 command/shader translator and the full XDK surface are not
   implemented.
3. **BioShock is a hard target.** It is a large, unoptimised-era Unreal
   Engine 2.5 title ("Vengeance"), ~12.8 MB of packed XEX image, heavy XMA
   audio, and a complex D3D9/Xenos renderer. The public ReXGlue ports listed
   in its README are smaller XBLA titles; even *Saints Row (360)* is only at
   "Planning". Expect this to be a multi-month reverse-engineering effort.

Everything that **can** be verified here **is** verified: the ISO was read,
the real `default.xex` was extracted and parsed, the native runtime compiles
and passes a host self-test, and an Android APK is assembled by Gradle.

---

## Directory map

```
BioShock1-Android-Recomp/
├── docs/               <- you are here; 19 detailed documents
├── tools/              <- host-side Python pipeline (no third-party deps)
│   ├── xex_inspect.py    XEX2 header -> JSON
│   ├── xdvdfs.py         ISO/XDVDFS reader + extractor
│   ├── pipeline.py       orchestrates inspect/codegen/build
│   └── vendor_extract.py unpacks the downloaded toolchain zips
├── config/             <- generated analysis + recompiler config
│   ├── xex_analysis.json       REAL data from your game dump
│   └── bioshock1.toml          ReXGlue/XenonRecomp config skeleton
├── third_party/        <- 12 vendored toolchain repos (see 02-TOOLCHAIN.md)
├── native/             <- Android native engine skeleton (C++20)
│   ├── include/          PPC context, HLE kernel, GPU layer, engine API
│   └── src/              implementations + JNI bridge + host selftest
├── android/            <- Kotlin launcher (no game data in the APK)
├── recomp/             <- generated C++ output (empty until codegen runs)
├── patches/            <- intended runtime patches
├── _game/              <- YOUR game data (ISO + extracted XEX), gitignored
├── _reference/         <- BIOS dump, analysis-only, NEVER shipped
├── scripts/            <- reproducible environment setup
└── build/              <- CMake/Gradle build output
```

---

## Quick start (verified on this machine)

```powershell
# 0. Toolchain was installed once by scripts/install-toolchain.ps1:
#    F:\tools\android-sdk\{platforms,build-tools,ndk,cmake,platform-tools}

# 1. Re-extract/verify the real XEX from your ISO and regenerate configs
python tools/pipeline.py inspect
python tools/pipeline.py configure

# 2. Build + run the native runtime self-test (host)
python tools/pipeline.py build-native

# 3. Build the Android launcher APK
python tools/pipeline.py build-apk

# 4. Install on an arm64 device with GLES3
adb install -r android\app\build\outputs\apk\debug\app-debug.apk
```

The launcher will ask you to pick your own ISO, copy it into app-private
storage, extract `default.xex`, and start the engine. No game bytes are inside
the APK.

---

## Reading order

1. `01-ARCHITECTURE.md` — how the pieces fit together
2. `02-TOOLCHAIN.md` — what every vendored tool does
3. `03-XEX-ANALYSIS.md` — the real facts about your BioShock dump
4. `04-PIPELINE.md` and `05-BUILDING.md` — end-to-end workflow and commands
5. `06-HLE-KERNEL.md`, `07-GPU-XENOS-GLES3.md`, `08-LAUNCHER.md` — runtime design
6. `09-RE-FINDINGS.md` — the reverse-engineering work that remains
7. `10-STATUS.md`, `11-ROADMAP.md` — where things stand and what is next
8. `12-...` through `18-...` — supporting reference material
9. `19-WORKLOG.md` — session-by-session progress journal
10. `20-REXGLUE-V011.md` — prebuilt toolchain / generated project model
11. `21-HLE-IMPORT-MATRIX.md` — the 216 real imports measured from generated code
12. `22-PHASE2-LINK.md` / `23-VERIFICATION.md` — link milestone and full verification report
13. `24-DEVICE-M0.md` — on-device launcher/GLES3 acceptance test (PASSED)
14. `25-PHASE3-RENDERER.md` — Phase 3 PM4 parser and renderer plan
15. `26-DOCS-WEBSITE.md` — SQLite + local SPA docs visualization (run `scripts/serve-docs.ps1`)
16. `27-ABI-HELPERS.md` — real XDK register save/restore helpers (+9 remaining placeholders)
17. `28-HLE-IMPORTS.md` — all 216 kernel/XAM imports registered, 32 real handlers
18. `29-FIRST-BOOT.md` — host first-boot harness and first fault analysis
19. `31-GOAL-TITLE-SCREEN.md` — active goal: structured plan and progress toward the title screen
