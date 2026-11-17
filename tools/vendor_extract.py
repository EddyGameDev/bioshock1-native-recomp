#!/usr/bin/env python3
"""Vendor the downloaded Xbox 360 recompilation toolchains into third_party/.

The source archives live next to this project (in the parent folder). Each
archive normally contains a single top-level "<repo>-main" / "<repo>-master"
directory. This script strips that wrapper so every tool lands in a clean,
predictable path under third_party/.

Usage:
    python tools/vendor_extract.py

Re-running is safe: existing third_party directories are left untouched unless
--force is passed.
"""

from __future__ import annotations

import argparse
import os
import shutil
import sys
import tempfile
import zipfile
from pathlib import Path

PROJECT = Path(__file__).resolve().parents[1]
SOURCES = PROJECT.parent
DEST = PROJECT / "third_party"

# archive name -> destination directory name
ARCHIVES: dict[str, str] = {
    "360tools-main.zip": "360tools",
    "recompiler-master.zip": "rexdex-recompiler",
    "rexglue-knowledges-main.zip": "rexglue-knowledges",
    "rexglue-sdk-main.zip": "rexglue-sdk",
    "x360port-main.zip": "x360port",
    "Xbox360RecompAndroid-main.zip": "Xbox360RecompAndroid",
    "xenon-jumptables-main.zip": "xenon-jumptables",
    "Xenon-Recomp-main.zip": "Xenon-Recomp-legacy",
    "XenonRecomp-main.zip": "XenonRecomp",
    "XenosRecomp-main.zip": "XenosRecomp",
    "Xerenge-main.zip": "Xerenge",
    "gears-of-war-2-pc-port-master.zip": "gears-of-war-2-pc-port",
}


def strip_wrapper(tmp: Path) -> Path:
    entries = [p for p in tmp.iterdir() if not p.name.startswith(".")]
    if len(entries) == 1 and entries[0].is_dir():
        return entries[0]
    return tmp


def extract_one(archive: Path, destination: Path, force: bool) -> None:
    if destination.exists():
        if not force:
            print(f"skip   {destination.name} (already present)")
            return
        shutil.rmtree(destination)

    print(f"vendor {archive.name} -> third_party/{destination.name}", flush=True)
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        with zipfile.ZipFile(archive) as zf:
            zf.extractall(tmp)
        shutil.copytree(strip_wrapper(tmp), destination)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--force", action="store_true", help="replace existing directories")
    args = parser.parse_args()

    DEST.mkdir(parents=True, exist_ok=True)
    missing = [name for name in ARCHIVES if not (SOURCES / name).is_file()]
    if missing:
        print("Missing source archives (expected next to the project):", file=sys.stderr)
        for name in missing:
            print("  " + name, file=sys.stderr)
        return 1

    for archive_name, dest_name in ARCHIVES.items():
        extract_one(SOURCES / archive_name, DEST / dest_name, args.force)

    print(f"\nVendored {len(ARCHIVES)} toolchains into {DEST}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
