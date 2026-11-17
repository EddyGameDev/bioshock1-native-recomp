# Reverse-engineering findings & work plan

This is the living notebook for game-specific analysis. Values marked **TODO**
in `config/bioshock1.toml` are filled here once measured. Keep every entry
evidence-based: record the byte pattern or address and how you found it.

## 0. What is already known (measured, not guessed)

| Item | Value | Source |
|---|---|---|
| Title | BioShock (World), 2007 | ISO + XEX headers |
| Title ID | `0x545407D8` (TT-2008) | XEX exec info, ASCII fields |
| XDK build | 5426 | 9 static-library records |
| Image base | `0x82000000` | `0x00010201` header |
| Image size | `0x00CD0000` | security header |
| PE offset | `0x3000` | XEX header (PE compressed) |
| Imports | 427 records (325 krnl + 102 XAM) | `0x000103FF` header |
| Engine | Unreal-derived "Vengeance" | `.u` package names (`ShockGame.u`, `VengeanceShared.u`) |
| Audio | FMOD `.fsb` banks | ISO file listing |
| Compiler era | MSVC 2005 (14.0.x) | XDK 5426 era; confirm from any available PDB |

## 0b. Generated facts from ReXGlue v0.11 (measured, 2026-11-17)

`rexglue codegen` analysed the real XEX in 61 s with **zero warnings** and
produced 308 files. These constants come straight from the generated
`bioshock1_pch.h` and are now authoritative for our runtime:

```
REX_IMAGE_BASE            0x82000000ull
REX_IMAGE_SIZE            0x00CD0000ull
REX_CODE_BASE             0x82160000ull
REX_CODE_SIZE             0x008F195Cull
REX_THUNK_RESERVE_SIZE    0x00010000ull
```

Function mappings start at `sub_82160000` (`kPPCFuncMappings[]` in
`bioshock1_init.cpp`). ReXGlue resolved function boundaries, gap-filling,
merging and validation automatically for this title — no manual hints were
needed for codegen. Jump tables and exception handling are handled by the
codegen pipeline (codegen flags default to all `false`, i.e. no unsafe
optimisations).

ReXGlue's own header dump confirms Title `545407D8`, Media `44E704F7`,
Version `0.0.0.1`, Filetime `2007-06-22 23:21:30 UTC`.

The remaining RE work moves from *codegen input* to *runtime integration*:

1. match `native/include/ppc_context.h` to `rex/ppc/context.h` (ReXGlue uses
   named fields `ctx.r3.u64`, `ctx.cr0.Compare<...>`, `ctx.lr`, etc.);
2. bind generated import shims (`bioshock1_register.cpp`) to our HLE kernel;
3. implement the Xenos PM4/GLES3 and audio paths.

## 0c. Guest entry point (measured, 2026-11-17)

`tools/extract_pe.py` successfully decrypted and LZX-decompressed the XEX into
`_work/default_unpacked` (13,434,880 bytes). The PE optional header gives:

```
AddressOfEntryPoint (RVA) = 0x19D698
ImageBase                 = 0x82000000
Guest entry VA            = 0x8219D698
```

ReXGlue's generated `bioshock1_init.cpp` maps this address to **`xstart`**, a
runtime function (not a generated `sub_...`). Disassembling the real entry with
Capstone confirms it is the XDK CRT startup routine (`mflr r12; bl ...; stwu
r1,...; ...`), so ReXGlue replaces it with a clean-room runtime implementation.

**Resolution (same session):** although ReXGlue gives the entry function the
name `xstart`, it **does** emit the real recompiled CRT body:
`bioshock1_recomp.84.cpp` contains `DEFINE_REX_FUNC(xstart)`, and the arm64
object defines strong `__imp__xstart` / weak `xstart`. Therefore no runtime
`xstart` implementation is needed — the only action required was to remove
`xstart` from `config/runtime_symbols.json` so our no-op stub does not override
the generated one. `tools/extract_runtime_symbols.py` now excludes it
automatically. This removes the perceived boot blocker.

## 1. Function boundary analysis

XenonRecomp's `XenonAnalyse` finds most boundaries from `.pdata` (functions with
stack frames) plus scans for branch-and-link (`bl`) targets. BioShock is an
MSVC 2005 build and should follow the standard ABI, so expect good results.

Known failure modes and their fixes:

| Problem | Symptom | Fix |
|---|---|---|
| Jump-table functions look like tail calls | functions merge or end early | add explicit `functions = [{address, size}]` entries |
| Exception data between functions | "invalid instruction" warnings | add `invalid_instructions` skip entries |
| Indirect calls through vtables | unresolved call errors | ReXGlue auto-resolves via RTTI/vtable detection; otherwise add function hints |
| Setjmp/longjmp | crashes on error paths | locate `RtlUnwind` callers; `setjmp` follows; add addresses to TOML |

Record here every manually added boundary with the reason.

## 2. Jump tables

The classic Xbox 360 switch idiom is:

```
... compute table index ...
mtctr rX
bctr
```

`third_party/xenon-jumptables/` documents boundaries, idioms and integration,
and `XenonAnalyse` emits a TOML of detected tables. Because BioShock predates
Sonic Unleashed, the surrounding instruction patterns will differ — budget time
to extend the detector. Every confirmed table goes into
`config/bioshock1_switch_tables.toml` and is referenced by the main TOML.

## 3. ABI helper functions (required by XenonRecomp)

These are the save/restore stubs the Microsoft compiler emits for non-volatile
registers. Find them with the byte patterns from XenonRecomp's README:

| Helper | Starting byte pattern | Address |
|---|---|---|
| `__restgprlr_14` | `e9 c1 ff 68` | TODO |
| `__savegprlr_14` | `f9 c1 ff 68` | TODO |
| `__restfpr_14` | `c9 cc ff 70` | TODO |
| `__savefpr_14` | `d9 cc ff 70` | TODO |
| `__restvmx_14` | `39 60 fe e0 7d cb 60 ce` | TODO |
| `__savevmx_14` | `39 60 fe e0 7d cb 61 ce` | TODO |
| `__restvmx_64` | `39 60 fc 00 10 0b 60 cb` | TODO |
| `__savevmx_64` | `39 60 fc 00 10 0b 61 cb` | TODO |
| `setjmp` | just after `RtlUnwind` callers | TODO |
| `longjmp` | callers of `RtlUnwind` | TODO |

Procedure: decompress the XEX with ReXGlue (or `extract_pe.py` as a fallback),
then search the decompressed `.text` for the patterns above; verify by
disassembling the following instructions (they should repeat the same load or
store with increasing register numbers and decreasing stack offsets).

## 4. Symbol sets and naming

`XBOX360_BUILD_VERSIONS.md` is the lookup table for development symbol sets.
For XDK 5426 it points at the 2007-03-09 Halo 3 MAP set (names, sizes, code,
lib/obj attribution) and nearby 4314/5632 sets. Use those to:

* name XDK import stubs in the disassembly;
* identify CRT helpers and `__restgprlr*` variants by byte matching (the sets
  with `Code: yes` let you match functions by bytes across builds);
* understand the D3D9 / XGRAPHC call patterns the game uses.

Never copy symbol files or game code into this repository — only derived facts
(addresses, names, patterns) belong here.

## 5. Mid-asm hooks to plan

XenonRecomp's `[[midasm_hook]]` entries let you call native code at an exact
instruction without replacing it. Plan hooks for:

| Hook | Purpose | Address |
|---|---|---|
| `HookXenosPresent` | intercept `VdSwap`/buffer flip → `gpu::present()` | TODO |
| `HookXenosDraw` | capture draw parameters for the first M2 milestone | TODO |
| `HookXInputState` | inject touch/gamepad state into guest `XINPUT_STATE` | TODO |
| `HookFileOpen` | redirect guest paths to app-private storage | TODO |
| `HookXmaDecode` | bridge to the Android audio decoder | TODO |

Use `return_on_true`/`jump_address_*` only when you understand the surrounding
control flow; a wrong hook can silently skip gameplay code.

## 6. Per-title quirks to expect for a 2007 UE2.5 game

* **Large single executable.** ~12.8 MB image; codegen output will be big.
  Expect long compile times; enable XenonRecomp's local-register optimisations
  only after a working baseline.
* **Static-linked XDK.** Nine static libraries (D3D9, XAPILIB, LIBCPMT, ...)
  mean much of the kernel is *inlined into the game code*, not imported.
  HLE cannot intercept those calls; you may need to patch the specific inlined
  sequences (this is the "inlining kills function-level wrappers" problem from
  the rexdex README). The `patches/` folder exists for exactly this.
* **Unreal scripting VM.** `Scripting.u`/`ShockGame.u` contain bytecode; the
  engine's VM is native code in the XEX. No need to emulate UC separately, but
  package loading must work through the guest filesystem.
* **No exceptions support in XenonRecomp.** If the game relies on SEH for
  normal control flow, ReXGlue's more mature exception handling is preferable.
* **XMA audio.** XMA2 decode is a known hard part (MMIO/unfinished in
  XenonRecomp); ReXGlue uses FFmpeg. Budget a dedicated audio milestone.

## 7. Definition of done for this notebook

Every TODO in `config/bioshock1.toml` has a corresponding entry here with:

```
Address:      0x........
Found by:     byte pattern / .pdata / symbol match / trace
Verified by:  disassembly snippet or runtime log
Applied in:   config/bioshock1.toml / patches/<name>.patch
```

Only then is it honest to run `pipeline.py codegen` on a "finished" config.
