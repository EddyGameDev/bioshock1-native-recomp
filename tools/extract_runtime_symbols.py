#!/usr/bin/env python3
"""Extract the link-time runtime symbols still needed by generated code.

Runs llvm-nm over the arm64 generated objects, subtracts the 216 kernel
imports (already covered by import_stubs.cpp) and the weak sub_* aliases
(resolved inside the generated set), and writes the remainder to
config/runtime_symbols.json. That file drives gen_runtime_stubs.py.

Usage:
    python tools/extract_runtime_symbols.py
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
from pathlib import Path

PROJECT = Path(__file__).resolve().parents[1]
OBJECTS = PROJECT / "build/codegen-validate-android"
NM = PROJECT / "tools-bin/android-ndk-r28c/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-nm.exe"
IMPORTS = PROJECT / "config/hle_imports.json"
OUTPUT = PROJECT / "config/runtime_symbols.json"

UNDEF = re.compile(r"^\s*U\s+(.+)$")

# xstart is the guest entry point; ReXGlue emits its real recompiled body in
# bioshock1_recomp.84.cpp as __imp__xstart. Do not stub it.
EXCLUDE = {"xstart"}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--objects", type=Path, default=OBJECTS)
    ap.add_argument("--nm", type=Path, default=NM)
    ap.add_argument("--output", type=Path, default=OUTPUT)
    args = ap.parse_args()

    objects = sorted(p for p in args.objects.rglob("*") if p.suffix == ".o")
    if not objects:
        raise SystemExit(f"no .o files under {args.objects}; build codegen-validate-android first")

    response = args.output.parent / ".nm_objects.rsp"
    response.write_text("\n".join(f'"{p}"' for p in objects), encoding="utf-8")
    result = subprocess.run([str(args.nm), "-u", f"@{response}"],
                            capture_output=True, text=True, check=True)
    symbols = sorted({m.group(1).strip()
                      for line in result.stdout.splitlines()
                      if (m := UNDEF.match(line))})

    imports = set(json.loads(IMPORTS.read_text(encoding="utf-8")))
    covered = {"__imp__" + name for name in imports}
    remaining = [s for s in symbols
                 if s not in covered and not s.startswith("sub_") and s not in EXCLUDE]

    args.output.write_text(json.dumps(remaining, indent=1), encoding="utf-8")
    print(f"objects: {len(objects)}  undefined: {len(symbols)}  runtime symbols: {len(remaining)}")
    print(f"wrote {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
