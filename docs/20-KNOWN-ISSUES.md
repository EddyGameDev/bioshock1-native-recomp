# Known Issues — detailed diagnosis (2026-11-21)

This document tracks the open issues on the experimental Android port, with the
evidence gathered so far. It is written to be handed to contributors / players.

**Test device:** SM-G998B (Exynos 2100, Mali-G78, GLES 3.2, Android 15).
Other player devices reported: TECNO LJ9 (Android 16).

---

## Issue 1 — Missing skybox / dark scene / lights not visible (Mali, OPEN)

### Symptom
- No skybox in outdoor areas; the lighthouse area is too dark to navigate.
- Lights that should be on (lighthouse beacon, lamps) are not visible.
- Previously thought to be "area-specific" — it is **not**: the cubemap that
  supplies sky/lighting is decoding to near-black transparent pixels.

### Evidence (from `DXT-REAL` / `SKY-DIAG` logs on-device)
- The sky cube is bound correctly — **not** the white fallback:
  `SKY-DIAG cube fetch: ... fmt=19 64x64 ... texFinal=0x6F whiteCube=0x2 fallback=no`.
  (`fmt=19` = DXT2/3; `constDim=3` / `fetchDim=3` = real cubemap.)
- The DXT CPU decoder (`gles3_resources.cpp`, `DecodeDxtBlock` / `LinearizeLevel`)
  is producing garbage/dark pixels for these textures. Example `DXT-REAL` lines:
  ```
  mode=2 endian=1 tiled=1 64x64 pitch=128 off=0 raw=0000000000000000 px=312D1900
  mode=2 endian=1 tiled=1 64x64 pitch=128 off=0 raw=0000000000000000 px=47495400
  mode=2 endian=1 tiled=1 1024x1024 pitch=1024 off=0 raw=0000000000000000 px=FFFFFF00
  ```
  - `raw` bytes are `00` and decoded alpha is `00` (transparent).
  - `off=0` for every tiled block — suspicious: `TiledOffset2D()` appears to
    return 0 for the cube faces, so the decoder reads the wrong memory.
  - Note: the first diagnostic iteration read `src[0]` without the tiled offset;
    a corrected diagnostic (`DXT-REAL`) now reads the actual offset and the
    actual decoded pixel, and still shows wrong colors — so this is a real
    decode problem, not a logging artifact.

### Root cause — strongest hypothesis
Mali has no S3TC (`s3tc=0`), so DXT textures take the **CPU DXT decoder** path.
The Windows/D3D11 path uses hardware DXT decode and renders the sky correctly,
so the bug is isolated to the CPU decoder. Candidate faults:
1. **Tiled block offset wrong for cubemap faces.** `TiledOffset2D(bx+bx0, by+by0,
   pa, bpbLog2)` may need cube-face-specific swizzle/addressing; `off=0` suggests
   the face base is wrong. DXT3 cubemaps are tiled with `bpbLog2=4`.
2. **Cubemap faces/mips not fully uploaded.** `mipCount` is forced to 1 for cubes
   and the mip-upload loop hardcodes `GL_TEXTURE_2D` — so cube mips are missing.
   Base level 0 should still show, but combined with a bad face stride the cube
   may be mostly black.
3. **DXT3 alpha decode.** `DecodeDxtBlock` mode 2 alpha indexing was recently
   fixed (`alpha[p]`), but the byte order for tiled DXT3 alpha may still be wrong.

### Files
- `renderer/src/gles3/gles3_resources.cpp` — `DecodeDxtBlock`, `LinearizeLevel`,
  `UploadTextureData` (isCube handling), `TiledOffset2D`.
- Reference (working): `renderer/src/render_resources.cpp` (`LinearLevelInto`).

### Next action
Instrument `TiledOffset2D` for the sky cube (print bx, by, pa, bpbLog2, computed
off for face 0..5). Compare against the D3D11 `LinearLevelInto` tiled path. Fix
the cube-face block addressing and upload all six faces + base level correctly.

---

## Issue 2 — Audio dies randomly / game fails to load (Mali, OPEN, intermittent)

### Symptom
- Audio works for a while, then dies (silent). Random and device-dependent.
- When audio dies, the game often does not finish loading into gameplay; it
  must be restarted multiple times before it loads.
- Player reports: "audio survives each time now" (after watchdog), then
  "Audio died again!!" on later sessions. Most recent: "Audio Still died after
  some restart testing, took longer too fail but its too random to say anything";
  "game also does not load in each time, needs multiple restart".

### Evidence / mechanism (well understood)
The guest's level-load progress is coupled to the audio callback chain:
```
AAudio data callback (AAudio_1)
  -> PullAAudio -> AudioFill (make_fill)
    -> AudioSystem::mix_period -> slot.on_frame_played()
      -> APU semaphore credit
        -> APU::WorkerMain executes guest XAudio2 render callback
          -> guest submits frame -> audio_->Submit() -> queue
```
When AAudio stops calling back, no credits are returned. After ~32 credits
(~160 ms at 5 ms/period) the APU worker stops invoking the guest callback, and
any guest-side wait on audio progress during load hangs.

Log evidence of Samsung HAL route changes that kill the MMAP stream:
```
audio_hw_proxy2: mmap_out-proxy_close_playback_stream: closed PCM Device FD(30)
audio_hw_proxy_route: proxy-set_route: Fade-out start
audio_hw_proxy2: fast_out-proxy_open_playback_stream ...
audio_hw_proxy2: fast_out-proxy_close_playback_stream: closed PCM Device FD(30)
```
`AAUDIO_PERFORMANCE_MODE_LOW_LATENCY` uses Samsung's MMAP path, which dies on
screen-state / audio-route changes. `AAUDIO_PERFORMANCE_MODE_NONE` avoids the
dropout but causes stretching/crackling (guest's fixed 256-frame callback does
not align with the buffer-queue burst when the frame-lock is removed).

### Fixes attempted and outcome
| Attempt | Result |
|---|---|
| LOW_LATENCY + 256-frame lock (original) | Clean audio, but MMAP dies randomly; load stalls. |
| NONE mode + 256-frame lock | No dropout, but stretching/crackling. |
| NONE mode, no frame lock | Stretching/crackling persisted. |
| Liveness watchdog (pump mixer when fill stalls >100 ms) | Load survives audio death, but audio stays silent (device never reopened). User: "audio survives... the fix is great". |
| Watchdog + synchronous `CloseDevice()` | **Regression**: `AAudioStream_close()` blocks on wedged HAL, freezes watchdog, game stops loading. Reverted. |
| Hold `AUDIOFOCUS_GAIN` in launcher + watchdog | Audio still dies (random), takes longer to fail; load still intermittent. |

### Current state (experimental test builds)
- LOW_LATENCY + 256-frame lock restored (best audio quality).
- Liveness watchdog pumps credits but does NOT call `CloseDevice()` (avoids the
  freeze). So the guest loads even if audio dies, but audio may stay silent.
- Launcher now requests `AUDIOFOCUS_GAIN` to reduce route-change deaths.
- An `IsDeviceLost()` + scheduled reopen path exists, but the Samsung HAL often
  never delivers the error callback needed to trigger it.

### Next action
1. Implement a **non-blocking** device reopen: when the watchdog detects a
   stalled fill, hand close+open to a detached thread (never block the mixer
   thread in `AAudioStream_close()`).
2. Add `REXAPU` diagnostics to the shareable log (device state, last-fill age,
   error codes) so failures are observable from EXPORT LOGS.
3. Consider an OpenSL ES / AudioTrack fallback for Samsung devices where MMAP
   is unreliable.

### Files
- `third_party/rexglue-git/src/apu/sdl3/sdl3_audio_driver.cpp` (AAudio setup,
  `OnAAudioError`, `IsDeviceLost`, `CloseDevice`).
- `third_party/rexglue-git/src/apu/audio_system.cpp` (watchdog, mixer pump).
- `third_party/rexglue-git/src/kernel/apu.cpp` (APU credits, worker).
- `android/app/src/main/java/com/bioshock1/recomp/MainActivity.kt` (audio focus).

---

## Issue 3 — 30 FPS cap (OPEN, low priority)

User reports an in-game options toggle (off by default) that unlocks the frame
rate. No FPS/vsync toggle exists in the launcher code — only a `vsync` setting
always passed as `--gpu.custom.vsync=true`. On-device PERF shows ~29–30 FPS with
the GPU worker busy the entire interval (`exec≈10s/10s`, `pres≈3s/10s`), i.e.
GPU-bound at ~4,250 draws/frame. Need to identify the guest-side option and
decide whether to expose it in the launcher.

---

## Issue 4 — Bathysphere projector cutscene bug (PARKED)

Documented in `docs/10-STATUS.md`. Xenia shader dumps confirmed the shader
translation is correct. Needs a state/input or draw-list comparison. Not worked
on in this session.

---

## Build / release status
- No GitHub release or push made since the user's "do not release until I test"
  instruction. All fixes below are in local **TEST** APKs only:
  - `BioShock1-Android-v0.0.6.6-TEST-norestep.apk` (Perf Mode without resolution step-down)
  - `BioShock1-Android-v0.0.6.6-TEST-dxtswap.apk` (+ DXT byte-swap fix)
  - `BioShock1-Android-v0.0.6.6-TEST-audio-none-diag.apk` (NONE mode, diagnostics)
  - `BioShock1-Android-v0.0.6.6-TEST-reopen-diag.apk` (forced reopen — reverted)
  - `BioShock1-Android-v0.0.6.6-TEST-focus.apk` (audio focus + watchdog)
  - `BioShock1-Android-v0.0.6.6-TEST-skyreal.apk` (corrected DXT diagnostic)
- Latest published release remains `v0.0.6.6-audio-load-fix-experimental`.
- `versionName` in `android/app/build.gradle` is currently
  `0.0.6.6-audio-load-fix-experimental` (test APKs reuse this version).
