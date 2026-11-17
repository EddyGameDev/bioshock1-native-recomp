# First boot attempt — host harness

This document records the first attempt to actually **execute** the recompiled
BioShock code (as opposed to only compiling/linking it).

## 1. The 9 remaining runtime symbols — done

`native/src/runtime_symbols.cpp` replaced the last no-op stubs with real
implementations (`config/runtime_non_abi.json`):

| Symbol | Implementation |
|---|---|
| `_ZN3rex3ppc10MMIOLoad32Ej` / `MMIOStore32` | big-endian guest-memory read/write via a thread-local base published by `bs_set_guest_base()` |
| `_ZN3rex3ppc15ResolveFunctionEj` | scans the generated `kPPCFuncMappings[]` table (fields `address` / `host`) and returns the host function pointer |
| `_ZN3rex3ppc4TrapER10PPCContextPht` | logs the trap kind |
| `mmio_slots` | 256-byte data symbol with the exact asm label |
| `Clock::QueryGuestTickCount` | steady_clock-based 100 ns ticks |
| `__aarch64_cas4/8_acq_rel` | `__atomic_compare_exchange_n` (portable on host and arm64) |
| `__main` | no-op CRT entry hook |
| `roundevenf`/`roundeven` | wrappers for newer libm functions MinGW lacks |

Fixes along the way: the prebuilt SDK's `PPCFuncMapping` uses fields
`address`/`host` (the vendored source snapshot differs), the asm label on the
data symbol must be on its definition, and MinGW's `__imp_` prefix collides
with DLL imports in host-only code.

## 2. Host boot harness

`tools/boot-harness/main.cpp` + `BS_BUILD_BOOT_HARNESS` in
`tools/codegen-validate/CMakeLists.txt`:

1. loads the **decrypted, decompressed** PE image (`_work/default_unpacked`);
2. reserves/commits a 2.25 GB guest address space and copies the image at
   guest `0x82000000`;
3. initialises ReXGlue's real `PPCContext` (`r1` stack pointer, MSR);
4. calls the generated guest entry `__imp__xstart(ctx, base)` at `0x8219D698`;
5. installs an exception filter that prints the faulting RVA.

Run:

```powershell
python tools/pipeline.py  # (codegen already done)
cd build\codegen-validate
.\bs1_boot.exe ..\..\_work\default_unpacked
```

## 3. Results (2026-11-17)

Run 1 faulted at host RVA `0x4928365`. Mapping the RVA to the nearest symbol
(via `llvm-nm -n`, subtracting the PE preferred base `0x140000000`) identified
generated `sub_8219EF10`, which calls the XDK's C++ static-initialiser loop via
`REX_CALL_INDIRECT_FUNC`.

Root cause: the **per-module indirect dispatch table** at
`IMAGE_BASE + IMAGE_SIZE + (func - CODE_BASE) * 2` was never populated, so the
generated lookup read garbage host pointers. Fixed in the harness by writing
every `kPPCFuncMappings[]` entry into that guest-memory table before boot:

```
[boot] installed 36625 indirect-dispatch entries
```

Run 2 progressed further and faulted at RVA `0x4C09152`, in generated
`sub_821A10B0` — a kernel/CRT routine that dereferences a guest structure
pointer passed in `r3`. This is the next missing piece of guest state: the
XDK startup path expects kernel initialisation (thread state, TLS, guest
globals) that our minimal runtime does not yet perform.

Both faults are now **concrete and symbolised**, rather than silent crashes.

## 4. Second debugging iteration (same session)

Call-stack capture (`RtlCaptureStackBackTrace` in the fault filter) plus
`llvm-nm` RVA mapping gave the full path:

```
main -> xstart -> sub_8219EF10 (static-init loop)
      -> sub_8262A288 -> sub_8262A360 -> sub_821A10B0 (fault)
```

Instrumenting the weak alias `sub_821A10B0` showed it was called with
`r3 = 0` — a guest global holding an allocator-context pointer was never
initialised. `sub_821A2538` loads it from guest address `0x82B2E2FC`
(`lis r11, 0x82B3; lwz r3, -7428(r11)`; note `-32077` as a signed 16-bit
immediate is `0x82B3`, not `0x8219`).

Fix applied in the harness: write the big-endian pointer `0x83400000` (valid
guest heap memory) into that global before boot. The trace now shows
`r3 = 0x83400000`, so that stage is fixed.

The next fault still lands in `sub_821A10B0`, this time on an access to
`guest_base - 3` (i.e. a wrapped 32-bit guest address near `0xFFFFFFFD`),
which indicates further guest state that the real `rexkernel` runtime would
initialise is still missing. No further guesses were made — this is the
documented stopping point for this iteration.

## 5. Next iteration

1. Continue tracing the wrapped guest address in `sub_821A10B0` (likely an
   uninitialised stack/TLS field in the allocator context).
2. Port the relevant guest-global initialisation from ReXGlue's `rexkernel`
   runtime contract.
3. Re-run and follow the HLE trace until the guest reaches `main`.

Every fault so far has been converted into a specific function, address and
root cause — that is the value of the harness.
