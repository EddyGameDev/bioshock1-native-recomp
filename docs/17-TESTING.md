# Testing and verification

Everything below has been executed at least once during project setup; repeat
any of it after changing code.

## 1. Host tests

### Native self-test

```powershell
python tools/pipeline.py build-native
```

Checks:

* `bs_engine_init()` returns 1 and registers the bootstrap imports.
* `hle::Kernel::call()` with ordinal 9999 returns false and appends to
  `missing_imports()` exactly once (fail-soft behaviour).
* Ten `bs_engine_render()` calls advance `gpu::stats().frames` to 10.
* `bs_engine_status_json()` returns parseable JSON.

Expected final line: `selftest OK`.

### XEX analysis regression

```powershell
python tools/xex_inspect.py _game\default.xex -o config\xex_analysis.json
python -c "import json; a=json.load(open('config/xex_analysis.json')); assert a['summary']['title_id']=='0x545407D8'; assert a['xdk_builds']==[5426]; assert a['summary']['base_address']=='0x82000000'; assert a['summary']['total_import_records']==427; print('analysis OK')"
```

If any assertion fails, a parser regression was introduced. Do not regenerate
`bioshock1.toml` until it is fixed.

### ISO reader round trip

```powershell
python tools/xdvdfs.py list "_game\BioShock (World) (En,Fr,De,Es,It).iso" | Select-Object -First 10
python tools/xdvdfs.py extract "_game\BioShock (World) (En,Fr,De,Es,It).iso" default.xex _game\default.xex.verify
```

Compare hashes of `default.xex` and `default.xex.verify`; they must match.

## 2. Android build tests

```powershell
cd android
.\gradlew.bat assembleDebug --no-daemon
```

Success criteria:

* `BUILD SUCCESSFUL`
* `android\app\build\outputs\apk\debug\app-debug.apk` exists
* `lib\arm64-v8a\libbioshock1-native.so` is inside the APK

Package-content check (no game data):

```powershell
Add-Type -AssemblyName System.IO.Compression.FileSystem
$z=[IO.Compression.ZipFile]::OpenRead('android\app\build\outputs\apk\debug\app-debug.apk')
$z.Entries | Where-Object { $_.FullName -match '\.(iso|xex|fsb|u)$' } | Select-Object FullName
$z.Dispose()
```

Expected: no rows. (The Kotlin runtime's `DebugProbesKt.bin` is fine — it is
not a console BIOS.)

## 3. On-device manual test (M0) — PASSED 2026-11-17

Executed on Samsung SM-A366B / Android 16. Full evidence, screenshots and the
two UI bugs fixed during the test are in `24-DEVICE-M0.md`. The GLES3 diagnostic
pattern and touch overlay render correctly; `glError=0x0`, shaders compiled,
`nativeInit -> true`. The remaining steps below are retained as the
re-production procedure.

1. Install: `adb install -r android\app\build\outputs\apk\debug\app-debug.apk`.
2. Launch: menu should show engine library "loaded", HLE import count 8,
   XEX extracted "no".
3. Tap **Import ISO**, select your BioShock ISO (7.8 GB). Wait for the copy,
   `default.xex` extraction and full game-tree extraction (progress text shows
   file/byte counts). Ensure the device has several GB free.
4. Tap **Start Game**. Expected: animated GLES3 diagnostic pattern (wave + grid
   + vignette) at full screen, with the translucent touch overlay on top.
5. Check `adb logcat`: no shader compile errors, no `UnsatisfiedLinkError`.

If step 4 shows the pattern, M0 is complete: EGL3, shaders, JNI, frame loop and
the launcher-to-native path all work. This is the current verified milestone
in terms of code correctness (the APK was built, but no device was attached in
this environment, so step 4 is the acceptance test for *you* to run).

## 4. Future milestones (not yet testable)

| Milestone | Test |
|---|---|
| M1 PM4 trace | log first N command-buffer packet names; compare with Xenia traces |
| M2 first draw | one textured triangle from a real Xenos draw call |
| M3 title screen | game menu renders; input responds |
| M4 gameplay | walk around Rapture; correct lighting/textures |
| M5 complete | audio, saves, cutscenes, full playthrough |

Each milestone gets a boot log (`docs/logs/`) and a screenshot, following the
MGS3 reference project's practice (`boot_35.png`, `boot_log_*.txt`).

## 5. Continuous checks worth adding

* A Python test that re-parses the XEX and diffs the JSON against a golden file.
* A unit test per HLE import family with synthetic big-endian guest memory.
* A CI workflow mirroring `Xbox360RecompAndroid`'s `build.yml` (Gradle assemble
  + host selftest). Flutter is not needed.
* A script that verifies generated C++ contains no unresolved `__imp__*`
  symbols before adding it to the Android target.
