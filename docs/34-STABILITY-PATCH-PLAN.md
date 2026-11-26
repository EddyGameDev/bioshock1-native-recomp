# Stability & Visual Patch Map — planning for `v0.0.8`

Status: **plan + implemented control editor (§C3)**. Renderer visual items remain investigation-only; all A/B experiments were reverted.

## 0. Baseline and ground rules

- Local HEAD: `d1fc99e` — `v0.0.7.9-experimental` (initial-lighting/shadow-box fixes + full Bluetooth/USB gamepad support). `android/app/build.gradle` is at `versionCode 15` / `versionName 0.0.7.9-experimental`.
- **There is a sizeable uncommitted renderer patch in the working tree** (`git status` shows 6 modified files). It is treated as the previous agent's in-progress work: do not revert, overwrite, or commit it without the owner's review. It is inventoried in §1.
- GitHub reports being tracked:
  - [Issue #5](https://github.com/EddyGameDev/bioshock1-native-recomp/issues/5): Snapdragon 870 / Realme GT Neo 3T / Android 14 — garbled menu text and black textures on GLES3; blurry/low-FPS Vulkan; intermittent audio. Owner stated Vulkan will be deprecated. Exported logs were only ~1 KB.
  - [Issue #7](https://github.com/EddyGameDev/bioshock1-native-recomp/issues/7): Snapdragon 680 / 4 GB — 25–30 FPS, missing textures and text artifacts; owner requested `EXPORT LOGS`, no reply yet.
  - Owner reports these two do **not** reproduce on their hardware — so they are treated as device-specific until logs arrive.
- The owner confirms `v0.0.7.9` solved the dynamic shadow-projector boxes and the pitch-black initial lighting. Remaining owner-visible items: **blue speckles (lighthouse facade, Mali), FPS lock, general performance**, plus the requested **on-screen control customization button**.
- Docs `00-START-HERE.md`, `10-STATUS.md` and `11-ROADMAP.md` still describe `v0.0.5`/`v0.0.7.6`; `20-KNOWN-ISSUES.md` is dated `2026-11-21` and lists skybox/audio as open even though both were fixed in `v0.0.7`. Part of this patch is a documentation refresh.
- Important current-code facts that contradict older docs: Perf Mode **no longer** steps resolution down (`MainActivity.effectiveRes()` is identity; `_work/rexglue-android-app/src/main.cpp:189-198` explicitly disabled the step-down because 540p→360p caused intermittent level-load failures). Perf Mode now only forces bilinear + anisotropy 1. There is no `setSustainedPerformanceMode` and no GPU-worker `-8` priority in the current source; both were removed deliberately.
- Ground rule from `20-KNOWN-ISSUES.md`: no release until the owner has tested. Build TEST APKs and A/B on device first.

## 1. Inventory of the working-tree patch (previous agent's WIP)

All paths under `renderer/src/gles3/`:

| File | What the uncommitted diff does | Intended symptom |
|---|---|---|
| `xenos_glsl.cpp` | Translator tag `v13-uvscale-highpdepth` → `v14-highp-mull`; `useMediump = false` (full FP32 in all pixel shaders); adds `mulL`/`mulL1` helpers that force `0*x = 0` so `0*Inf`/`Inf*0` cannot produce NaN; moves clamp **before** `texScale/texBias` in `textureAdjust()`; sanitizes `isnan()` fragment outputs to zero. | Fine text/fog/speckle corruption from FP16 overflow, NaN, or wrong clamp order. **Perf risk:** removing mediump loses the v0.0.7.6 Adreno ALU win. |
| `gles3_shaders.cpp` | Gamma LUT fetch uses half-texel center `(255/256, 0.5/256)`; 9-tap Lanczos clamps result to neighbor min/max (anti-ringing); accumulated weights removed. | Banding/off-by-one colors and overshoot halos around high-contrast text. |
| `gles3_resources.cpp` | DXT/BC blocks are **always** 16-bit byte-swapped, matching the working D3D11 path, regardless of declared endian; new `LatestColorTargetAt()`/`LatestDepthTargetAt()`; `TEX-DIM2` one-shot logging for 3D texture fetches. | Garbled text/black textures (#5/#7), wrong 128×128 spotlight-shadow depth resolves, cube/3D fetch diagnostics. |
| `gles3_backend.cpp`, `.h` | New `RB_COPY_DEST_PITCH` handling (`kRegCopyDestPitch`); picks the most recently drawn EDRAM target when the game restores surface info before a small resolve; clamps resolve rect to source pitch/rows/copy-dest; `key.surfaceInfo/tl/br` now use effective values; **disabled consecutive-defer** (`canDeferConsec = false`); direct-copy fast path disabled when a shader clamp is needed; UBO half-swap re-marks all bindings dirty (also in present path); frame-dump hooks triggered by `/sdcard/Android/data/com.bioshock1.recomp/files/bs1_dump.trig` → `bs1_frame.txt`; highp forced for alpha-blended, depth-write-off passes. | Wrong resolve source / smeared or speckled rectangles, UBO binding corruption, water feedback, diagnostics. |

**Step 0 (before any new change):** build a TEST APK from this exact tree, run `scripts/verify-all.py`, and A/B it against `v0.0.7.9` on at least the owner's device (Adreno 710), a Mali device, and ideally the SD870/SD680 reporters. Capture `EXPORT LOGS` plus `PERF(10s)` counters. If the WIP patch fixes the speckles, keep it; if it regresses FPS materially, make the FP32 change selective (see B2) rather than blanket.

## 1b. Live on-device A/B findings (2026-11-26, SM-A366B / Adreno 710)

Evidence: the WIP `.so` was already staged into the installed APK (built at 23:22 after the 23:18–23:20 source edits). The `bs1_dump.trig` → `bs1_frame.txt` hook works: frame 180 = intro Bink (2 draws, YUV `fmt=2`), frame 2610 = gameplay with ~600 draws. A gameplay screenshot shows blue/purple **blocks/decals on multiple surfaces plus tinted/garbled HUD text**, not isolated random pixels.

| Test | Change | Result |
|---|---|---|
| A | Restore HEAD's conditional DXT/BC byte-swap (`endian == 1 \|\| 2`) instead of the WIP unconditional swap | **No change** — blue blocks persisted identically. Byte-swap is cleared as the cause. |
| B | Force `SetShadingRate()` to 1×1 (disable VRS completely) | One capture looked clean, but FPS at 720p fell 66 → 46. **This was a confounded comparison**: each relaunch landed in a different scene/camera, so a clean screenshot is not proof. The owner confirms the blue speckles existed long before VRS was added, so VRS is **not** the root cause. |
| C | Keep VRS but force 1×1 for draws with signed (normal-map) texture fetches | No reliable improvement; later controlled captures still showed speckles. Reverted. |
| D | Keep VRS only for opaque, non-alpha-tested draws | Still showed colored speckles on thin geometry in a later capture. Reverted. |
| E | Force all cubemap fetches to the white fallback | Inconclusive — the game advanced to a different scene before capture. Not kept. |

| F | Force `wantSrgb = false` (no hardware sRGB decode) | Deterministic 360p CONTINUE scene: **no change** — speckles on plants/walls and garbled HUD text remained. Reverted. |
| G | Force all cube fetches to white fallback | Deterministic 360p scene: **no change**. Cubemap mip gap is not the cause. Reverted. |
| H | Force every texture to re-upload every fetch (bypass fingerprint/write-watch) | **No change** in speckles; FPS collapsed to 4. Stale-cache hypothesis eliminated. Reverted. |
| I | Present filter Lanczos → Bilinear (persisted setting) | **No change** in speckles; present-stage ringing eliminated. |

**Deterministic A/B procedure now established:** `am start -n com.bioshock1.recomp/.MainActivity --es res 360p` → tap START GAME (1215,728) → tap on-screen START (1285,1032) four times at 2 s intervals to skip the 2K intro → tap CONTINUE via A (2086,690) → wait ~35 s → screencap. This lands in Medical Pavilion with speckles visible on the plant at right and the HUD text.

**Conclusion:** VRS, DXT byte-swap, sRGB decode, and the WIP colour/present changes are all eliminated. All experiments were reverted and the source is back to the exact WIP patch. Remaining long-standing candidates: (1) cubemaps forced to `mipCount = 1` with no mip generation (`gles3_resources.cpp:1528-1534`, mip loop hardcodes `GL_TEXTURE_2D` at `:1612-1632`), causing specular aliasing on small shiny surfaces; (2) the sampled `Fingerprint()` blind spot (`:49-76`) missing small detail-atlas rewrites; (3) the depth/fog `repRRRR` swizzle path (`gles3_backend.cpp:1326-1341`); (4) implicit-LOD sampling with textures that have no mip chain.

Working tree was restored from `C:\Users\dariu\AppData\Local\Temp\opencode\wip-backup` after every experiment. Screenshots: `bs1_game.png`, `bs1_ab_conditional.png`, `bs1_720p.png`, `bs1_ab_novrs.png`, `bs1_ab_selective.png`, `bs1_ab_control.png`, `bs1_ab_opaque.png`, `bs1_title2_opaque.png`, `bs1_nowhite_cube2.png`.

## 2. Workstream A — visual fidelity

### A1. Blue speckles on the lighthouse facade (Mali, owner-visible, top priority)
Symptom: small isolated blue pixels on the lighthouse facade, reported in `10-STATUS.md` line 5. Only tracked in that one line — no dedicated diagnosis exists yet.

Likely causes, ordered by plausibility:
1. **Compressed-texture decode** on Mali (no hardware S3TC): `DecodeDxtBlock` (`gles3_resources.cpp:361`) / `LinearizeLevel` (`:471`) and `TiledOffset2D` (`:78`). The WIP unconditional 16-bit byteswap (`:538,556`) is directly relevant — validate with a controlled A/B on Mali. Historical `DXT-REAL` logs showed `raw=0000000000000000`, `alpha=00`, `off=0` for cube faces; the skybox was later fixed by the `vbRange == 0` vertex-texture-buffer bug, not by the decoder, so decoder correctness is still unproven. Two known gaps worth checking for the facade texture: cubemaps force `mipCount = 1` (`:1528-1534`), and the mip-upload loop hardcodes `GL_TEXTURE_2D` (`:1612-1632`) instead of a cube-face target.
2. **Stale/wrong resolved surface** being sampled as a texture: `bindStageTextures()` texture-adjust clamp order (now `clamp → scale → bias` in the WIP translator) and the resolved-target write-watch/eviction added in `v0.0.7.9`.
3. **FP16 NaN/Inf** in the previous `v13` mediump shaders: fixed by the WIP FP32 + `mulL`/`mulL1` + `isnan` sanitization. Speckles are a classic NaN symptom.
4. **ESRAM aliasing** between color/depth targets sharing `0x5a0`/`0x5b0`: `PrimaryColorTargetAt`/`PrimaryDepthTargetAt` were fixed in `v0.0.7.9`, but the new `Latest*TargetAt()` WIP logic may need more regression checks.

Steps:
1. Obtain a screenshot plus `EXPORT LOGS` from the reporter/owner with the exact area and resolution. Record GPU, driver, `RES` value, and whether Perf Mode is on.
2. Use the WIP frame dump: create `bs1_dump.trig` in app files, switch one frame, read `bs1_frame.txt` (draws + per-fetch base/fmt/size/swizzle + resolve list from `ExecuteDraw`/`ExecuteResolve`). Find the draw that produces the facade and the fetch feeding its blue channel.
3. One-shot `DXT-REAL`/`SKY-DIAG` instrumentation for that texture (block offset, format, byteswap mode, decoded pixel). Compare against the working D3D11 `renderer/src/render_resources.cpp::LinearLevelInto`.
4. A/B toggles (one at a time): unconditional byteswap on/off, FP32 vs selective highp, resolve-defer on/off, texture-adjust clamp order. Keep only changes with a clear visual win and no FPS regression.
5. Acceptance: zero blue speckles on the lighthouse facade at 360p/540p/720p, on both Adreno and Mali, with no new shadow/geometry artifacts.

### A2. Garbled text / black textures (issues #5, #7 — not locally reproducible)
Do **not** blind-patch for these. The WIP patch already targets the three most likely causes (DXT endian, clamp order, NaN). Required evidence:
- Ask #7 for `EXPORT LOGS` (owner already requested on Nov 26) and #5 for a fresh log (the existing zip was ~1 KB — likely wrong file or log mirroring not active).
- Reproduce with GLES3 at the reporter's exact device/resolution; compare `bs1_frame.txt` fetch records for the garbled font texture and black texture (base address, fmt, tiled flag, endian, swizzle, resolved-vs-guest flag).
- Suggested mitigation if logs never arrive: document as device-specific and keep the conservative WIP fixes; do not add more speculative shader changes.
- Owner's plan to deprecate the Vulkan backend removes the blurry/low-FPS half of #5; mirror that by hiding/removing the launcher `DRIVER` pill toggle once the custom GLES3 path is confirmed default everywhere.

### A3. Lighting / shadow follow-up (v0.0.8 per release notes)
- Keep the `v0.0.7.9` invariants: direct `msaa=0` clears must not be stolen by wider targets; 4× fast-clear aliases only redirect to exact `pitch*2`; fresh depth targets and resolves initialized to `1.0` via `ClearDepthTextureOne`; dead menu/loading resolves evicted after 300+ idle frames; compressed DXT/BC fetches must not match stale resolved targets; target creation isolated on `GL_TEXTURE31`.
- Implement dynamic object shadow casting polish only after A1's speckle work, since both touch the same depth/target code.
- Regression check: boot straight into Rapture (no `RES` tap) and confirm no pitch-black areas and no black tilted boxes.

## 3. Workstream B — FPS lock and general performance

### B1. The ~30 FPS lock
Facts from code:
- `renderer/src/gles3/android_custom_driver.cpp:255-275` — `VsyncLoop()` raises a graphics interrupt at the guest video-mode refresh rate (60 Hz default). This is a VBlank source, not a 30 FPS cap.
- `renderer/src/custom_settings.h:49` — `gpu.custom.vsync` defaults to `true`; `gles3_backend.cpp:2626-2628` calls `eglSwapInterval`, which caps at display refresh (~60 Hz), not 30. The Android CLI hardcodes `--gpu.custom.vsync=true` (`_work/rexglue-android-app/src/main.cpp:206`).
- SDK `gpu.frame_limit` defaults to `0.0` and its present-thread pacing is inert for the custom driver because `BeginPresent()` returns false (`android_custom_driver.cpp:180`). There is **no host-side 30 FPS cap** anywhere.
- `GameSurfaceView.kt:27-32` pumps `nativeFrame()` every 16 ms; the HUD FPS pill (`MainActivity.kt:88-101`) counts real presents via `nativeGetPresentCount()`.
- `20-KNOWN-ISSUES.md` Issue 3 records the user report of a guest-side BioShock options toggle, off by default. Docs' `~29–30 FPS` with `exec≈10s/10s` matches a guest-side 30 Hz cadence plus vsync.

Steps:
1. Capture a 60-second `PERF` sample in one static scene with `vsync=true` and `vsync=false` (runtime setting). If present count stays ~30 with vsync off, the cap is guest-side/PM4 cadence, not swap interval.
2. Correlate `VdSwap` present timestamps with VBlank interrupt counts to see whether the guest presents every other VBlank.
3. Separately expose `gpu.custom.vsync` through the existing JNI settings pattern (`_work/rexglue-android-app/src/main.cpp:301-330` shows how `RES`/filter are done) as a debug/test lever; keep the default `true` because disabling it caused 220+ FPS thermal drain on menus (`33-CUSTOM-RENDERER.md:153-156`).
4. If guest-side: locate the options value in the guest config/settings path (the Unreal config loader work in `19-WORKLOG.md` Session 12 is the precedent) and surface a launcher pill or CLI flag. Do not guess a patch to guest code without evidence.
5. Acceptance: a documented, user-visible lever for the frame cap, with a measured before/after on device.

### B2. Performance (without sacrificing the new stability fixes)
- The WIP patch reverts all pixel shaders to FP32; this may undo part of the `v0.0.7.6` 2–3× Adreno gain. Make it **selective**: keep FP32 only for shaders that use reciprocal/depth reconstruction/world-space constants (`BS1_HIGHP` already exists as a specialization mechanism in `xenos_glsl.cpp`); restore clamped mediump for simple text/UI/blit shaders after verifying no speckles.
- Preserve the existing wins: 16-pixel render-target alignment (`540p → 960×544`), VRS 2×2 on 3D HDR passes (hardcoded decision at `gles3_backend.cpp:1109-1122`), direct `glCopyImageSubData` resolves, 16-bit static index buffers, shader microcode/VB/IB caches, async shader compilers at `nice 10` (`gles3_shaders.cpp:334-337`).
- **Do not** re-enable Perf Mode's resolution step-down (it caused level-load failures) or restore `sustainedPerformanceMode`/GPU-thread `-8` priority (they capped clocks and starved the guest XMA thread). A Lanczos→bilinear A/B already showed no measurable FPS gain, so the `present_filter` pill is a quality lever, not a performance one.
- The real cost is GPU-side: ~620 draws and ~53 HDR resolves per frame with `eglSwapBuffers` blocking (`10-STATUS.md:173-192`). Focus on resolve/draw fast paths, not CPU thread tuning; the SDK present-thread sleep is inert and removing it would not fix GPU-bound frames.
- Re-check the WIP changes that can cost frames: disabling consecutive resolve coalescing (was a Mali TBDR win), forcing a shader clamp instead of direct copy for clamped float/snorm destinations, gamma/Lanczos changes in `gles3_shaders.cpp`, and UBO half-swap full re-uploads.
- Device matrix: Adreno 710 (owner, 540p), Mali-G78, SD870 (issue #5), SD680/4 GB (issue #7). Record FPS, `pres=` time, direct-resolve percentage, shader compile count, and thermals using `PERF(10s)` (`android_custom_driver.cpp:361-431`).
- Acceptance: no >10% FPS regression versus `v0.0.7.9` on Adreno 710; materially better or equal on Mali; blue speckles gone.

## 4. Workstream C — on-screen control customization button

### C1. Current code facts
- `TouchControlsView.kt` is a single canvas `View` (`android/app/src/main/java/com/bioshock1/recomp/TouchControlsView.kt:29`). Its whole layout is hardcoded in `getLayout(w,h)` at lines 107-209: sticks (116-117), D-pad (120-135), face/stick-click circles (138-152), shoulder/system pills (155-196), cached at 198-208. Opacity is inline in `onDraw` (221-280). Hit-testing is `classify()` (295-314); touch handling `onTouchEvent()` (327-385).
- Guest index contract: `0=A,1=B,2=X,3=Y,4=LB,5=RB,6=LT,7=RT,8=BACK,9=START,10=L3,11=R3,12..15=D-pad`; axes 0/1 = MOVE, 2/3 = LOOK. Native remapping would require changing `_work/rexglue-android-app/src/main.cpp:396-443`, so rebinding must happen in Kotlin by writing to different indices.
- `MainActivity.kt`: prefs at line 62 (`bs1_render_prefs`), HUD bar built in `startGame()` (592-654), `makeHudPill()` (386-394) already sets `isFocusable=false`/`isFocusableInTouchMode=false`, and bar has `FOCUS_BLOCK_DESCENDANTS` (597). `TOUCH` pill at 643-648 only toggles visibility; nothing is persisted.
- Gamepad merge at 396-414 and handlers at 429-536 are independent of touch config. Native rising-edge latch (`main.cpp:438-442` and `64-69`) means stray presses during an editor would leak into the game.

### C2. Design (minimal, uses no new dependencies)
1. **New pill:** add `CUSTOMIZE` in `MainActivity.startGame()` right after `hudBar.addView(touchBtn)` (line 653), using `makeHudPill` so it inherits gamepad-focus protection. Add `liveCustomBtn` field near line 60. Label toggles `CUSTOMIZE` ↔ `DONE`; while editing force `touchOverlay.visibility = VISIBLE` and `liveTouchBtn.text = "TOUCH: ON"`.
2. **Config model in `TouchControlsView.kt`:** a small `TouchConfig` holding normalized values (`x/w`, `y/h`, size as fraction of `u = min(w,h)`, opacity multiplier, optional guest-index bindings, preset name). `getLayout()` builds `Layout` from it; keep `onDraw()`/`classify()` consuming `Layout` unchanged. Presets: DEFAULT, COMPACT, LEFT-HANDED, RIGHT-HANDED.
3. **Edit mode:** `var editMode`, `selectedControlId`. In `onTouchEvent()` handle toolbar hits first (drag, size ±, opacity ±, BIND, PRESET, RESET, DONE), then normal classification. While editing, suppress `listener?.onControls(...)` at line 382 (send zeros or skip) and guard the touch-derived `pushMergedControls()` path so the native latch sees no phantom presses. Gamepad input can remain live via the `pad*` arrays.
4. **Persistence:** reuse `bs1_render_prefs` with `touch_config_v1` (platform `org.json`, no Gradle change) written on DONE, plus `touch_enabled_v1` so TOUCH ON/OFF survives restart. Load near `MainActivity.kt:133-139` and pass into `TouchControlsView(this, config)` at line 576.
5. **Edge cases:** landscape is locked but foldable/multi-window resizes still happen — normalized coordinates only; enforce a minimum hit slop comparable to the current +14 px pill margin; avoid a focusable Dialog/PopupMenu; keep the Xbox Guide key consumption at 478-480 intact; add `contentDescription` to the new pill.
6. **Acceptance:** move/resize/rebind/opacity + preset + reset all survive app restart; HUD pills never steal gamepad focus; no phantom inputs; TOUCH OFF/ON interoperates; layout still usable after a window resize.

### C3. Implemented (2026-11-26) — verified on SM-A366B

- `TouchControlsView.kt` now carries a normalized `TouchConfig` (`positions`, `scale`, `opacity`, `bindings`, `preset`) with `exportConfig()`/`applyConfig()` using platform `org.json`. `getLayout()` builds sticks/D-pad/circles/pills from that config; all 15 controls are individually addressable.
- `editMode` draws a 9-button in-canvas toolbar: **SEL / S- / S+ / O- / O+ / BIND / PRESET / RESET / DONE**. Tap a control to select (white outline), drag to move, `S±` resize, `O±` fade, `BIND` cycles the guest index, `PRESET` cycles DEFAULT → COMPACT → LEFT → RIGHT, `RESET` restores defaults, `DONE` saves and exits. Guest input is suppressed in edit mode, so the native rising-edge latch cannot see phantom presses.
- `MainActivity.kt` adds a `CUSTOMIZE` HUD pill after `TOUCH` (non-focusable, like the others), loads/saves `touch_config_v1` in `bs1_render_prefs`, forces the overlay visible while editing, clears touch state on entry/exit, and exits edit mode when `TOUCH: OFF` is pressed.
- On-device verification: `CUSTOMIZE` → `DONE` label + toolbar appeared; selecting A, `S+`, `O-`, `BIND` (→1) then `DONE` persisted across reopen; dragging A and reopening confirmed the new position persisted; `RESET` restored the original layout. Gradle `assembleDebug` BUILD SUCCESSFUL.
- The renderer WIP files were left byte-for-byte unchanged; only the two Kotlin launcher files and this document were edited for this feature.

## 5. Diagnostics and validation infrastructure

- Frame dump already in the WIP patch: create `bs1_dump.trig` in `Android/data/com.bioshock1.recomp/files/`, let one frame render, read `bs1_frame.txt`; `ExecutePresent()` closes the file and logs `BS1-DUMP complete`. Keep this for `#5`/`#7` triage; consider an `EXPORT LOGS` button addition that attaches it.
- Keep one-shot `DXT-REAL`, `SKY-DIAG`, `TEX-DIM2`, `CUTSCENE-DIAG`, `SHADER-PERF-DIAG`, `PERF(10s)` gates bounded so logs stay small — issue #5's 1 KB log shows the current mirroring path can silently capture nothing.
- `scripts/verify-all.py` (12/12 PASS historically) must stay green after each renderer change.
- Docs to update with evidence as work lands: `10-STATUS.md` (new top entry, move the four open issues into `20-KNOWN-ISSUES.md` with proper diagnosis sections), `19-WORKLOG.md` (new Session 30+ with Next action), `11-ROADMAP.md` (Phase 5 gate + compatibility DB checkbox), `00-START-HERE.md` (current release version), and this document (mark each item done with evidence).

## 6. Recommended sequencing

1. **Validate WIP** — build/test APK, `verify-all.py`, baseline A/B logs + FPS on Adreno/Mali. (Gate before anything else.)
2. **Blue speckles** — frame dump + `DXT-REAL` on the lighthouse facade; minimal fix; A/B at three resolutions.
3. **Selective highp** — restore mediump for safe shaders, keep FP32 for NaN-sensitive ones; re-measure FPS.
4. **FPS cap investigation** — vsync A/B + `VdSwap`/VBlank correlation; surface a guest-side lever if confirmed.
5. **Control customization pill** — independent of renderer work; can proceed in parallel once step 1 is green.
6. **Issue #5/#7** — only with reporter logs; otherwise document and keep conservative fixes.
7. **Deprecate Vulkan toggle** — after GLES3 is confirmed as default on all devices.
8. **Docs + release** — refresh docs, TEST APK to owner, then publish `v0.0.8` only after owner sign-off.

## 7. Open questions for the owner

- Should the WIP patch be committed as the base of `v0.0.8`, or do you want a clean branch from `v0.0.7.9` with selected hunks?
- Do you want the `CUSTOMIZE` pill implemented now, or only specified here first?
- Is the FPS target "remove the cap entirely" or "expose an options toggle / 60 FPS mode"?
- Can you provide the device on which the blue speckles appear (exact model + `RES` + Perf Mode state) and a screenshot?
