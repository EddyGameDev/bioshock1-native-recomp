#!/usr/bin/env python3
"""Minimal XDVDFS reader for Xbox 360 disc images.

Only the parts needed by the port are implemented: locate the game partition,
walk the B-tree directory, and stream a single file out (default.xex and,
optionally, the whole game tree). No decryption is performed - the redump
style ISO this project consumes already has the game partition in the clear.

Usage:
    python tools/xdvdfs.py list <game.iso>
    python tools/xdvdfs.py find <game.iso> default.xex
    python tools/xdvdfs.py extract <game.iso> default.xex _game/default.xex
"""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

MAGIC = b"MICROSOFT*XBOX*MEDIA"
SECTOR = 2048
KNOWN_PARTITIONS = (0x0FD90000, 0x0FDA0000, 0x02080000, 0x00000000)


class XDVDFS:
    def __init__(self, path: Path):
        self.path = path
        self.f = path.open("rb")
        self.base, self.root_sector, self.root_size = self._locate()

    def close(self) -> None:
        self.f.close()

    def __enter__(self) -> "XDVDFS":
        return self

    def __exit__(self, *args: object) -> None:
        self.close()

    def _locate(self) -> tuple[int, int, int]:
        f = self.f
        for off in KNOWN_PARTITIONS:
            f.seek(off)
            if f.read(20) == MAGIC:
                base = off - 0x10000 if off >= 0x10000 else off
                f.seek(off + 20)
                root, size = struct.unpack("<II", f.read(8))
                # Accept only if the adjusted base yields plausible data.
                if base >= 0 and root and size and base + root * SECTOR + size <= f.seek(0, 2):
                    return base, root, size
                return off, root, size

        # Brute-force search in the first 512 MB.
        f.seek(0)
        pos = 0
        chunk = 1024 * 1024
        while pos < 0x20000000:
            f.seek(pos)
            buf = f.read(chunk)
            idx = buf.find(MAGIC)
            if idx >= 0:
                magic_off = pos + idx
                base = magic_off - 0x10000 if magic_off >= 0x10000 else magic_off
                f.seek(magic_off + 20)
                root, size = struct.unpack("<II", f.read(8))
                return base, root, size
            pos += chunk - 20
        raise RuntimeError("XDVDFS partition not found (image may be encrypted)")

    def _parse_dir(self, sector: int, size: int) -> list[dict]:
        self.f.seek(self.base + sector * SECTOR)
        data = self.f.read(size)
        entries: list[dict] = []
        stack = [0]
        seen: set[int] = set()
        while stack:
            off = stack.pop()
            if off in seen or off + 14 > len(data):
                continue
            seen.add(off)
            left, right = struct.unpack_from("<HH", data, off)
            start, fsize = struct.unpack_from("<II", data, off + 4)
            attrs, name_len = data[off + 12], data[off + 13]
            if not name_len or off + 14 + name_len > len(data):
                continue
            raw = data[off + 14:off + 14 + name_len]
            if not all(32 <= b < 127 for b in raw):
                continue
            entries.append({
                "name": raw.decode("ascii"),
                "sector": start,
                "size": fsize,
                "is_dir": bool(attrs & 0x10),
            })
            if left and left * 4 < len(data):
                stack.append(left * 4)
            if right and right * 4 < len(data):
                stack.append(right * 4)
        return entries

    def walk(self, prefix: str = "") -> list[dict]:
        """Return all files as {path, sector, size}."""
        entries = self._parse_dir(self.root_sector, self.root_size)
        out: list[dict] = []
        for e in entries:
            path = f"{prefix}/{e['name']}" if prefix else e["name"]
            if e["is_dir"]:
                # Directly parse subdirectories using their recorded size.
                sub = self._parse_dir(e["sector"], e["size"])
                stack = [(path, sub)]
                while stack:
                    cur_prefix, cur = stack.pop()
                    for se in cur:
                        sp = f"{cur_prefix}/{se['name']}"
                        if se["is_dir"]:
                            stack.append((sp, self._parse_dir(se["sector"], se["size"])))
                        else:
                            out.append({"path": sp, "sector": se["sector"], "size": se["size"]})
            else:
                out.append({"path": path, "sector": e["sector"], "size": e["size"]})
        return out

    def extract(self, entry: dict, dest: Path) -> None:
        dest.parent.mkdir(parents=True, exist_ok=True)
        self.f.seek(self.base + entry["sector"] * SECTOR)
        remaining = entry["size"]
        with dest.open("wb") as out:
            while remaining > 0:
                chunk = self.f.read(min(remaining, 4 * 1024 * 1024))
                if not chunk:
                    raise RuntimeError(f"short read for {entry['path']}")
                out.write(chunk)
                remaining -= len(chunk)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("command", choices=("list", "find", "extract"))
    ap.add_argument("iso", type=Path)
    ap.add_argument("name", nargs="?")
    ap.add_argument("dest", nargs="?", type=Path)
    args = ap.parse_args()

    with XDVDFS(args.iso) as disc:
        print(f"XDVDFS base=0x{disc.base:08X} root=sector {disc.root_sector}")
        files = disc.walk()
        if args.command == "list":
            for f in files:
                print(f"  {f['size']:>12,}  {f['path']}")
            print(f"Total: {len(files)} files")
        else:
            matches = [f for f in files
                       if f["path"].lower().endswith("/" + args.name.lower())
                       or f["path"].lower() == args.name.lower()]
            if not matches:
                print(f"not found: {args.name}", file=sys.stderr)
                return 1
            for f in matches:
                print(f"found {f['path']} ({f['size']:,} bytes, sector {f['sector']})")
                if args.command == "extract":
                    dest = args.dest or Path(f["path"])
                    disc.extract(f, dest)
                    print(f"wrote {dest} ({dest.stat().st_size:,} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
