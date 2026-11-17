# Phase 3 — Xenos → GLES3 renderer (started)

Phase 3's goal is first real geometry from the recompiled game. This document
records the first implemented piece: a **PM4 command-stream parser** with
tracing and counters, verified on the host and exposed through the JNI API.

## 1. What is implemented (2026-11-17)

| File | Role |
|---|---|
| `native/include/xenos_pm4.h` | PM4 packet/opcode definitions, `CommandBufferStats`, parse API |
| `native/src/xenos_pm4.cpp` | Clean-room parser for big-endian PM4 type 0/1/2/3 packets; opcode tracing |
| `native/include/gpu_layer.h` | Added `submit_command_buffer()`, `pm4_stats()` |
| `native/src/gpu_layer.cpp` | Accumulates PM4 stats; draws/swaps/interrupts/shader loads counted |
| `native/include/engine.h` | Added `bs_engine_submit_cmds()` flat C API |
| `native/src/engine.cpp` | Implementation; status JSON now includes `pm4Draws` / `pm4Swaps` |
| `native/src/jni_bridge.cpp` | `nativeSubmitCmds(ByteArray)` JNI export |
| `android/.../NativeBridge.kt` | Matching Kotlin declaration |
| `native/src/selftest.cpp` | Synthetic PM4 buffer test |

## 2. Packet handling (from the Xenia-derived SDK disassembler)

Words are read **big-endian** from guest memory. Packet type = top 2 bits.

| Type | Layout | Handling |
|---|---|---|
| 0 | `count = ((hdr>>16)&0x3FFF)+1`, `regbase = hdr&0x7FFF`, optional single-register flag | `count` register writes |
| 1 | two register indices in low bits | 2 register writes |
| 2 | no payload | no-op |
| 3 | `opcode = (hdr>>8)&0x7F`, `count = ((hdr>>16)&0x3FFF)+1` | dispatch by opcode |

Opcodes recognised (values from `rex/graphics/xenos.h`):

```
ME_INIT 0x48   NOP 0x10   DRAW_INDX 0x22   DRAW_INDX_2 0x36
DRAW_INDX_BIN 0x34   DRAW_INDX_2_BIN 0x35
SET_CONSTANT 0x2d   SET_CONSTANT2 0x55
IM_LOAD 0x27   IM_LOAD_IMMEDIATE 0x2b
INTERRUPT 0x54   XE_SWAP 0x64
```

`PM4_XE_SWAP` is the Xenia convention used by ReXGlue's `VdSwap` path — it is
the natural hook for `gpu::present()`.

## 3. Verified behaviour

Host self-test builds a 24-byte big-endian buffer:

```
type0 register write (0x1234 <- 0xDEADBEEF)
type3 PM4_DRAW_INDX  (index count 3)
type3 PM4_XE_SWAP
```

Result:

```
packets=3  register_writes=1  draws=1  swaps=1  malformed=0
status: {"initialized":true,...,"pm4Draws":1,"pm4Swaps":1}
selftest OK
```

The APK also rebuilds successfully with the new JNI entry point.

## 4. What this is NOT yet

* No register state is stored — writes are counted, not applied.
* No GLES3 draw calls are issued; the screen still shows the M0 test pattern.
* No shader translation, texture detiling or EDRAM surface management.
* No real command buffer has been captured from the game.

This is the **M1 tracing milestone** groundwork: before translating anything,
we can walk a buffer and prove the parser is correct.

## 4b. Register file and draw queue (same session)

Added `native/include/xenos_register_file.h`: a 0x4000-word register file with
real Xenos indices from the SDK's `register_table.inc`:

```
VGT_INDX_OFFSET    0x2102
VGT_DMA_SIZE       0x21FB
VGT_DRAW_INITIATOR 0x21FC
```

`xenos::pm4::parse()` now optionally applies type-0/type-1 register writes to a
`RegisterFile` and captures `DrawCommand{index_count, index_offset,
draw_initiator}` on every draw opcode. `gpu_layer` owns the register file and
draw queue and exposes `register_file()` / `draw_commands()`.

The host self-test synthetic buffer now writes `VGT_DMA_SIZE=36` and
`VGT_INDX_OFFSET=8` before the draw and asserts:

```
packets=5  register_writes=3  draws=1  swaps=1  malformed=0
register[0x1234]=0xDEADBEEF  dma_size=36  index_offset=8
draw_commands[0].index_count=36  index_offset=8
selftest OK
```

This is the foundation for translating PM4 draws into `glDrawElements*` once a
real index buffer is available.

## 5. Next steps (in order)

1. Wire `VdSwap` HLE handler to submit the game's real command buffer and call
   `gpu::present()` on `PM4_XE_SWAP`.
2. Add a `RegisterFile` (Xenos register state) so type-0 writes set real state
   (viewport, blend, depth, vertex declarations).
3. Translate `PM4_DRAW_INDX*` to `glDrawElements*` with staged index buffers.
4. Port EDRAM surface allocation/resolve to GLES3 FBOs.
5. Adapt XenosRecomp shader output to GLSL ES 3.00.
6. Texture detiler + BCn→ASTC/RGBA8 upload in `gpu::upload_texture()`.

See `07-GPU-XENOS-GLES3.md` for the full design and `11-ROADMAP.md` for phase
gating. Note that Phase 2 boot work (HLE handlers, ABI helpers, guest context)
remains a prerequisite for seeing real draws — Phase 3 is being built in
parallel so the renderer is ready when boot works.
