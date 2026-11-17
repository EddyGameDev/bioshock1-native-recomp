# XEX / disc analysis — real data from your dump

All values in this document were measured from the files you supplied, using
the project's own tools (`tools/xdvdfs.py`, `tools/xex_inspect.py`). The machine-
readable version is `config/xex_analysis.json`.

## 1. The container

| Property | Value |
|---|---|
| Source archive | `BioShock (World) (En,Fr,De,Es,It).7z` (6.45 GB compressed) |
| Inner image | `BioShock (World) (En,Fr,De,Es,It).iso` (7,835,492,352 bytes) |
| Disc format | Xbox 360 XGD2 |
| XDVDFS game partition | `0x0FD90000` (magic at `+0x10000`) |
| XDVDFS root | sector `1,757,104`, `default.xex` at sector `1,757,120` |
| File system | XDVDFS B-tree; files include `.u` Unreal packages and `.fsb` FMOD banks |
| Notes file in archive | `Vimm's Lair.txt` (provenance of the dump; not used by the pipeline) |

The ISO was decompressed once into `_game/` and the 13,021,184-byte
`default.xex` was streamed out of it. The launcher has an equivalent Kotlin
reader (`android/.../Xdvdfs.kt`) so it can do the same on the device.

Observed game content (sample from the root directory):

```
IGVisualEffectsSubsystem.u      29,921 bytes
Scripting.u                     92,596 bytes
ShockGame.u                 13,786,151 bytes
Tyrion.u                        13,414 bytes
VengeanceShared.u              118,643 bytes
Sounds_Xenon/streams_*_audio.fsb / *_music_audio.fsb   (many, 10-40 MB each)
```

The `.u` names confirm BioShock's "Vengeance" / Unreal Engine 2.5-derived
engine. `ShockGame.u` is the main game package; `VengeanceShared.u` holds shared
scripting. Audio is FMOD `.fsb`, so an XMA/FMOD path will be needed in the HLE
audio layer (`docs/07-GPU-XENOS-GLES3.md` discusses the companion graphics work).

## 2. XEX2 header facts

| Field | Value | Notes |
|---|---|---|
| Magic | `XEX2` | Standard XEX2 container |
| Module flags | `0x00000001` | |
| PE data offset | `0x00003000` | PE is encrypted/compressed; needs ReXGlue to unpack |
| Security offset | `0x00000088` | RSA signature + SHA-1 present |
| Security header size | `0x14BC` | |
| Image size | `0x00CD0000` (13,435,904 bytes) | Decompressed guest image size |
| Optional headers | 14 | Listed below |
| Image base address | `0x82000000` | Where the recompiled code must be mapped |
| Original image name | `default.exe` | From the `0x000183FF` header blob |
| Entry point | *not in optional headers* | Read from PE header after decryption/decompression |
| Title ID | `0x545407D8` | **TT-2008**, Take-Two / BioShock ✓ |
| Version field | `5454` (raw `0x35343534`) | ASCII digits, as Xbox 360 stores them |
| Base version field | `07D8` (raw `0x30374438`) | ASCII digits; with `5454` forms the title ID |
| Media ID field | `0x00000014` | Container-level media ID |
| Platform / exe type / media | `0 / 9 / 93` | |
| XDK build | **5426** | From static-library version records |
| Title ID (ReXGlue-confirmed) | `545407D8` | `rexglue codegen` header dump ✓ |
| Media ID (ReXGlue-confirmed) | `44E704F7` | `rexglue codegen` header dump ✓ |
| Version (ReXGlue) | `0.0.0.1` | ReXGlue header dump |
| Filetime (ReXGlue) | `2007-06-22 23:21:30 UTC` | ReXGlue header dump |

### Optional header map (from the file, big-endian IDs)

```
0x000002FF EXECUTION_INFO      -> 0x1544
0x000003FF SYSTEM_FLAGS        -> 0x1558
0x00010100 BOUNDING_PATH       -> 0x8219D698 (guest address)
0x00010201 IMAGE_BASE_ADDRESS  -> 0x82000000
0x000103FF IMPORT_LIBRARIES    -> 0x28E0
0x00018002 LAN_KEY             -> 0x1578
0x000183FF ORIGINAL_BASE       -> 0x1580 ("default.exe")
0x000200FF STATIC_LIBRARIES    -> 0x1590
0x00020104 DELTA_PATCH         -> 0x1624
0x00020200 DEFAULT_HEAP_SIZE   -> 0x1F0000 (1984 KB)
0x00040006 TLS_INFO            -> 0x1634
0x00040310 DEFAULT_STACK_SIZE  -> 0x164C
0x00040404 DEFAULT_FS_CACHE    -> 0x168C
0x000405FF BASE_REFERENCE      -> 0x169C
```

> **Parser note.** The vendored `360tools/xex_info.py` labels several of these
> IDs incorrectly (it swaps `STATIC_LIBRARIES` and `BASE_REFERENCE`, and treats
> pointer-valued headers as direct values). Our `tools/xex_inspect.py` uses the
> Xenia-compatible IDs above and is the authoritative tool in this project.

### Title-ID decoding — a trap we fixed

The two "version" fields contain ASCII hex digits, not numeric version
numbers. `0x35343534` is the string `"5454"` and `0x30374438` is `"07D8"`.
Together they spell the real title ID **`0x545407D8`**, which matches the
public Xbox 360 title database (BioShock, Take-Two, "TT-2008"). The raw field at
`+0x0C` of the execution-info structure is `0x82C36D80` and is *not* the title
ID; our first parser reported it incorrectly and has been corrected.

## 3. Static libraries — XDK build identification

Nine records, all from the same XDK:

| Library | Version |
|---|---|
| D3D9 | 2.0.5426.2 |
| XAPILIB | 2.0.5426.2 |
| XONLINE | 2.0.5426.0 |
| LIBCPMT | 2.0.5426.0 |
| XBOXKRNL | 2.0.5426.0 |
| D3DX9 | 2.0.5426.0 |
| XAUD | 2.0.5426.0 |
| XMP | 2.0.5426.0 |
| XGRAPHC | 2.0.5426.0 |

**XDK 5426** is the key result. `XBOX360_BUILD_VERSIONS.md` lists symbol sets
by XDK build; at build 5426 it lists *Halo 3 [2007-03-07] (MAP)*, title
`4D5307E6`, with a MAP+PDB set for Halo 3 on 2007-03-09 that has function names,
sizes, code, library/object attribution and debug info. Builds on either side
(4314 / 5632) link largely the same XDK code, so those symbol sets are the
starting point for naming XDK imports in this game. See `16-BIOS-AND-XDK.md`.

## 4. Import table

Two imported modules, **427 import records** total:

| Library | Version field | Records |
|---|---|---|
| `xam.xex` | 32.22.32512 | 102 |
| `xboxkrnl.exe` | 32.22.32512 | 325 |

Each record is an 8-byte `(thunk_address, descriptor_address)` pair. Example
from `xam.xex`:

```
82000400  82A40624
82000404  82A40634
82000408  82A40644
```

The first word is the address of the import thunk inside the game image; the
second points to a descriptor structure that lives inside the **compressed
PE**. The actual ordinal number (e.g. `XInputGetState = 641`) is in that
descriptor, so it cannot be recovered from the cleartext XEX header alone.

**Consequence:** do not trust ordinal names printed by simple Python XEX
parsers (including the vendored `parse_xex_imports.py`, which misreads thunk
addresses as ordinals). ReXGlue's codegen decompresses the image and produces
the authoritative import list; that list is what should drive the HLE stub
priority order in `docs/06-HLE-KERNEL.md`.

Rough HLE scope: ~325 kernel imports + ~102 XAM imports. The vast majority are
from the standard XDK surface (memory, threads, files, D3D, XAudio, XInput),
which ReXGlue already models; the work is adapting those models to Android and
filling BioShock-specific gaps.

## 5. What the recompiler still needs from this XEX

Because the PE is compressed, the following must be produced by the host
recompiler (not by the header parser):

1. **Decompressed PE image** and its section table (`.text` range, entry point).
2. **Function boundaries** — from `.pdata` plus XenonAnalyse's BL/branch scan.
3. **Jump tables** — `mtctr`/`bctr` idioms; BioShock's MSVC 2005-era code will
   differ from Sonic Unleashed's patterns, so expect to extend the detector.
4. **ABI helpers** — `__restgprlr*`, `__savegprlr*`, `__restvmx*`, `__savevmx*`,
   `setjmp`/`longjmp`, matching the byte patterns documented in
   `09-RE-FINDINGS.md`.
5. **Import ordinal map** — from the decompressed import descriptors.
6. **Shader binaries** — for XenosRecomp to translate.

These values are placeholders (`TODO`) in `config/bioshock1.toml` by design.
Filling them is the primary reverse-engineering task described in
`09-RE-FINDINGS.md`.
