# Toolchain inventory

All 12 archives supplied with the task were unpacked by `tools/vendor_extract.py`
into `third_party/`. The table below records what each one is, who made it,
and the role it plays. Licences are preserved in each vendored directory.

| Directory | Source archive | Role in this project | Key facts |
|---|---|---|---|
| `rexglue-sdk` | `rexglue-sdk-main.zip` | **Primary recompiler + runtime** | v0.8.x; `rexglue init` + `rexglue codegen`; self-contained XEX/STFS/LZX/AES extraction, jump-table/RTTI/vtable/ABI detection, manifest model, Xboxkrnl/XAM kernel, D3D12 **and** Vulkan GPU backends, XMA via FFmpeg, SDL3 input. Requires Clang 20+, CMake 3.25+, Ninja. |
| `360tools` | `360tools-main.zip` | Triage scripts + playbook | Python helpers (`xex_info.py`, `extract_iso.py`, `extract_pe.py`, `find_abi_addrs.py`, `extract_switch_tables.py`, `find_missing_vtable_funcs.py`, `lzx_decompress.py`); `recomp_harness.py` for batch triage; `templates/` stubs overlay; docs on runtime debugging. |
| `rexglue-knowledges` | `rexglue-knowledges-main.zip` | **Background knowledge base** | Static recompilation overview, Xbox 360 hardware, XEX format, containers/extraction, toolchains, recompilation pipeline, plus deep-dive notes. Excellent source material for further docs. |
| `XenonRecomp` | `XenonRecomp-main.zip` | Alternative static recompiler | PPC→C++, inspired by N64Recomp. Needs a runtime (explicitly disclaims one). Uses TOML config, has `XenonAnalyse` for jump tables/function boundaries, mid-asm hooks, ARM64 VMX via SIMDE. Requires Clang 18+, CMake 3.20+, submodules. |
| `XenosRecomp` | `XenosRecomp-main.zip` | GPU shader translator | Xenos shader microcode → HLSL → DXIL/SPIR-V via DXC. Written around UnleashedRecomp; shader container/constants/vertex-fetch support is game-specific. Starting point for a GLS3 backend, not a drop-in. |
| `xenon-jumptables` | `xenon-jumptables-main.zip` | Jump-table recovery | Patterns, boundaries, integration notes and example config for `mtctr`/`bctr` jump tables. |
| `Xbox360RecompAndroid` | `Xbox360RecompAndroid-main.zip` | Launcher UI reference | Flutter/Dart launcher + C++20 NDK engine. **Dynamic JIT + HLE + Xenos→Vulkan**, not static recomp, and roadmap Phase 2-5 incomplete. We borrow its UI layout ideas, not its code. |
| `x360port` | `x360port-main.zip` | Xenia-backed runtime reference | Xenia dynarec embedding boundary; AGENTS.md/CLAUDE.md describe a real Xenia executor with typed imports and overrides. Explicitly **no Android runtime owner** — useful as HLE/import-design reading. |
| `rexdex-recompiler` | `recompiler-master.zip` | Historical reference | The original (2015) Xbox 360→C++ recompiler by rexdex. Windows/VS2015/wxWidgets only. Its README explains block reconstruction and HLE import thunks clearly. Educational. |
| `Xenon-Recomp-legacy` | `Xenon-Recomp-main.zip` | Historical reference | Older Xenon recompilation material. |
| `Xerenge` | `Xerenge-main.zip` | Xbox 360 tooling reference | General Xbox 360 reverse-engineering helpers; useful for binary triage. |
| `gears-of-war-2-pc-port` | `gears-of-war-2-pc-port-master.zip` | UE3 porting reference | Open-source PC port of another Xbox 360 title; relevant as an example of a full engine-level port, including build/systemd concepts. |

Additional inputs at the project root (not vendored into `third_party/`):

| Input | Purpose |
|---|---|
| `BioShock (World) (En,Fr,De,Es,It).7z` | Your redump-style game dump; contains the 7.8 GB XGD2 ISO |
| `XBOX360_BUILD_VERSIONS.md` | 293 Xbox 360 symbol-set versions — used to match the game's XDK build to development symbols |
| `xbox360-bios-v10.bin` | 256 KB console bootrom dump; **analysis/reference only**, copied to `_reference/`, never shipped |

## Why ReXGlue is the primary path

* It is the only vendored tool that provides *both* the recompiler **and** a
  substantial runtime (kernel + GPU + audio + input). XenonRecomp explicitly
  provides no runtime, so choosing it would mean writing all of the above.
* Its `codegen` handles XEX decryption/decompression internally, which the
  older standalone `extract_pe.py` cannot reliably do.
* It has a manifest/project model and a batch harness for resolving codegen
  failures — exactly the iteration loop needed for a title this large.

## Why XenonRecomp and XenosRecomp are still vendored

* XenonRecomp's TOML schema is clearer for learning the required analysis
  inputs, and its documentation of ARM64 VMX/SIMDE support is directly relevant
  to an Android port. `config/bioshock1.toml` is written in a compatible shape.
* XenosRecomp is the only vendored shader translator. Even though it targets
  HLSL/SPIR-V, its handling of constants, vertex fetch, texture sampling and
  specialization constants is the design reference for a GLES3 backend.

## Prebuilt host recompiler (Phase 1)

Building ReXGlue from the vendored source snapshot requires 22 git submodules
(FFmpeg, glslang, SPIRV-Tools, SDL3, Vulkan, …) plus Clang 20+ / CMake 3.25+.
Upstream publishes prebuilt SDK archives, so the fast path is:

```
scripts/install-rexglue.ps1
  -> tools-bin/rexglue-sdk-0.11.0/win-amd64/bin/rexglue.exe
```

This v0.11.0 CLI provides `init`, `codegen`, `recompile-tests`, `vfs dump`
(STFS/SVOD/GDFX extraction) and `list shim`. Its manifest format is simpler
than the v0.8-era documentation in the vendored snapshot; use `rexglue <sub>
--help` as the authority and record differences in `19-WORKLOG.md`.
`tools-bin/` and `_work/` are gitignored — the downloader and pipeline scripts
recreate them.

## Toolchain installed on this machine

`scripts/install-toolchain.ps1` downloaded and unpacked, with no admin rights:

```
F:\tools\android-sdk\
  cmdline-tools/latest\
  platforms\android-34\
  build-tools\34.0.0\
  platform-tools\
  ndk\26.3.11579264\
  cmake\3.22.1\bin\cmake.exe   (also used as host CMake)
```

Host tools already present: Python 3.14, Git, Java 21, Clang/Clang++ 23
(llvm-mingw), Ninja, Make. `py7zr` is installed for reading the game's `.7z`.

## Toolchain still needed for full codegen (not installed here)

ReXGlue SDK's own build requires **Clang 20+**, CMake 3.25+ and a full set of
vendored dependencies fetched by its recursive git submodules. This project
vendors the *source archive*, not its submodule contents. To build the
recompiler itself you will need to clone ReXGlue recursively and follow its
build instructions; see `05-BUILDING.md` for the exact hand-off.
