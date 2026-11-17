"""Convert a DXBC blob into a C++ byte array header like ReXGlue's shader headers."""
import sys
from pathlib import Path

src = Path(sys.argv[1])
out = Path(sys.argv[2])
name = sys.argv[3]
data = src.read_bytes()
lines = []
lines.append('/**')
lines.append(' * Auto-generated from renderer/shaders/' + src.name)
lines.append(' * Compiled with DXC (Windows SDK) -T ps_5_1.')
lines.append(' */')
lines.append('')
lines.append(f'const BYTE {name}[] = {{')
for i in range(0, len(data), 16):
    chunk = data[i:i+16]
    lines.append('    ' + ', '.join(str(b) for b in chunk) + ',')
lines.append('};')
lines.append('')
out.write_text('\n'.join(lines))
print(f'wrote {out} ({len(data)} bytes)')
