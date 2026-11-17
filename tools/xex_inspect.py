#!/usr/bin/env python3
"""Inspect an Xbox 360 XEX2 image and emit structured data for the port.

This is intentionally dependency-free. It reads only the cleartext XEX2
headers (no decryption), which is enough to plan a static recompilation:
image base, entry point, title ID, XDK build, static libs and kernel/XAM
imports.

Usage:
    python tools/xex_inspect.py <default.xex> [-o config/xex_analysis.json]

XEX2 is big-endian on the outside. All multi-byte fields below are '>I'
unless noted otherwise. Header IDs match Xenia's xex2.h.
"""

from __future__ import annotations

import argparse
import json
import struct
import sys
from pathlib import Path

MAGIC = b"XEX2"

# Optional-header keys (Xenia xex2.h)
H_EXEC_INFO = 0x000002FF
H_SYSTEM_FLAGS = 0x000003FF
H_BOUNDING_PATH = 0x00010100
H_IMAGE_BASE = 0x00010201
H_IMPORTS = 0x000103FF
H_LAN_KEY = 0x00018002
H_ORIGINAL_BASE = 0x000183FF
H_STATIC_LIBS = 0x000200FF
H_DELTA_PATCH = 0x00020104
H_DEFAULT_HEAP = 0x00020200
H_ENTRY_POINT = 0x00030000
H_TLS_INFO = 0x00040006
H_DEFAULT_STACK = 0x00040310
H_FS_CACHE = 0x00040404
H_BASE_REFERENCE = 0x000405FF

HEADER_NAMES = {
    H_EXEC_INFO: "EXECUTION_INFO",
    H_SYSTEM_FLAGS: "SYSTEM_FLAGS",
    H_BOUNDING_PATH: "BOUNDING_PATH",
    H_IMAGE_BASE: "IMAGE_BASE_ADDRESS",
    H_IMPORTS: "IMPORT_LIBRARIES",
    H_LAN_KEY: "LAN_KEY",
    H_ORIGINAL_BASE: "ORIGINAL_BASE_ADDRESS",
    H_STATIC_LIBS: "STATIC_LIBRARIES",
    H_DELTA_PATCH: "DELTA_PATCH_DESCRIPTOR",
    H_DEFAULT_HEAP: "DEFAULT_HEAP_SIZE",
    H_ENTRY_POINT: "ENTRY_POINT",
    H_TLS_INFO: "TLS_INFO",
    H_DEFAULT_STACK: "DEFAULT_STACK_SIZE",
    H_FS_CACHE: "DEFAULT_FILESYSTEM_CACHE_SIZE",
    H_BASE_REFERENCE: "BASE_REFERENCE",
}

# Ordinal -> name lookup is intentionally NOT attempted here. XEX import
# records point at descriptor structures inside the compressed PE image, so a
# meaningful ordinal map can only be produced by the recompiler (ReXGlue) after
# it decrypts and decompresses the image. Reporting a guessed ordinal would be
# misinformation; we report the raw thunk/descriptor pairs instead.


def u32(data: bytes, off: int) -> int:
    return struct.unpack_from(">I", data, off)[0]


def u16(data: bytes, off: int) -> int:
    return struct.unpack_from(">H", data, off)[0]


def _ascii_hex_field(value: int) -> str | None:
    """XEX execution-info 'version' fields store four ASCII hex digits."""
    raw = value.to_bytes(4, "big")
    try:
        text = raw.decode("ascii")
    except UnicodeDecodeError:
        return None
    if all(c in "0123456789ABCDEFabcdef" for c in text):
        return text
    return None


def parse_exec_info(data: bytes, off: int) -> dict:
    if off + 32 > len(data):
        return {}
    version_raw = u32(data, off + 4)
    base_raw = u32(data, off + 8)
    version_text = _ascii_hex_field(version_raw)
    base_text = _ascii_hex_field(base_raw)
    # Retail XEX2 stores the two 16-bit halves of the title ID as ASCII strings
    # in the 'version'/'base_version' fields (e.g. "5454" + "07D8").
    title_id = (f"0x{int(version_text, 16):04X}{int(base_text, 16):04X}"
                if version_text and base_text
                else f"0x{u32(data, off + 12):08X}")
    return {
        "media_id": f"0x{u32(data, off):08X}",
        "version": version_text or f"0x{version_raw:08X}",
        "version_raw": f"0x{version_raw:08X}",
        "base_version": base_text or f"0x{base_raw:08X}",
        "base_version_raw": f"0x{base_raw:08X}",
        "title_id": title_id,
        "title_id_raw_field": f"0x{u32(data, off + 12):08X}",
        "platform_id": data[off + 16],
        "executable_type": data[off + 17],
        "disc_media_type": data[off + 18],
    }


def parse_static_libs(data: bytes, off: int) -> tuple[list[dict], list[int]]:
    """Return (records, xdk_builds). Records are 8-byte name + version words."""
    if off + 4 > len(data):
        return [], []
    size = u32(data, off)
    libs, builds = [], []
    pos = off + 4
    end = min(off + size, len(data))
    while pos + 16 <= end:
        raw = data[pos:pos + 8]
        name = "".join(chr(b) for b in raw if 32 <= b < 127).rstrip("\x00")
        major, minor, build = u16(data, pos + 8), u16(data, pos + 10), u16(data, pos + 12)
        approval, qfe = data[pos + 14], data[pos + 15]
        if name:
            libs.append({
                "name": name,
                "version": f"{major}.{minor}.{build}.{qfe}",
                "build": build,
                "approval": approval,
            })
            if build:
                builds.append(build)
        pos += 16
    return libs, sorted(set(builds))


def parse_imports(data: bytes, off: int) -> list[dict]:
    if off + 12 > len(data):
        return []
    total_size = u32(data, off)
    str_size = u32(data, off + 4)
    lib_count = u32(data, off + 8)
    str_start = off + 12
    strings = data[str_start:str_start + str_size]
    names = []
    p = 0
    while p < len(strings):
        e = strings.find(b"\x00", p)
        if e < 0:
            break
        s = strings[p:e].decode("ascii", errors="replace")
        if s:
            names.append(s)
        p = e + 1
        while p < len(strings) and p % 4:
            p += 1

    result = []
    lib_off = str_start + str_size
    for _ in range(lib_count):
        if lib_off + 40 > len(data):
            break
        lib_size = u32(data, lib_off)
        version = u32(data, lib_off + 28)
        name_index = u16(data, lib_off + 36)
        num_records = u16(data, lib_off + 38)
        name = names[name_index] if name_index < len(names) else f"lib_{name_index}"
        # Each 8-byte record is a (thunk_address, descriptor_address) pair.
        # The ordinal itself lives in the descriptor inside the compressed PE,
        # so only the recompiler can resolve it after decompression.
        records = []
        rec_off = lib_off + 40
        for r in range(num_records):
            if rec_off + 8 > len(data):
                break
            first, second = u32(data, rec_off), u32(data, rec_off + 4)
            records.append({
                "first": f"0x{first:08X}",
                "second": f"0x{second:08X}",
            })
            rec_off += 8
        result.append({
            "library": name,
            "version": f"{(version >> 24) & 0xFF}.{(version >> 16) & 0xFF}.{version & 0xFFFF}",
            "records": num_records,
            "import_records": records,
        })
        lib_off += lib_size
    return result


def inspect(path: Path) -> dict:
    data = path.read_bytes()
    if data[:4] != MAGIC:
        raise ValueError(f"Not an XEX2 image (magic={data[:4]!r})")

    module_flags = u32(data, 0x04)
    pe_offset = u32(data, 0x08)
    security_offset = u32(data, 0x10)
    header_count = u32(data, 0x14)

    out: dict = {
        "file": str(path),
        "file_size": len(data),
        "module_flags": f"0x{module_flags:08X}",
        "pe_offset": f"0x{pe_offset:X}",
        "security_offset": f"0x{security_offset:X}",
        "optional_headers": [],
    }

    if security_offset and security_offset + 8 <= len(data):
        out["security"] = {
            "header_size": f"0x{u32(data, security_offset):X}",
            "image_size": f"0x{u32(data, security_offset + 4):X}",
        }

    pos = 0x18
    header_values: dict[int, int] = {}
    for _ in range(header_count):
        if pos + 8 > len(data):
            break
        key, value = u32(data, pos), u32(data, pos + 4)
        header_values[key] = value
        out["optional_headers"].append({
            "id": f"0x{key:08X}",
            "name": HEADER_NAMES.get(key, "UNKNOWN"),
            "value": f"0x{value:08X}",
        })
        pos += 8

    if H_EXEC_INFO in header_values:
        out["execution_info"] = parse_exec_info(data, header_values[H_EXEC_INFO])
    if H_IMAGE_BASE in header_values:
        out["image_base"] = f"0x{header_values[H_IMAGE_BASE]:08X}"
    if H_ENTRY_POINT in header_values:
        out["entry_point"] = f"0x{header_values[H_ENTRY_POINT]:08X}"
    if H_ORIGINAL_BASE in header_values:
        p = header_values[H_ORIGINAL_BASE]
        if p + 4 <= len(data):
            blob_size = min(u32(data, p), 256)
            blob = data[p + 4:p + 4 + blob_size]
            text = "".join(chr(b) if 32 <= b < 127 else "\x00" for b in blob).split("\x00")[0]
            out["original_image_name"] = text

    # Some tools disagree on the static-library header ID; sniff both.
    for key in (H_STATIC_LIBS, H_BASE_REFERENCE):
        if key in header_values:
            libs, builds = parse_static_libs(data, header_values[key])
            if libs:
                out["static_libraries"] = libs
                out["xdk_builds"] = builds
                out["static_libraries_header_id"] = f"0x{key:08X}"
                break

    if H_IMPORTS in header_values:
        out["import_libraries"] = parse_imports(data, header_values[H_IMPORTS])

    # Handy summary for the config generator / docs.
    exec_info = out.get("execution_info", {})
    out["summary"] = {
        "title_id": exec_info.get("title_id"),
        "version": exec_info.get("version"),
        "xdk_builds": out.get("xdk_builds", []),
        "base_address": out.get("image_base"),
        "entry_point": out.get("entry_point"),
        "import_library_count": len(out.get("import_libraries", [])),
        "total_import_records": sum(
            l["records"] for l in out.get("import_libraries", [])
        ),
    }
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("xex", type=Path)
    ap.add_argument("-o", "--output", type=Path, default=None)
    ap.add_argument("--json-only", action="store_true")
    args = ap.parse_args()

    result = inspect(args.xex)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(result, indent=2), encoding="utf-8")
    if not args.json_only:
        s = result["summary"]
        print(f"Title ID      : {s['title_id']}")
        print(f"Version       : {s['version']}")
        print(f"XDK builds    : {s['xdk_builds']}")
        print(f"Base address  : {s['base_address']}")
        print(f"Entry point   : {s['entry_point']}")
        print(f"Import libs   : {s['import_library_count']}")
        print(f"Import records: {s['total_import_records']} (ordinals need decompression)")
        for lib in result.get("import_libraries", []):
            print(f"  - {lib['library']} v{lib['version']} ({lib['records']} records)")
        print(f"Static libs   : {len(result.get('static_libraries', []))}")
        if args.output:
            print(f"Full JSON     : {args.output}")
    else:
        print(json.dumps(result, indent=2))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (ValueError, OSError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        raise SystemExit(1)
