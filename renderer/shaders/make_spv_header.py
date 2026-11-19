"""Convert a SPIR-V binary into a C++ u32 array header matching ReXGlue's vulkan_spirv headers."""
import struct
import sys
from pathlib import Path

src = Path(sys.argv[1])
out = Path(sys.argv[2])
name = sys.argv[3]
data = src.read_bytes()
assert len(data) % 4 == 0, f"SPIR-V size {len(data)} is not a multiple of 4"
words = struct.unpack(f"<{len(data) // 4}I", data)

lines = [
    "/**",
    f" * Auto-generated from renderer/shaders/{src.name}",
    " * Compiled with NDK glslc -O --target-env=vulkan1.0.",
    " */",
    "",
    "#include <rex/types.h>",
    "",
    f"const u32 {name}[] = {{",
]
for i in range(0, len(words), 8):
    chunk = words[i : i + 8]
    lines.append("    " + ", ".join(f"0x{w:08X}" for w in chunk) + ",")
lines.append("};")
lines.append("")
out.write_text("\n".join(lines), encoding="utf-8")
print(f"wrote {out} ({len(words)} words / {len(data)} bytes)")
