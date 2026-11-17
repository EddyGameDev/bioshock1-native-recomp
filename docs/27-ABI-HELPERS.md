# XDK register save/restore ABI helpers — implemented

The 236 no-op `__rest*`/`__save*` stubs are now **real implementations**, derived
from the actual XDK helper code inside the decrypted game image rather than
guessed.

## 1. How the semantics were established

1. Generated code calls helpers as PPC functions: `__savegprlr_14(ctx, base)`.
2. ReXGlue's `src/codegen/phase_register.cpp` gives the authoritative naming:
   * `__{save,rest}gprlr_N`, `N = 14..31`, address stride 4
   * `__{save,rest}fpr_N`, `N = 14..31`, stride 4
   * `__{save,rest}vmx_N`, `N = 14..31` and `N = 64..127`, stride 8
3. Disassembling the real helpers in `_work/default_unpacked` (via Capstone and
   raw word dumps for VMX128 instructions) gave exact memory layouts.

### Measured layouts

| Helper family | Base register | Guest-memory offsets | Width |
|---|---|---|---|
| `save/restoregprlr_N` | `r1` | `-0x98 + 8*(i-14)` for `i=N..31`; LR via `r12` at `r1-8` | 64-bit (LR 32-bit) |
| `save/restfpr_N` | `r12` | `-0x90 + 8*(i-14)` for `i=N..31` | 64-bit |
| `save/restorevmx_N` (14..31) | `r12` | `-0x120 + 16*(i-14)` | 128-bit |
| `save/restorevmx_N` (64..127) | `r12` | `-0x400 + 16*(i-64)` | 128-bit |

Example from the real image:

```
82621970: std r14,-0x98(r1)   ... 826219B4: std r31,-0x10(r1)
826219B8: stw r12,-8(r1)      826219BC: blr
826219C0: ld  r14,-0x98(r1)   ... 82621A04: ld  r31,-0x10(r1)
82621A08: lwz r12,-8(r1)      82621A0C: mtlr r12   82621A10: blr
```

Every `__*_N` entry is a slice of this contiguous table starting at register N.

## 2. Implementation

| File | Role |
|---|---|
| `tools/gen_ppc_accessors.py` | Generates `native/include/ppc_accessors.h`: X-macro FPR (`f0..f31`) and VMX (`v0..v127`) accessors that work with both our standalone context and ReXGlue's real `PPCContext` |
| `tools/gen_abi_helpers.py` | Generates `native/src/abi_helpers.cpp` from `config/runtime_symbols.json` and the measured layouts |
| `native/src/abi_helpers.cpp` | 236 generated `extern "C"` helper functions with correct `(PPCContext&, base)` ABI; big-endian 32/64-bit and VMX stores/loads |
| `config/runtime_non_abi.json` | The 9 remaining symbols that are *not* ABI helpers |
| `native/src/runtime_stubs.cpp` | Now only those 9 placeholders |

Guest memory is addressed as `base + guest_address`, values stored big-endian with
`__builtin_bswap{32,64}`; VMX registers are stored as four big-endian 32-bit words.

## 3. Verification

* Host self-test now round-trips `r14`/LR through `__savegprlr_14` /
  `__restgprlr_14` using a 64 KB fake guest-memory buffer, and a full 16-byte
  VMX register through `__savevmx_64` / `__restvmx_64`. **Passes.**
* Fixed a bug found by this test: the standalone `PPCRegister` used 32 bits,
  truncating 64-bit guest values. It is now a 64-bit field, matching ReXGlue's
  `Register::u64`.
* The arm64 `.so` relinked with `BS_USE_REX_CONTEXT=1` and real helpers:
  `libbs1_link_test.so` **139.5 MB**, `-Wl,-z,defs`, zero undefined symbols.
* APK rebuilds successfully.

## 4. Remaining link placeholders (9)

```
_MMIOLoad32, _MMIOStore32, _ResolveFunction, _Trap, mmio_slots,
Clock::QueryGuestTickCount, __aarch64_cas4/8_acq_rel, __main
```

These are compiler-runtime/SDK-runtime functions, not ABI helpers. They are
still no-ops; MMIO access and trap handling will need real implementations when
the guest reaches those paths.

## 5. What this changes for boot

Previously a guest call to `__restgprlr_14` would silently do nothing, so every
restored non-volatile register and the link register would be wrong — the guest
would almost certainly crash immediately. That class of failure is removed.
The next boot blockers are the 216 kernel-import handlers and the remaining 9
runtime symbols.
