# Verification report

`scripts/verify-all.py` is the single source of truth for "is the project
internally consistent right now". It runs only checks that work without a
physical device, is safe to re-run, and uses incremental builds.

## Run it

```powershell
python scripts/verify-all.py
```

## Checks (last run: 2026-11-17 — 12/12 PASS)

| # | Check | What it proves |
|---|---|---|
| 1 | Required files present | ISO, XEX, analysis/config JSON, native + launcher sources all exist |
| 2 | Docs folder detailed | at least 31 markdown documents |
| 3 | XEX analysis reproducible | re-parsing the XEX still yields title `0x545407D8`, XDK `[5426]`, base `0x82000000`, 427 import records |
| 4 | Import matrix complete | `config/hle_imports.json` has exactly 216 names |
| 5 | Runtime symbol list complete | `config/runtime_symbols.json` has exactly 245 symbols |
| 6 | `xstart` not stubbed | generated CRT body is never overridden by our no-op stubs |
| 7 | Generated snapshot intact | staged manifest says 308 files; 305 `bioshock1_*` files present |
| 8 | Host native selftest | HLE dispatch, missing-import handling, named stub forwarding, frame counter |
| 9 | Host generated objects | 152 `.obj` files from the codegen validation build |
| 10 | arm64 generated objects | 153+ `.o` files from the NDK r28c validation build |
| 11 | arm64 `.so` linked | `libbs1_link_test.so` ~146 MB with `__imp__xstart` and `__imp__DbgPrint` exported |
| 12 | APK built and game-data-free | 9.6 MB APK contains `libbioshock1-native.so` and no ISO/XEX/FSB/U assets |

## What "rock solid" means here

* Every number above was **measured**, not assumed; the script re-derives the
  XEX facts each run.
* Generated files are never hand-edited. They are produced by `rexglue codegen`
  → `tools/stage_generated.py` → `tools/gen_import_stubs.py` /
  `tools/gen_runtime_stubs.py`.
* The link uses `-Wl,-z,defs`, so a missing runtime symbol fails loudly rather
  than producing a half-linked library.
* The APK packaging guarantee is enforced by inspecting the ZIP contents.

## What is NOT covered

* **On-device behaviour** — no emulator/device was attached. The M0 acceptance
  test in `17-TESTING.md` still needs to be run on an arm64 Android device.
* **Runtime correctness** — the 245 runtime stubs and 216 import stubs are
  link-time placeholders; they do not yet implement kernel semantics.
* **Boot** — executing `__imp__xstart(ctx, base)` is Phase 2 work still in
  progress; the `.so` linking is a necessary but not sufficient milestone.

## Maintenance rule

Any change to codegen, stubs, CMake or the launcher must be followed by a
green `verify-all.py` run. Record the result in `19-WORKLOG.md`.
