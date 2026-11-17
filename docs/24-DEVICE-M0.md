# On-device M0 acceptance test — PASSED

Date: 2026-11-17. Device: Samsung **SM-A366B** (Galaxy A36 5G), Android **16**
(API 36), `arm64-v8a`, Adreno GPU. Connected over ADB.

## What was tested

This is the **launcher/engine boot test**, not the game: it verifies that the
APK installs, the native library loads, the GLES3 context is created, shaders
compile, frames are drawn, and the touch overlay renders on real hardware.

| Step | Command / action | Result |
|---|---|---|
| Install | `adb install -r app-debug.apk` | `Success` |
| Launch | `adb shell am start -n com.bioshock1.recomp/.MainActivity` | activity resumed |
| Menu screenshot | `adb shell screencap` | UI renders; status lines correct |
| Seed XEX | push `default.xex` to app-private `files/game/` via `/data/local/tmp` | 13,021,184 bytes present |
| Start | `input tap 1130 785` (START GAME) | GLES3 surface created |
| GLES3 screenshot | `adb shell screencap` | animated blue wave + grid + vignette pattern + touch overlay |
| Logcat | `logcat -d` | shaders compiled, `glError=0x0`, `ready=1`, `nativeInit -> true`, `first nativeRender() issued`, no FATAL/Exception |

Screenshots are stored in `_work/device_m0_*.png` (gitignored).

## Bugs found and fixed during the test

1. **"Engine library: MISSING"** — the status check looked for
   `libbioshock1-native.so` in `applicationInfo.nativeLibraryDir`. With
   `extractNativeLibs=false` (default for modern packaging) the library is
   loaded directly from inside the APK and that directory is empty. Fixed:
   status now uses a successful JNI call (`nativeHleImportCount() > 0`) as proof.
2. **"HLE imports implemented: 0"** — bootstrap imports were registered only
   in `Kernel::initialize()` (called at game start). Fixed: registration now
   also happens at library load, so the menu shows `8` immediately.
3. **First screenshot looked black** — it was captured before the first GL
   frame was presented; a later capture shows the pattern. Not a code bug.
   Added `BS1`/`BS1-GPU` logcat diagnostics to make this unambiguous.

After the fixes, the menu reads:

```
Engine library: loaded
XEX extracted: 13021184 bytes
HLE imports implemented: 8
```

## Limitations

* The game itself is **not** running: the APK uses the hand-written engine
  only (`BS_USE_GENERATED_CODE=OFF`). This test validates the delivery shell
  and GLES3 presentation path.
* ISO import via the Android SAF picker was not automated; `default.xex` was
  seeded directly into app-private storage for the test. The SAF flow itself
  is unchanged and still needs a manual check.
* No game rendering, HLE semantics or touch→XINPUT mapping is exercised yet.

## Reproducing

```powershell
$adb='F:\tools\android-sdk\platform-tools\adb.exe'
$apk='...\android\app\build\outputs\apk\debug\app-debug.apk'
& $adb install -r $apk
& $adb shell am start -n com.bioshock1.recomp/.MainActivity
# tap START GAME; capture with:
& $adb shell screencap -p /sdcard/s.png
& $adb pull /sdcard/s.png _work\device.png
& $adb logcat -d | Select-String 'BS1|BS1-GPU'
```
