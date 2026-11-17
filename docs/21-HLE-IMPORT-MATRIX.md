# HLE import matrix — measured from generated code

This is the **authoritative** HLE scope, derived from the undefined symbols of
the 152 compiled ReXGlue-generated object files (not estimated from XEX header
record counts). Method:

```
llvm-nm -u @<all generated .obj> | sort -u
```

Result: **36,647 unique undefined symbols**, of which:

* **36,425** are `__imp__sub_XXXXXXXX` / ABI-helper symbols that are internal
  to the generated code (weak aliases for override and `__restgprlr*` /
  `__restfpr*` / `__restvmx*` helpers — ReXGlue emits the full range, e.g.
  `__restvmx_100` … `__restvmx_127`).
* **216** are **real Xbox kernel / XAM imports** that the Android runtime must
  provide. This is the actual HLE workload — much smaller and better defined
  than the 427 raw import records measured in `03-XEX-ANALYSIS.md`.

Machine-readable lists: `_work/undefined_symbols.json`, `_work/hle_imports.json`
(gitignored, recreate by re-running codegen).

## 1. Import families (counts)

| Family | Count | Role / difficulty |
|---|---|---|
| X* (XAudio, XMA, Xex, crypto, notifications) | 35 | XMA2 decode is hardest; others are straightforward |
| Ke (threads, sync, IRQL, TLS, timers) | 32 | core runtime; map to pthreads/atomics |
| Nt (files, objects, sync) | 28 | plus guest filesystem mapping |
| XAM (content, users, UI stubs, input) | 28 | mostly safe no-ops for single-player |
| Rtl / CRT | 22 | string/memory helpers, SEH, time conversion |
| Graphics (Vd*/D3D) | 20 | VdSwap → GPU present; Vd* → Xenos init |
| Network (NetDll_*) | 16 | **stub only** — no Xbox Live |
| Ex (pool, threads, config) | 6 | guest heap + threads |
| Mm (memory) | 6 | guest memory management |
| Ob (objects) | 4 | handle bookkeeping |
| Debug (Dbg*) | 2 | logging |
| Io | 2 | device close |
| Hal | 1 | `HalReturnToFirmware` → clean exit |

## 2. Full import list by family

### X* (35)
`XAudioGetVoiceCategoryVolume`, `XAudioGetVoiceCategoryVolumeChangeMask`,
`XAudioRegisterRenderDriverClient`, `XAudioSubmitRenderDriverFrame`,
`XAudioUnregisterRenderDriverClient`, `XGetAVPack`, `XGetGameRegion`,
`XGetLanguage`, `XGetVideoMode`, `XMACreateContext`, `XMADisableContext`,
`XMAEnableContext`, `XMAGetInputBufferReadOffset`,
`XMAGetOutputBufferReadOffset`, `XMAGetOutputBufferWriteOffset`,
`XMAInitializeContext`, `XMAIsInputBuffer0Valid`, `XMAIsInputBuffer1Valid`,
`XMAIsOutputBufferValid`, `XMAReleaseContext`, `XMASetInputBuffer0Valid`,
`XMASetInputBuffer1Valid`, `XMASetInputBufferReadOffset`,
`XMASetOutputBufferReadOffset`, `XMASetOutputBufferValid`, `XMsgInProcessCall`,
`XMsgStartIORequest`, `XNotifyGetNext`, `XeCryptSha`,
`XeKeysConsolePrivateKeySign`, `XeKeysConsoleSignatureVerification`,
`XexCheckExecutablePrivilege`, `XexGetModuleHandle`, `XexGetProcedureAddress`,
`XexLoadImageHeaders`

### Ke (32)
`KeAcquireSpinLockAtRaisedIrql`, `KeBugCheck`, `KeBugCheckEx`,
`KeDelayExecutionThread`, `KeEnableFpuExceptions`, `KeEnterCriticalRegion`,
`KeGetCurrentProcessType`, `KeInitializeSemaphore`, `KeLeaveCriticalRegion`,
`KeLockL2`, `KeQueryPerformanceFrequency`, `KeQuerySystemTime`,
`KeRaiseIrqlToDpcLevel`, `KeReleaseSemaphore`,
`KeReleaseSpinLockFromRaisedIrql`, `KeResetEvent`, `KeResumeThread`,
`KeSetAffinityThread`, `KeSetBasePriorityThread`, `KeSetDisableBoostThread`,
`KeSetEvent`, `KeTlsAlloc`, `KeTlsFree`, `KeTlsGetValue`, `KeTlsSetValue`,
`KeUnlockL2`, `KeWaitForMultipleObjects`, `KeWaitForSingleObject`,
`KfAcquireSpinLock`, `KfLowerIrql`, `KfReleaseSpinLock`,
`KiApcNormalRoutineNop`

### Nt (28)
`NtAllocateVirtualMemory`, `NtClearEvent`, `NtClose`, `NtCreateEvent`,
`NtCreateFile`, `NtCreateMutant`, `NtCreateSemaphore`,
`NtDeviceIoControlFile`, `NtFlushBuffersFile`, `NtFreeVirtualMemory`,
`NtOpenFile`, `NtPulseEvent`, `NtQueryDirectoryFile`,
`NtQueryFullAttributesFile`, `NtQueryInformationFile`,
`NtQueryVirtualMemory`, `NtQueryVolumeInformationFile`, `NtReadFile`,
`NtReadFileScatter`, `NtReleaseMutant`, `NtReleaseSemaphore`,
`NtResumeThread`, `NtSetEvent`, `NtSetInformationFile`, `NtSuspendThread`,
`NtWaitForMultipleObjectsEx`, `NtWaitForSingleObjectEx`, `NtWriteFile`

### XAM (28)
`XamContentClose`, `XamContentCreateEnumerator`, `XamContentCreateEx`,
`XamContentDelete`, `XamContentGetCreator`, `XamContentGetDeviceData`,
`XamContentGetDeviceState`, `XamContentSetThumbnail`, `XamEnumerate`,
`XamGetExecutionId`, `XamGetSystemVersion`, `XamInputGetState`,
`XamInputSetState`, `XamLoaderLaunchTitle`, `XamLoaderTerminateTitle`,
`XamNotifyCreateListener`, `XamShowDeviceSelectorUI`,
`XamShowDirtyDiscErrorUI`, `XamShowMessageBoxUI`, `XamShowMessageBoxUIEx`,
`XamShowSigninUI`, `XamTaskCloseHandle`, `XamTaskSchedule`,
`XamTaskShouldExit`, `XamUserGetSigninState`, `XamUserGetXUID`,
`XamUserReadProfileSettings`, `XamUserWriteProfileSettings`

### Rtl / CRT (22)
`RtlCaptureContext`, `RtlCompareMemoryUlong`, `RtlCompareStringN`,
`RtlEnterCriticalSection`, `RtlFillMemoryUlong`, `RtlFreeAnsiString`,
`RtlImageXexHeaderField`, `RtlInitAnsiString`, `RtlInitUnicodeString`,
`RtlInitializeCriticalSection`, `RtlInitializeCriticalSectionAndSpinCount`,
`RtlLeaveCriticalSection`, `RtlMultiByteToUnicodeN`, `RtlNtStatusToDosError`,
`RtlRaiseException`, `RtlTimeFieldsToTime`, `RtlTimeToTimeFields`,
`RtlTryEnterCriticalSection`, `RtlUnicodeStringToAnsiString`,
`RtlUnicodeToMultiByteN`, `RtlUnwind`, `sprintf`

### Graphics / Vd (20)
`VdCallGraphicsNotificationRoutines`, `VdEnableDisableClockGating`,
`VdEnableRingBufferRPtrWriteBack`, `VdGetCurrentDisplayGamma`,
`VdGetCurrentDisplayInformation`, `VdGetSystemCommandBuffer`,
`VdInitializeEngines`, `VdInitializeRingBuffer`,
`VdInitializeScalerCommandBuffer`, `VdIsHSIOTrainingSucceeded`,
`VdPersistDisplay`, `VdQueryVideoFlags`, `VdQueryVideoMode`,
`VdRetrainEDRAM`, `VdRetrainEDRAMWorker`, `VdSetDisplayMode`,
`VdSetGraphicsInterruptCallback`, `VdSetSystemCommandBufferGpuIdentifierAddress`,
`VdShutdownEngines`, `VdSwap`

### Network (16) — stub only
`NetDll_WSACleanup`, `NetDll_WSAStartup`, `NetDll_XNetGetTitleXnAddr`,
`NetDll_XNetStartup`, `NetDll___WSAFDIsSet`, `NetDll_accept`, `NetDll_bind`,
`NetDll_closesocket`, `NetDll_connect`, `NetDll_inet_addr`, `NetDll_listen`,
`NetDll_recv`, `NetDll_select`, `NetDll_send`, `NetDll_setsockopt`,
`NetDll_socket`

### Others
Ex (6): `ExAllocatePool`, `ExCreateThread`, `ExFreePool`,
`ExGetXConfigSetting`, `ExRegisterTitleTerminateNotification`,
`ExTerminateThread`.
Mm (6): `MmAllocatePhysicalMemoryEx`, `MmFreePhysicalMemory`,
`MmGetPhysicalAddress`, `MmQueryAllocationSize`, `MmQueryStatistics`,
`MmSetAddressProtect`.
Ob (4): `ObCreateSymbolicLink`, `ObDeleteSymbolicLink`,
`ObDereferenceObject`, `ObReferenceObjectByHandle`.
Debug (2): `DbgBreakPoint`, `DbgPrint`. Io (2): `IoDismountVolume`,
`IoDismountVolumeByFileHandle`. Hal (1): `HalReturnToFirmware`.

### ReXGlue runtime symbols (not kernel imports)
`ZN3rex3ppc10MMIOLoad32Ej`, `ZN3rex3ppc11MMIOStore32Ejj`,
`ZN3rex3ppc15ResolveFunctionEj`, `ZN3rex3ppc4TrapER10PPCContextPht`,
`ZN3rex3ppc6detail10mmio_slotsE`, `ZN3rex5Clock19QueryGuestTickCountEv`,
`ZN3rex8platform16FindImageSectionE...`, `__C_specific_handler`,
`___chkstk_ms`, `__ImageBase`, `__gxx_personality_seh0`, `__cxa_begin_catch`,
`assert`, `_snprintf`, `_vsnprintf`, `vswprintf`. These must be provided by
our Android runtime or by a port of ReXGlue's runtime layer. The SEH symbols
(`__C_specific_handler`, `__gxx_personality_seh0`) are Windows-specific and
will need a clean-room replacement for ARM64 Linux/Android.

## 3. Implementation priority

Priority is driven by "needed to reach title screen", not by family size:

1. **Boot/core:** Ex pool alloc/free, Mm memory, `KeQuery*`, `Rtl*` string/CRT,
   `NtCreateFile`/`NtOpenFile`/`NtReadFile`, `XexGet*`, `__imp__*` resolution.
2. **Threads/sync:** Ke events/semaphores/spinlocks/TLS, Nt sync, ExCreateThread.
3. **Rendering:** `VdSetGraphicsInterruptCallback`, `VdInitializeEngines`,
   `VdGetSystemCommandBuffer`, `VdSwap` → `gpu::present()`.
4. **Input:** `XamInputGetState` / `XamInputSetState` from touch overlay.
5. **Audio:** `XAudio*` + XMA2 (hardest; budget a dedicated milestone).
6. **Stubs:** all NetDll_*, most Xam* UI/content, crypto key APIs → safe defaults.

## 3b. Runtime link symbols (separate from kernel imports)

`tools/extract_runtime_symbols.py` produced `config/runtime_symbols.json`:
**245** non-kernel link symbols after subtracting the 216 imports, the weak
`sub_*` aliases and `xstart` (whose real body is generated). Breakdown:
118 `__rest*`, 118 `__save*` ABI helpers, 6 mangled `rex::` runtime functions,
and 3 compiler/entry symbols (`__aarch64_cas4_acq_rel`,
`__aarch64_cas8_acq_rel`, `__main`).
**Status update:** `tools/gen_abi_helpers.py` now implements **236** of them
with real, measured semantics (`native/src/abi_helpers.cpp`, see
`27-ABI-HELPERS.md`). `native/src/runtime_stubs.cpp` contains only the remaining
**9** non-ABI placeholders (MMIO, ResolveFunction, Trap, clock, compiler atomics).

## 4. Current coverage

**Status update:** all 216 names now have registered handlers. 32 have real
implementations (see `28-HLE-IMPORTS.md`); the rest use a safe
`r3 = STATUS_SUCCESS` default. Names must still match this measured list
exactly — the symbol linker is case-sensitive and there is no ordinal
indirection at this layer. Note the generated code wants `ExAllocatePool`
(exactly), not `ExAllocatePoolWithTag`.
