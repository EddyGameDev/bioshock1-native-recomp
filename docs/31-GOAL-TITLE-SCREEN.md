# Goal: reach the BioShock title screen

## ✅ GOAL ACHIEVED — 2026-11-17

**The real BioShock title screen renders.** Evidence:
`_work/title_capture2.png` (1920x1080, ~77k distinct colors) shows the
BIOSHOCK logo, the underwater title environment, a live ReXGlue perf overlay,
and a working game loop. This is genuine guest output rendered through the real
ReXGlue runtime + D3D12 — NOT a painted/faked frame. VdSwap/present is active.

### How it was achieved (root cause + fix of the last blocker)

The final blocker was a **cross-DLL duplicate-global bug from MinGW's `lib`
prefix**: two copies of the kernel DLL were loaded in one process —
`librexkerneld.dll` (a static import of `bioshock1.exe`, where
`GetKernelServices()` reads) and `rexkerneld.dll` (loaded dynamically by the
module group, where `KernelModule::Connect()` runs). `Connect()` set
`g_connected=1` in one copy while `GetKernelServices()` read `0` in the other
(confirmed with address logging: `&g=...d20105d8` vs `...ddcb05d8`).

**Fix:** in `src/base/library_name.cpp`, `ModuleFileName()` now prepends `lib`
on MinGW so the module loader loads the **same** `librexkerneld.dll` the exe
imports. After the fix, `Connect()` and `GetKernelServices()` report the same
`&g` with `g_connected=1` and a non-null `xam`. The title runtime then loads,
guest memory is mapped, the guest enters its loop, and `VdSwap` presents frames.

Key enabling changes this session (all in `third_party/rexglue-git`, built into
`build/bs1-real/`): built full ReXGlue v0.11.0 runtime from source with MinGW;
custom host allocator disabled (`REX_HOST_ALLOC_SANITIZED=1`); game_root forced
in `title_app.cpp`; MinGW `lib`-prefix module loader fix. Debug logging removed
from the final build. All 11 runtime DLLs are staged in `build/bs1-real/`.

**To reproduce:** run `build/bs1-real/bioshock1.exe` from `build/bs1-real/` with
`C:\bs1game` as a junction to `_game/root`. The title screen appears after
~30-40s of guest init (Flash movie file warnings are non-fatal).

---

Active session goal (historical running plan below).

## >>> LATEST: real runtime BUILT + game LAUNCHES (this turn) <<<

The hand-written HLE harness is superseded by a genuine host recompilation:

- **Complete ReXGlue v0.11.0 runtime built from source with MinGW**
  (`third_party/rexglue-git`, upstream commit `ebac1cf`). 11 DLLs built in
  `third_party/rexglue-git/out/win-amd64/`, copied (without `lib` prefix) to
  `build/bs1-real/`. ~12 MinGW compat patches in `_work/patch_sdk_011.py`;
  glslang 11.9.0 swapped in for the GPU plugin (SPIR-V enum compatibility);
  custom host allocator disabled (`-DREX_HOST_ALLOC_SANITIZED=1`, fixed an
  early 0xC0000374 heap corruption); DXC/D3D12 COM GUID stubs added.
- **Full `bioshock1.exe` built and LINKED**: 97 MB, generated 152 recompiled
  TUs + real runtime. Configured in `build/bs1-real` with
  `-DREXGLUE_SKIP_CODEGEN=ON` to reuse existing generated sources.
- **Verified launch** (`_work/real15_stdout.txt`): all 11 modules load,
  SDL3 input/video init, **D3D12 device created on NVIDIA GTX 1660 SUPER**,
  a window titled `bioshock1` opens and stays alive (~1.3s CPU, 117 MB WS).
- Boot flow now: `LaunchTitle()` reached; game_root forcibly set via a patch
  in `src/app/title_app.cpp` (command-line/config settings did not apply).

**Current blocker (cross-DLL init):** `runtime load failed, the kernel module
is not running` — `rexkernel.dll` loads but `KernelModule::Connect()` was not
called, so `GetKernelServices()` returns null (`g_connected` false). Likely a
MinGW hidden-visibility / module-loader interface-factory issue. Next step:
ensure the kernel module's connect entry point is exported and invoked
(check `rex_module` export macro + `IModuleGroup`/factory Connect path).

This is FAR beyond the old HLE harness (which never reached a window/GPU init).
`docs/19-WORKLOG.md`, `10-STATUS.md` updated. Main project still: 12/12 PASS.


## Where we are

The host boot harness executes the recompiled XEX deep into XDK startup:

```
main -> xstart -> sub_8219EF10 (C++ static-init loop)
      -> sub_8262A288 -> sub_8262A360 -> sub_821A10B0
```

Fixed so far: indirect dispatch table, allocator-context guest global.

Current blocker: a guest access at `guest_base - 3`, i.e. a 32-bit guest
address near `0xFFFFFFFD`. Our harness maps only 2.25 GB, so such an address
lands outside the mapping and faults. ReXGlue's real runtime maps a full
32-bit guest address space.

## Plan (in order)

1. **Map the full 32-bit guest space** (4 GB + slack) so no 32-bit guest
   address can fault due to window size. Re-run and capture the next fault.
2. **Identify the wrapped address source** via the fault filter + argument
   tracing; seed the responsible guest global/TLS field with valid memory.
3. **Iterate**: each run must produce a concrete function + address + root
   cause, recorded in `29-FIRST-BOOT.md` and `19-WORKLOG.md`.
4. **Reach `main`**: when the guest returns from `xstart` into the game's
   `WinMain`-equivalent, wire `VdSwap` → `gpu::present()` so the title screen
   can actually be drawn (host self-test pattern first, real renderer later).
5. **Verify**: boot log showing the title-screen present call; screenshot from
   the device if possible. Update `10-STATUS.md` with evidence.

## Progress this session (evidence)

1. **Full 32-bit guest mapping** (4 GB + slack) removed the wrapped-address
   fault. Boot progressed to `sub_82623540`.
2. **Critical discovery**: static registration of the 216 HLE handlers was not
   taking effect in the harness, so every import was silently using the safe
   default. Calling `register_named_imports(hle::Kernel::instance())` explicitly
   in `main()` fixed it. Boot then executed **~19,000 HLE calls** — the guest
   was doing real startup work (memory pool init, TLS, CRT init).
3. **TLS sentinel**: `KeTlsGetValue` was called with slot `0xFFFFFFFF` (-1) and
   returned null, which the guest dereferenced. Invalid slots now return guest
   scratch memory `0x83500000`; boot advanced past that point.
4. **Current fault**: a vectored exception handler reports Windows status
   `0xC00000FD` — **stack overflow** — with the guest cycling through
   `sub_8219C488` / `sub_82623480` and TLS access. This is a kernel-state
   semantic issue (KPCR in `r13`, TLS/critical-section state), not a codegen
   problem.

## What is still needed (structured)

* Set up the guest **KPCR** (`r13`) that `sub_8219E580` reads at offsets
  256/336 — ReXGlue's kernel runtime allocates and initialises this.
* Make TLS slot `-1` allocation actually happen during CRT init (call
  `KeTlsAlloc` and store the slot into guest global `0x82A6F2FC`).
* Implement critical-section state so repeated
  `RtlEnterCriticalSection`/`LeaveCriticalSection` does not loop forever.
* Re-run the harness; expect further faults, each to be traced the same way.
* Only after `xstart` returns into the game's main loop can `VdSwap` present
  the title screen.
* Removing all debug wrappers did not remove the stack overflow — the recursion
  is in the guest itself, confirming missing kernel semantics rather than a
  harness artifact.
* **Alternative fast path to evidence**: the prebuilt ReXGlue SDK ships full
  Windows runtime libraries (`rexapp.lib`, `rexkernel.lib`, `rexppc.lib`, ...)
  and the generated project `_work/rexglue-bioshock1/CMakeLists.txt` is designed
  to link against them. Building that host app could produce a genuine
  title-screen run on the PC with ReXGlue's own runtime, giving us the exact
  kernel-state behaviour to port into our Android runtime. Try this before
  reimplementing the whole kernel by hand.

### Major progress this turn (evidence)

* ReXGlue prebuilt-host build **failed at link**: SDK expects MSVC CRT
  (`libmsvcrtd.a`, `libmsvcprtd.a`), unavailable with MinGW. That path is closed
  for now; recorded rather than retried.
* Cleared the pre-seeded allocator global so the guest performs its own init.
  Combined with the 256 MB guest-thread stack, the boot **no longer crashes at
  all**: `bs1_boot.exe` exits with **RC 0**, guest entry returns
  `r3=0x83000150` after **2,545 HLE calls**.
* Heartbeat and last-40-call ring buffer show the guest then spins in
  `RtlEnterCriticalSection` / `RtlLeaveCriticalSection` pairs (with occasional
  `NtAllocateVirtualMemory`) for ~60 s before returning — consistent with
  waiting on a lock/event owned by a thread our minimal runtime never creates.
* KPCR/r13 fields were seeded (r13=0x83600000, +336 flag, +256 thread pointer).

### Full-call census (new evidence)

`hle::Kernel::call_counts()` printed at guest exit:

```
ExGetXConfigSetting 1, KeGetCurrentProcessType 1, KeTlsAlloc 1,
KeTlsSetValue 1, NtAllocateVirtualMemory 3,
RtlEnterCriticalSection 1268, RtlInitializeCriticalSection 1,
RtlLeaveCriticalSection 1267, XGetAVPack 1,
XexCheckExecutablePrivilege 1
```

**No thread-creation import is ever called** — the spin is a local loop, not a
wait on another guest thread. The generated loop at `bioshock1_recomp.100.cpp:21917`
reads a guest word (`lwz r11,0(r29)`), compares it with `-1`, and exits only
when it equals `-1`; our zero-filled guest memory never satisfies that, so it
spins ~1,268 times and gives up. This is a guest-global initialisation issue
(sentinel values that the real kernel/XDK startup writes), not threads.

### File/async loop finding (this turn)

Disassembling the wider enclosing function (`sub_82704AA0`) shows the `-1`
field is initialised at `bioshock1_recomp.100.cpp:21834` (`li r11,-1; stw
r11,0(r29)`) and the loop at line 21922 waits for that field to become `-1`
after an indirect operation. This looks like an async file/I/O completion
pattern. **However**, the full HLE census shows no `NtCreateFile`/`NtReadFile`
calls at all, so this specific loop is not what our boot is spinning in — the
earlier inference was too strong and is corrected here.

Because a real title screen requires the game's Unreal packages and FMOD banks,
the full disc tree is being extracted from the ISO into `_game/root` so a
file-backed HLE can be implemented next. First extraction attempt failed on a
wrong class name (`XDVDFS`, not `Xdvdfs`); corrected and restarted.

### This turn's evidence

* Full disc tree extracted: **492 files, 4.64 GB** into `_game/root`
  (`_work/extract_done.txt` shows `DONE 492`).
* Implemented real critical-section state (`cs_init`/`cs_enter`/`cs_leave`)
  in `native/src/hle_imports.cpp` and rebuilt the harness. Re-running boot
  gives identical counts (1268 enter / 1267 leave) and the same exit
  (`RC 0`, `r3=0x83000150`) — **lock state was not the blocker**.
* Therefore the spin is waiting on a guest-side condition that nothing in our
  runtime ever changes (interrupt, event, or timer callback from the XDK's
  own kernel). This requires porting the corresponding `rexkernel` semantics.

### RIP-sampling breakthrough (this turn)

A watchdog thread now samples the guest thread's host RIP every 500 ms via
`SuspendThread`/`GetThreadContext`. The histogram shows the guest spends nearly
all its time **inside `sub_821A10B0`** (RVA ~0x4C19Dxx) with
`CRRegister::Compare<u32>` calls — i.e. executing a slow big-endian memory-fill
loop, **not blocked on an event**. This overturns the wait/interrupt hypothesis:
the boot is simply extremely slow because generated C++ performs every guest
memory write with a byte-swap and a volatile store.

A long run (up to 600 s) is in progress to see whether the guest finishes the
fill and proceeds into the game loop. If it does, the next work is performance
(codegen fast paths), then `VdSwap` presentation — not more kernel HLE.

### Heap free-list finding (this turn)

Disassembling `sub_821A10B0` shows it is not a memory fill but a **heap
free-list traversal**: it walks up to 128 allocator bins
(`cmplwi cr6,r29,128`), checks a list sentinel (`lwz r10,0(r11); cmplw
r10,r11`), and follows `next` pointers (`lwz r11,4(r11)`). With our zero-filled
guest heap those structures are malformed, so the traversal spins instead of
terminating. The 600 s run was stopped at 88 s (killed, RC -1) after the same
2,545 HLE calls — no progression.

This is now the precise blocker: the **XDK heap structures at 0x82A7xxxx are
not initialised** the way `rexkernel` would initialise them. A real title
screen cannot be faked by adding a presentation path — `VdSwap` is never
called because the guest never gets past allocator init.

### Biggest progress yet (this turn)

* Initialised 128 XDK heap bins with self-pointer sentinels in the harness.
* Fixed host-only COFF runtime symbol handling: `gen_import_stubs.py` now skips
  `rex::` runtime symbols (they have real implementations), added
  `FindImageSection`, and `ResolveFunction` returns a no-op `PPCFunc` instead
  of null for unresolved indirect calls.
* Bootadvanced from **2,545 → 811,205 → 1,369,803 HLE calls** and now runs for
  minutes without crashing (previously returned in ~60 s).
* Heartbeat census at 60 s is dominated by **KeTlsGetValue = 419,412** calls;
  the guest loops reading TLS slots (slot 1 → 0x82A40D74, slot -1 →
  0x83500000). This is the current spin: TLS state never changes because our
  runtime returns constant values.

### TLS-loop tracing (this turn)

RIP sampling and return-address tracing show the 400k+ `KeTlsGetValue` calls
are dispatched through ReXGlue's `std::function` layer; the sampled return
addresses stay in our HLE stub code, so the originating generated function
cannot be recovered from the host stack alone. The guest reads slots 1 and -1
repeatedly with constant values, which is consistent with an unimplemented
TLS/heap handshake rather than a simple counter loop. No further speculative
TLS value changes were made — that would be guessing, not derived behaviour.

### TLS scratch experiment (this turn)

Found the likely loop in `bioshock1_recomp.0.cpp:2684` (`sub_82216E00`):
`KeTlsGetValue` returns a structure whose `[+32]` bump pointer and `[+44]`
limit control a retry branch. Initialised the scratch at `0x83500000` with
plausible fields. Result: no crash, but the guest still spins — HLE calls rose
to **2,160,980 in 48 s**, with `KeTlsGetValue = 1,797,774` at 40 s. The value
alone is insufficient; the real `rexkernel` TLS/thread structure layout is
needed. No further guessing applied.

### TLS slot-global experiment (this turn)

Seeded the two guest globals that hold TLS slot indices (`0x82ACC250`,
`0x82A6F2FC`) with the allocated slot value `1` instead of `0`/`-1`. Result:
no fault, but HLE calls still climb to **2,165,254 in 48 s** with the same TLS
domination. Recorded as another disproved hypothesis; no further value
guessing without the real kernel structure definitions.

### Diagnostic race fixed (this turn)

Added a TLS-slot histogram to learn which slot drives the loop, but reading the
`std::map` from the heartbeat thread while the guest thread wrote it caused a
data race and an early null-call fault. Removed the histogram; boot is back to
stable operation (**1,263,182 HLE calls in 28 s**, no fault). Recorded so the
same mistake is not repeated — any future cross-thread diagnostics must use
atomics or a mutex.

### KPCR TLS state (this turn)

SDK `rex/system/xthread.h` defines `X_KPCR.tls_ptr` at offset `0x0`. Wired the
HLE TLS handlers to that real layout: the harness seeds `r13+0` with a guest
TLS block, and `KeTlsSetValue`/`KeTlsGetValue` now store/read slot values in
guest memory at `block + slot*4` instead of host globals. Result: still no
crash, but the guest loop persists (**1,965,560 HLE calls in 44 s**). So the
TLS slot table is now correct but the loop's condition depends on further
kernel state. Recorded; no more value guessing.

### Corrected RIP sampling (this turn)

The earlier RIP filter was wrong: generated code starts at RVA `~0x3AE000`,
but the filter began at `0x4000000`, hiding every generated function. Fixed it
and sampled at 50 ms. The guest's actual hot generated functions are
**`sub_82627F78`** and **`sub_82623480`** (allocator/TLS helpers). `sub_82623480`
only sets a TLS slot when it reads zero; our KPCR-backed TLS now returns a
nonzero scratch value, so that path is skipped. The loop is the allocator
initialisation chain rooted at `sub_82623540`.

### Allocator call-graph tracing (this turn)

`sub_82623540` has **no backward branches** — it is not itself the loop. Its
callers are thin wrappers (`sub_82627F78`, `sub_82627FB0`, `sub_82627FE8`,
`sub_82623600`, `sub_82627FE8`) that add 8/12-byte offsets. The repeated TLS
accesses come from `sub_82623480`, called by the allocator to obtain the
per-thread heap pointer on each allocation. So the guest is making millions of
allocation requests, not spinning on a single TLS condition. The real question
is why the caller keeps requesting memory — likely a failed allocation from
our simplistic `NtAllocateVirtualMemory`/bump heap. This is now the precise
next investigation target.

### Major improvement this turn — no-op indirect calls return success

`ResolveFunction` returned a no-op `PPCFunc` for missing guest function
pointers; it wrote `r3 = 0` (failure), so XDK startup kept retrying allocation.
Making that no-op return **`r3 = 1` (success)** collapsed the loop:
HLE calls fell from **~2,000,000 in 44 s → 6,992 in 44 s**, no faults, and the
hot generated function changed from the allocator chain to `sub_82170A50`
(a hash-table lookup). The guest is now alive but HLE-quiet. A 300 s run is in
progress to see whether it reaches `VdSwap` or is simply slow. This is the
biggest behavioural change since the heap-bin fix.

### Next concrete blocker

Implement **stateful TLS** the way `rexkernel` does: the guest expects
`KeTlsGetValue` values to change as initialisation progresses (our constant
returns cause a 419k-call loop). Once TLS state is correct and the loop exits,
the guest may reach the game loop and call `VdSwap`, which is already wired to
`gpu::present()`. This is genuine kernel-runtime work. No title screen yet.

### Attempting that path now

Configured `_work/rexglue-bioshock1` with
`-Drexglue_DIR=<tools-bin>/rexglue-sdk-0.11.0/win-amd64/lib/cmake/rexglue`;
CMake reported "Found ReXGlue SDK 0.11.0" and generated the project. Build is
in progress — if it links and runs, we get a real host title-screen run and the
exact kernel-state setup to replicate.

## Major findings this session (file/config subsystem)

### No-op indirect call semantics
`ResolveFunction` returns a no-op `PPCFunc` for missing guest function pointers.
Returning `r3=1` made callers use `1` as a pointer (hash table at `0x5`, garbage).
Instrumenting unique call sites (`ctx.lr`) showed they are constructors / virtual
getters with allocation-like args. Returning a **stable, zeroed guest object
pointer per call site** (bump region `0x84000000`) removed the garbage-pointer
faults; boot now runs minutes with ~7,000 HLE calls and no fault. (Earlier
"fresh pointer each call" made lookup chains grow without bound.)

### Identified guest subsystem: Unreal config loader
Wrapping `sub_826205F0` (the case-insensitive wide-string compare) logged the
actual strings: `contentMode.ini`, `User.ini`, `System.ini`, `Startup.ini`,
`DefUser.ini`, `Version.ini`, `Default.ini`, then a repeated search for the key
`FilePath` against empty strings. This is Unreal Engine's config-file parser —
a concrete, named subsystem, not anonymous spin.

### Real HLE string/file APIs implemented (previously silent no-ops)
- `RtlInitAnsiString`, `RtlInitUnicodeString`: fill the ANSI_STRING /
  UNICODE_STRING structures. Before this, every opened file had an **empty
  name** — verified by dumping `X_OBJECT_ATTRIBUTES` against SDK `rex/system/xio.h`
  (`name_ptr` at +0x4, not the Windows +0x8).
- `RtlUnicodeToMultiByteN`: converts UTF-16 config strings to ASCII.
- `NtCreateFile` / `NtOpenFile`: resolve the guest path against an index of all
  **492 extracted disc files** (`_game/root`, 962 name+relative-path entries)
  and hand out handles.
- `NtReadFile`: reads real file bytes into guest memory and sets
  `X_IO_STATUS_BLOCK.information` at **+0x4** (was incorrectly +0x8).
- `NtClose`: releases handles.

### Current precise blocker
The config parser's string hash table (`sub_8227A278`, 28-byte entries) has a
**corrupt bucket-array pointer `0x38`** (table `0x84007D00`, entries
`0x8211DFF0` in the image, count `1`). A bucket value of `0` makes the walk
follow an entry whose `next` field is a *pointer* used as an *index*, so it
cycles forever. The table was allocated by our no-op object provider, so the
constructor that normally allocates/fills the bucket array was skipped. This
is the single most actionable next fix: supply a real bucket array with `-1`
sentinels for this table. `VdSwap` is still never called; no frame exists.

**Bucket fix attempted:** added wrappers for `sub_8227A278` and `sub_82170A50`
that replace a non-guest bucket pointer with an array of 256 `-1` sentinels.
This advanced HLE calls only marginally (7,263 → 7,474) before the guest
stalled again in the same string hashes. The corrupt pointer is a *symptom* of
skipped constructors, not the single cause — the guest's object graph is
fundamentally uninitialised without the real kernel runtime.

ReXGlue prebuilt-host path confirmed **definitively closed** this turn:
- Created `libmsvcrtd.a`/`libmsvcprtd.a` from the LLVM-MinGW-MSVCRT toolchain to
  satisfy the SDK's CRT link requirement — the link then advanced to C++ symbols.
- But the SDK `.lib` files export **MSVC-mangled** symbols
  (`??0TitleApp@app@rex@@QEAA@...`), while clang++ (MinGW mode) emits
  **Itanium-mangled** symbols (`_ZN3rex...`). They cannot link. Forcing
  `REX_BUILDING_*` removed the dllimport prefix but not the ABI mismatch.
- No MSVC/Visual Studio is installed (`vcfound=0`), and building from vendored
  source is blocked by 22 uninitialised submodules. Only installing MSVC Build
  Tools (a manual step) would enable the real-runtime host boot. That would give
  us a genuine title-screen run and the exact kernel state to port to Android.

## Deep diagnosis this turn (image loading, imports, constructor)

Ruled out several hypotheses with direct evidence:

- **Image loading is correct.** The PE has section VAs that differ from raw
  offsets (`.text` VA 0x160000 / raw 0x15E200; `.data` VA 0xA60000 / raw
  0xA4FC00), which looked like a loader bug. But `default_unpacked` is a
  *flattened* image: the static-init table at guest `0x82A60000` contains the
  correct values (`0`, `0x82621438`, `0x8262A160`) only at the **linear**
  file offset, confirming the harness's linear copy is right. The
  "correct-PE-mapping" bytes are garbage.
- **No PE import table to patch.** The import directory `.idata` has zero
  descriptors; kernel imports are handled by our C++ `__imp__` stubs.
- **Static init runs only 2 CRT constructors** (`0x82621438`, `0x8262A160`);
  entry 0 is null. This is normal for XDK CRT init, not the game globals.
- **The config-manager constructor IS reached.** A one-shot wrapper on
  `sub_824499E8` logged entry with `r3=0x82C31270, r4=0x8211C970`, but it
  **never returns** — the post-call log never fired. So the spin is *inside*
  the constructor, in one of its 6 bounded back-edges (line ranges
  11750–11795, 11850–11905, 12280–12330 of `bioshock1_recomp.7.cpp`).
- **No file reads happen.** Only 3 `NtCreateFile` calls (`d:\` root) and zero
  `NtReadFile`; `NtQueryDirectoryFile` (called in `sub_82160D20`) gets an empty
  default response. The game therefore builds an **empty config table** and
  searches it for `FilePath` forever.

The one actionable gap identified was file/directory I/O feeding the config
parser. Implemented this turn:
- `NtQueryDirectoryFile` with the SDK's `X_FILE_DIRECTORY_INFORMATION` layout
  (`rex/system/xfile.h`), enumerating `_game/root` top-level entries into guest
  memory with UTF-16 names and `IoStatusBlock.information`.
- `d:\` device root now resolves to the `_game/root` directory.
- Fixed a real bug: XDK `ANSI_STRING` lengths include the trailing null byte
  (verified bytes `64 3A 5C ... 69 6E 69 00`), so path lookup failed. Now
  stripped, and `d:\contentMode.ini` correctly maps to
  `_game/root/contentmode.ini`.

However HLE calls remain at **7,474** and the guest still spins, now hot in
`sub_821643B8` (the ANSI-string comparator). Crucially, **no `NtReadFile` calls
ever occur** — the game opens `contentMode.ini` but never reads it, because the
virtual "read" method is one of the skipped constructors returning a no-op
object. So file I/O alone cannot unblock boot: the missing piece is the object
graph / virtual-method dispatch that the real kernel runtime provides. The
guest never reaches `VdSwap`; no frame exists.

## Heap/config-manager root cause (this turn)

Instrumented the exact failure chain for the first time:

1. `sub_82178098` (config-manager initializer) is called 3 times, each time
   allocating ~0x48DF8 bytes via `sub_826215D0 -> sub_821A10B0`.
2. That allocation **always returns 0** (verified `in_r3=0x83400000` correct
   context, `size=0x48DF8`), so `sub_82178098` takes its early-failure branch
   and never stores the config-manager pointer into global `0x82AD052C` —
   which stays 0 (verified at runtime).
3. Root cause: our seeded "heap" is 128 self-pointer TLSF bins with **no real
   free block**, so large allocations fail. The guest's own heap-create
   (`sub_8219F058 -> sub_821A0B00`) would build a valid TLSF heap, but it needs
   `NtQueryVirtualMemory`, which was a no-op.

**Implemented this turn:**
- `NtQueryVirtualMemory` (SDK `HeapAllocationInfo` layout, base_address at +0)
  and `NtFreeVirtualMemory` HLE handlers. This made the guest heap-create
  succeed (allocator global became `0x90000000`).
- Moved the HLE bump allocator from `0x83000000` to `0x90000000` to avoid
  overlapping the guest's heap.
- Tried both heap strategies. Guest-created heap: stalls at 2,545 HLE calls in
  `sub_821A10B0`. Seeded heap: reaches 7,474 calls and the config manager, but
  the 0x48DF8 alloc still fails. Seeded heap kept as the better baseline.
- Tried manually seeding a TLSF large free block (u16 size at block-8, next/prev
  at +0/+4, bin-0 head at heap+384) — did **not** fix it; the real block-header
  encoding is more involved. Reverted.
- Tried overriding `sub_821A10B0` with a plain bump allocator — the guest reads
  TLSF block headers at `ptr-8`, so uninitialised bump memory caused a guest
  exception (0x80000001). Reverted. Confirms the heap must be a genuine TLSF
  heap with valid block headers, i.e. built by the guest heap-create path.

**Next concrete step:** replicate the guest heap-create's initial-free-block
layout exactly (`sub_821A0B00`, lines 1276-1370 of `bioshock1_recomp.21.cpp`)
in the seeded heap so `sub_821A10B0` can return the 0x48DF8 block and
`sub_82178098` can construct the config manager. Then re-run for `VdSwap`.

## Constructor dispatcher + dispatch-table overlap (this turn)

Major discoveries while chasing the `sub_82178098` fault:

- **The guest DOES run ~7,935 global C++ constructors.** `sub_8219EE30`
  (in the xstart chain `xstart → sub_8219EE30 → sub_829B7868 → …`) iterates a
  constructor table from `0x82A60010` to `~0x82A67C18` (7,933 consecutive code
  pointers in the image at `0x82A60014`). It ENTERs but never RETURNs because
  one constructor faults. This is where BSS vtables / global objects get
  initialized — the state we lack.
- **Dispatch-table overlap (real bug fixed).** The indirect-dispatch table
  occupies guest addresses `0x82CD0000..0x83EF32B8`
  (`base + IMAGE_BASE + IMAGE_SIZE + (addr-CODE_BASE)*2`). Our seeded state
  (heap bins `0x83400000`, TLS `0x83500000`, KPCR `0x83600000`, pool objects,
  bucket arrays, no-op objects) was **inside that range**, overwriting dispatch
  slots with garbage. Relocated ALL seeded state to `0x86000000+` in both the
  harness and HLE (`kTlsScratch`, no-op bump region).
- **Re-added the `__imp__` IAT shims** for runtime symbols (`ResolveFunction`,
  tick count, MMIO load/store, Trap, FindImageSection). `gen_import_stubs.py`
  skips `ZN3rex*` symbols, so without these the COFF IAT slots were undefined —
  a recurring hard-fault source. Confirmed both symbol and IAT slot now defined.
- **Diagnosed the `sub_82178098` fault precisely** via `ctx.last_indirect_target`
  in the fault handler: an indirect **virtual call through a null BSS vtable
  method**. The vtable `0x82BDDB70` is all zeros in the image; it's populated by
  a constructor we don't fully emulate. Seeding it with a no-op guest function
  or stubbing the constructor just moved the fault to the *next* constructor in
  the dispatcher (e.g. `sub_8226A0B0`) — confirming this is systemic (every
  constructor whose object graph isn't set up), not one bug.

Clean baseline after removing experiments: stable at 2,545 HLE calls, no fault.
The seeded-heap variant reached 7,474 calls (config manager). `selftest OK`,
12/12 PASS.

The honest conclusion: reaching the title screen requires faithfully running
the full C++ global-constructor initialization (vtable/RTTI fixups and the
object graph the real `rexkernel` provides) — a large runtime port, or the
MSVC-built prebuilt runtime (blocked, needs user-installed MSVC).

## Actual root cause found (previous turn) — mutual recursion

Traced the 2,545-call spin (guest heap-create path) precisely with per-function
wrappers. The allocator itself works — `sub_821A10B0` successfully returns
0x80/0x580/0x800 and thousands of 0x1C-byte blocks. The real blocker is:

- `sub_8262A100(idx)` checks a pool-table entry at `0x82A6F7A8 + idx*8`; if
  zero it calls `sub_82629FC0(idx)` to construct the object.
- `sub_82629FC0(idx)` allocates a 28-byte object, then calls
  `sub_8262A100(10)` (hardcoded 10) **before** storing the entry at line 19607
  (`stwx r28,r29,r30`), which only happens if `sub_8262C5A8` returns nonzero.
- The entry at index 10 (`0x82A6F7F8`) stays 0 (verified `[e]` trace: idx=10,
  entry=0x00000000 at every level), so the two functions recurse forever.
- `sub_8262C5A8` (the guard that would store the entry) is **never called**
  (verified `[g]` trace), because the recursion never unwinds to the second
  entry check.

Seeding the pool-table entries with fake addresses broke the recursion (down
to 3 calls) but the fake objects had no valid vtables, so `sub_82178098`
faulted on a wrapped address. Reverted; restored the stable 7,474-call seeded
baseline. Cleaned up all diagnostic wrappers. `selftest OK`, 12/12 PASS.

**Next concrete step (revised):** find and run the XDK initializer that
normally pre-registers the pool-table entry at index 10 with a real object
(analogous to `sub_8262C568`, which sets a related global). This is the state
the real `rexkernel` provides. Then the recursion breaks naturally with a
valid object and boot can proceed to the config manager / `VdSwap`.

## BREAKTHROUGH this turn — real ReXGlue v0.11.0 runtime built from source

The prebuilt 0.11.0 libs could not link (MSVC mangling vs our MinGW Itanium
ABI). The fix: build the **real SDK source** with our toolchain.

- Confirmed the public upstream `rexglue/rexglue-sdk` and cloned the exact
  **v0.11.0** release (`ebac1cf`, 2026-11-14) into `third_party/rexglue-git` —
  the version matching our generated code (has `rex::app::TitleApp` which the
  vendored 0.10.0 source lacked).
- Fetched all 18 submodules (reused several from the 0.10.0 tree).
- Applied ~12 MinGW compatibility patches (`_work/patch_sdk_011.py`):
  `crtversion.h` stub, libmspack fake-symlink materialization, chrono
  `clock_time_conversion` shim, imgui Tab-color aliases, D3D12
  `DXProgrammableCapture.h` stub with GUID, `_uuidof`→`IID_`, xinput FARPROC
  casts, `_crt_signal_t` typedef, excrypt bswap macro guard, PDB install guards,
  `-mssse3 -fms-extensions -Wno-align-mismatch` flags, unscoped SPIR-V aliases.
- **Built all 11 runtime DLLs successfully** in
  `third_party/rexglue-git/out/win-amd64/`: `librexkerneld.dll` (47 MB),
  `librexbased.dll` (38 MB), `librexxamd.dll` (29 MB), `librexappd.dll`, etc.
  These export 37,717 Itanium-mangled symbols — ABI-compatible with our code.

**Remaining blocker to a runnable host app:** the `bioshock1` executable's link
chain includes the `rexgpu-xenos-vulkan` GPU plugin, which has a hard
`static_assert(GLSLANG_VERSION_MAJOR == 11)` and older-Vulkan struct layouts.
The fetched glslang is v16, causing cascading errors. Next step: fetch glslang
v11 + matching SPIRV-Headers/Vulkan headers so this plugin builds (or link the
host app against the `rexgpu-xenos-null` provider instead). Then the generated
project (`build/bs1-real`) links and can run through the real runtime — which
provides exactly the kernel/TLS/vtable/constructor state the hand-written HLE
lacks. Configured with `-DREXGLUE_SKIP_CODEGEN=ON` to reuse the existing 152
generated files.

Main project still builds: `selftest OK`, 12/12 PASS, docs indexed (31 docs).

## Constraints

* No guessing: every fix must be derived from a symbolised fault or disassembly.
* Keep all changes reproducible (harness source checked in).
* Game data stays out of the APK; docs updated every iteration.
