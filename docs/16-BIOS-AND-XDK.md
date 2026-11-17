# BIOS dump and XDK build matching

You supplied `xbox360-bios-v10.bin` (262,144 bytes) and a 293-row XDK symbol-set
table (`XBOX360_BUILD_VERSIONS.md`). This document explains how they fit into a
static recompilation project — and where they do *not*.

## 1. What the BIOS is (and is not) for

A 256 KB Xbox 360 bootrom dump is the console's first-stage bootloader. In an
**emulator** (Xenia), it participates in the boot chain and can help identify
the console revision and key material. In a **static recompilation**:

* The game's XEX is decrypted/decompressed by the recompiler using publicly
  documented algorithms and keys, not by booting the console.
* The kernel is replaced with HLE (`06-HLE-KERNEL.md`).
* Therefore **the BIOS is not used at runtime** and is never packaged.

We copied it to `_reference/xbox360-bios-v10.bin` as analysis material only.
Useful things you can do with it:

1. Confirm bootrom version ("v10" in the filename) and console generation.
2. Compare with Xenia's expectations if you experiment with a hybrid
   Xenia-backed runtime (`third_party/x360port`).
3. Identify XDK-era constants if you inspect strings — but never commit
   proprietary data derived from it.

## 2. Reading the XDK build from the game

The authoritative method is in `XBOX360_BUILD_VERSIONS.md` §"Reading your own
game's XDK build". The XEX2 header `0x000200FF` (`STATIC_LIBRARIES`) contains
16-byte records. Our parser found 9:

```
D3D9       2.0.5426.2      XAPILIB    2.0.5426.2
XONLINE    2.0.5426.0      LIBCPMT    2.0.5426.0
XBOXKRNL   2.0.5426.0      D3DX9      2.0.5426.0
XAUD       2.0.5426.0      XMP        2.0.5426.0
XGRAPHC    2.0.5426.0
```

**XDK build = 5426.** All records agree, so this is an exact `xex`-derived
value, not an estimate.

## 3. Using the symbol-set table

In `XBOX360_BUILD_VERSIONS.md`:

1. Find build **5426** — the table lists the 2007-03-07/09 Halo 3 MAP sets
   (title `4D5307E6`) at that build. The 2007-03-09 MAP set is especially
   useful: `Fn names yes 31,247`, `Code yes xex`, `Lib/obj lib+obj`, debug
   info `no`.
2. Neighbouring builds on either side: **4314** (Call of Duty: World at War,
   Dead Space Vertical Slice) and **5632** (Halo 3-era Skate, Too Human).
   Builds on either side link largely the same XDK code, so those sets are
   fallback naming sources.
3. Look at the **Has** column: prefer `pdb` or at least `map`, with
   `XeReleaseLTCG`/`Opt` configurations. Where content columns are filled,
   check `Fn names`, `Sizes`, `Code`, `Lib/obj`.
4. Middleware: BioShock's imports show D3D9/XGRAPHC/XAUD/XMP from the XDK, plus
   its own Unreal/FMOD code — the table's middleware index can help identify
   bundled libraries in other titles, but the game's own engine is not in it.

## 4. What to extract from symbol sets

Only **derived facts** belong in this repository:

* function names for XDK ordinal stubs (e.g. mapping ordinal → `NtCreateFile`);
* byte patterns for `__restgprlr*`, `__savegprlr*`, VMX helpers, CRT helpers;
* sizes and library/object attribution useful for matching functions across
  builds by bytes.

Never commit the PDB/MAP/XEX files themselves, nor name lists copied verbatim
from proprietary symbol sets. Record in `09-RE-FINDINGS.md` how each fact was
derived.

## 5. title updates / patches

The XEX has a `DELTA_PATCH_DESCRIPTOR` header (`0x00020104` at `0x1624`), which
means title updates may apply. XenonRecomp's TOML supports `patch_file_path`
and `patched_file_path` for an `.xexp`; if you use a TU, feed the patched XEX to
the recompiler and note the version in `03-XEX-ANALYSIS.md`. Public databases
list BioShock TUs for media IDs `44E704F7` / `6B31B637` — only use updates you
legally possess.

## 6. Legal reminder

The BIOS and any development symbol sets are proprietary Microsoft material.
Keep them in `_reference/` (gitignored), use them only for local analysis, and
never ship them in the APK or commit them. `13-LEGAL.md` has the full policy.
