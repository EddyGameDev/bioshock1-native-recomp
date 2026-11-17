#!/usr/bin/env python3
"""Copy validated ReXGlue codegen output into recomp/generated/.

The authoritative output lives in _work/rexglue-bioshock1/generated/default
(gitignored). This script stages a validated snapshot into the project so the
Android CMake target can consume a stable path. Generated files are never
hand-edited — re-run codegen and re-stage instead.

Usage:
    python tools/stage_generated.py [--force]
"""

from __future__ import annotations

import argparse
import json
import shutil
from pathlib import Path

PROJECT = Path(__file__).resolve().parents[1]
SOURCE = PROJECT / "_work/rexglue-bioshock1/generated/default"
DEST = PROJECT / "recomp/generated"
PATTERNS = ("bioshock1_*.cpp", "bioshock1_*.h", "codegen.*.json", "sources.cmake")
REQUIRED = ("bioshock1_init.cpp", "bioshock1_register.cpp", "bioshock1_pch.h",
            "bioshock1_funcs.h", "bioshock1_init.h")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--force", action="store_true")
    args = ap.parse_args()

    if not SOURCE.is_dir():
        raise SystemExit(f"No codegen output at {SOURCE}; run rexglue codegen first.")

    missing = [name for name in REQUIRED if not (SOURCE / name).is_file()]
    if missing:
        raise SystemExit(f"Refusing to stage incomplete output; missing: {missing}")

    if DEST.exists() and args.force:
        shutil.rmtree(DEST)
    DEST.mkdir(parents=True, exist_ok=True)

    copied = []
    for pattern in PATTERNS:
        for src in sorted(SOURCE.glob(pattern)):
            dst = DEST / src.name
            shutil.copy2(src, dst)
            copied.append(src.name)

    manifest = {
        "source": str(SOURCE.relative_to(PROJECT)),
        "file_count": len(copied),
        "total_bytes": sum((DEST / name).stat().st_size for name in copied),
        "files": copied,
    }
    (DEST / "staged_manifest.json").write_text(
        json.dumps(manifest, indent=2), encoding="utf-8")

    print(f"staged {len(copied)} files ({manifest['total_bytes'] / 1e6:.1f} MB) -> {DEST}")
    print(f"manifest: {DEST / 'staged_manifest.json'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
