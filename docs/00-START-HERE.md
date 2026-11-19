# BioShock 1 — Native Android & Windows Static Recompilation

**START HERE.** This is the master index and honest status statement for the
project. Read this before touching any tool.

- **Community & Feedback Discord:** **https://discord.gg/aaDRJQNw8T**
- **Required Game Version:** **Xbox 360 World Release** (**Title ID: `545407D8`**, **Media ID: `44E704F7`**)
- **Current Release:** **`v0.0.5-experimental`** (Android APK + Windows standalone zip)

---

## What this project is

A host-side **static recompilation** pipeline plus native **Windows** and **Android**
launchers that turn an Xbox 360 game (**BioShock 1, World Release**) into C++ once
on a PC, then build that C++ into native `x86_64` (Windows) and `arm64-v8a` (Android)
executables executed directly on the target device.

There is **no emulator loop, no JIT, and no interpreter** in the final design.
The PowerPC code is translated ahead of time; only the runtime (ReXGlue kernel HLE,
memory, XAM, audio, input, and our custom Xenos D3D11 / OpenGL ES 3.2 renderers)
executes on the device. This is the same class of project as the reference ports
in the parent folder:

| Reference | Console | Recompiler | Launcher |
|---|---|---|---|
| Sonic Advance 2 | GBA | gbarecomp | Kotlin/Flutter-style launcher |
| DBZ: Legacy of Goku | GBA | gbarecomp | Kotlin launcher |
| Contra: Legacy of War | PS1 | psxrecomp | recomp-ui |
| Metal Gear Solid 3 | PS2 | PS2Recomp | Kotlin launcher (not embedded) |
| **BioShock 1 (this)** | **Xbox 360** | **ReXGlue / XenonRecomp** | **Kotlin Android launcher + Windows launcher (not embedded)** |

---

## Honest status (`v0.0.5-experimental`)

Both **Windows** and **Android** builds boot through the real ReXGlue runtime,
render the title screen, intro cinematic, Bathysphere descent, Andrew Ryan
elevator speech, and in-game Rapture levels with audio, full controls, live
resolution scaling, and working in-game Save / Load:

1. **Full ahead-of-time PPC→C++ codegen (152 TUs / 36,625 functions).** Generated
   from the real `default.xex` (`0x545407D8`, Media ID `44E704F7`, XDK 5426) and
   linked against the real ReXGlue v0.11.0 runtime on both Windows and Android.
2. **Custom Xenos Renderers (`renderer/`, `rexgpu-xenos-custom`):**
   - **Windows (D3D11):** Live no-restart internal resolution scaling (`360p`–`2160p`
     via **F1**), live Lanczos2 / Bilinear present filter (**F2**), async FIFO FXC
     shader compilation + disk cache, and strict hazard/binding tracking.
   - **Android (OpenGL ES 3.2):** Xenos→GLSL ES 3.20 shader translator, live
     no-restart internal resolution scaling (`360p`–`1440p` via the on-screen **RES**
     pill, defaulting to `540p` / `2x` anisotropy on fresh installs), tiered texture
     change-detection, fast row-copy Bink video uploads, `RRRR` resolved depth
     replication for underwater fog/lighting, and display-priority GPU worker thread.
3. **Audio, Controls & Save / Load:**
   - **Android Audio:** Native 48 kHz stereo `libaaudio.so` driver in `sdl3_audio_driver.cpp`.
   - **Full Xbox 360 Touch Controller:** Dual floating sticks, `LT`/`LB`/`RT`/`RB`,
     `D-Pad`, `A`/`B`/`X`/`Y`, `L3`/`R3`, `BACK`, `START`, `RES`, and `HUD` toggle.
   - **In-Game Save / Load:** Fixed `ObDosDevices` (`0xFFFFFFFD`) root directory
     handles, `\??\` prefix stripping, Android SELinux `MoveNoReplace` (`Save.tmp`
     → `DLContentInfo.pkg`), and numeric profile settings persistence.

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
