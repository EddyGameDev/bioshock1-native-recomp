# References and further reading

## Local references (already on disk)

| Path | What to read |
|---|---|
| `../XBOX360_BUILD_VERSIONS.md` | 293 Xbox 360 symbol sets by XDK build; how to read XEX headers yourself |
| `third_party/rexglue-knowledges/general/00-static-recompilation-overview.md` | The overall method |
| `third_party/rexglue-knowledges/general/10-xbox360-hardware.md` | Xenon/Xenos hardware |
| `third_party/rexglue-knowledges/general/20-xex-format.md` | XEX container details |
| `third_party/rexglue-knowledges/general/25-containers-and-extraction.md` | STFS/ISO/LZX handling |
| `third_party/rexglue-knowledges/general/40-recompilation-pipeline.md` | Full pipeline description |
| `third_party/360tools/README.md` | v0.8.0 ReXGlue workflow, stub priority, harness |
| `third_party/360tools/docs/rexglue-workflow.md` | Practical ReXGlue usage |
| `third_party/360tools/docs/runtime-debugging.md` | "Boots but crashes" playbook |
| `third_party/360tools/docs/xbla-triage-findings.md` | Cross-title findings |
| `third_party/360tools/templates/STUBS.md` | How to write missing kernel stubs |
| `third_party/XenonRecomp/README.md` | TOML schema, ABI helpers, optimisations, hooks |
| `third_party/XenosRecomp/README.md` | Shader container, constants, vertex fetch, textures |
| `third_party/rexdex-recompiler/readme.md` | Classic explanation of blocks, imports, EDRAM |
| `third_party/xenon-jumptables/docs/` | Jump-table boundaries and idioms |
| `third_party/x360port/AGENTS.md`, `CLAUDE.md` | Import/override design around Xenia |
| `third_party/Xbox360RecompAndroid/README.md` | Flutter launcher + Xenos→Vulkan architecture (reference only) |
| `../Metal Gear Solid 3 .../mgs3-recompiled-android/README.md` | Launcher-not-embed pattern, host pipeline, patches |
| `../Sonic Advance 2 .../SonicAdvance2Recomp/README.md` | GBA static recomp structure |
| `../DbzLegacyOfGokuRecomp/` | Small GBA launcher example |
| `../Contra Legacy of War .../Refrences/` | PS1 recomp references |

## Upstream projects

* ReXGlue SDK — `github.com/rexglue/rexglue-sdk` (recompiler + runtime)
* 360tools — ReXGlue workflow and triage harness
* XenonRecomp — `github.com/hedge-dev/XenonRecomp` (PPC→C++)
* XenosRecomp — `github.com/hedge-dev/XenosRecomp` (shader→HLSL)
* Xenia / Xenia Canary — `github.com/xenia-project/xenia`, `github.com/xenia-canary/xenia-canary`
* N64Recomp — the design XenonRecomp was inspired by
* UnleashedRecomp — API-level renderer translation reference
* Free60 — XEX format documentation
* extract-xiso — XDVDFS/decryption tooling
* XBOX360 symbol-set inventory tools (`pdb_inventory.py`, `pdb_content.py`, `pdb_public.py`)

## Public facts used in this project

* BioShock Xbox 360 title ID **`0x545407D8`** (Take-Two "TT-2008") and World
  media ID `44E704F7` — confirmed against public Xbox 360 title databases.
* XDK **5426** build family — the `XBOX360_BUILD_VERSIONS.md` table places
  symbol sets with names/sizes near this build (Halo 3 sets at 5426, 4314/5632
  on either side).
* Power ISA 2.07 — the authoritative PPC instruction reference used by all
  the vendored recompilers.
* AMD R600/R7xx ISA docs — basis for Xenos shader translation.

## How to keep this list current

When you solve a new problem (a jump-table idiom, an EDRAM quirk, an XMA
 glue detail), add the reference you used here *and* the derived fact in
`09-RE-FINDINGS.md`. Separate "where I learned it" from "what I measured".
