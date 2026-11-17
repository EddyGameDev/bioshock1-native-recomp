# Legal and provenance

## 1. No game data is distributed

This repository contains **no** ISO, XEX, BIOS, FMOD bank, Unreal package,
font, texture, audio or recompiled game code. The verified APK contains only
our own engine library (`libbioshock1-native.so`), the Kotlin runtime and the
C++ shared runtime (`libc++_shared.so`). The only `.bin` file is Kotlin's
`DebugProbesKt.bin`.

The game dump you supplied (`BioShock (World)...7z`) and the extracted ISO /
`default.xex` live in `_game/`, which is gitignored. The BIOS dump lives in
`_reference/`, also gitignored, and is never referenced by Gradle or CMake.

You must own a legal copy of any game you port. The tools here are for
interoperability, preservation and reverse-engineering research.

## 2. Third-party components (vendored under `third_party/`)

| Component | Typical licence | Notes |
|---|---|---|
| ReXGlue SDK | see its `LICENSE` in `third_party/rexglue-sdk/` | GPL/other per file; derived from Xenia |
| 360tools | MIT unless noted | its `extract_stfs.py` includes BSD-derived code |
| rexglue-knowledges | see repo | documentation |
| XenonRecomp | see `LICENSE.md` | code based on Xenia and Xenia Canary |
| XenosRecomp | see `LICENSE.md` | code based on Xenia |
| xenon-jumptables | see `LICENSE` | |
| Xbox360RecompAndroid | GPL-2.0 | used only as a UI/architecture reference |
| x360port | see `LICENSE` | Xenia-backed runtime reference |
| rexdex/recompiler | see repo | historical reference |
| Xerenge | see repo | tooling reference |
| gears-of-war-2-pc-port | see its `LICENSE` | UE3 port reference |

Android SDK, NDK, build-tools and Gradle are distributed by Google under their
own licences; `scripts/install-toolchain.ps1` accepts those licences on your
behalf and downloads the packages directly from `dl.google.com`.

Microsoft Xbox 360 SDK headers, symbols, XDK libraries, BIOS images and game
assets are **not** included and are **not** redistributed. `XBOX360_BUILD_VERSIONS.md`
contains only facts (set names, version numbers, counts) derived from file
contents — no symbol files, name lists or code.

## 3. Clean-room rule

All HLE functions written for this project must be clean-room reimplementations
based on:

* public documentation (Free60, Xenia, ReXGlue, XenonRecomp and related open source),
* observed behaviour of the game's imports,
* facts recorded in the project's own analysis files.

Do not copy proprietary Microsoft source or game code into `native/`, `tools/`
or `docs/`. Keep derived facts (addresses, byte patterns, function names) in
`09-RE-FINDINGS.md` with evidence.

## 4. Trademarks

BioShock is a trademark of its respective owners. Xbox, Xbox 360, Xenon, Xenos
and XDK are trademarks of Microsoft Corporation. Android is a trademark of
Google LLC. This project is unaffiliated with and not endorsed by any of them.

## 5. Attribution for derived design

The architecture follows published work:

* **Xenia** — XEX format, HLE kernel, PM4 and shader translation research.
* **N64Recomp / XenonRecomp / ReXGlue** — modern static recompilation pipeline.
* **UnleashedRecomp + XenosRecomp** — API-level renderer translation concepts.
* **PS2Recomp, gbarecomp, psxrecomp** (the reference ports in the parent folder)
  — launcher-not-embedded and host-codegen patterns.

Per-file licences in `third_party/` always take precedence over this summary.
