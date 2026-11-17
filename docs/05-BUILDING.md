# Building — exact commands and prerequisites

This is the command-level reference. All paths assume the project root is
`F:\Game Recomp Into a Native Android Port\BioShock 1 Android Port With Launcher\BioShock1-Android-Recomp`.

## 1. Prerequisites

### Already installed for you

| Tool | Version / location | How it was obtained |
|---|---|---|
| Python | 3.14.7 (`C:\Python314\python.exe`) | pre-existing |
| `py7zr` | installed | pre-existing |
| Git | 2.55 | pre-existing |
| Java (JDK) | 21.0.13 (`C:\Program Files\Eclipse Adoptium\jdk-21.0.13.9-hotspot`) | pre-existing |
| Clang / Clang++ | 23.1.3 (llvm-mingw) | pre-existing |
| Ninja | `C:\Python314\Scripts\ninja.exe` | pre-existing |
| Android SDK platform 34 | `F:\tools\android-sdk\platforms\android-34` | `scripts/install-toolchain.ps1` |
| Android build-tools 34.0.0 | `...\build-tools\34.0.0` | same |
| Android platform-tools | `...\platform-tools` (adb) | same |
| Android NDK | `...\ndk\26.3.11579264` | same |
| CMake | `...\cmake\3.22.1\bin\cmake.exe` | same |
| Gradle | 8.6 (wrapper, downloaded on first build) | `android/gradlew.bat` |

> **Note:** the environment variable `JAVA_HOME` was initially stale
> (`jdk-21.0.12.101-hotspot`, which does not exist). `android/gradlew.bat` was
> written to fall back to `java` on PATH when that happens. If you use another
> Gradle wrapper, set `JAVA_HOME` to the real JDK 21 path first.

### Still required for full codegen (not installed here)

* **ReXGlue SDK build prerequisites**: Clang **20+**, CMake **3.25+**, Ninja,
  and the SDK's recursive git submodules. The archive in `third_party/rexglue-sdk`
  is the source snapshot without submodule contents. Clone the upstream repo
  recursively, materialise any Windows symlinked sources (libmspack), and build
  with its presets. See `third_party/360tools/README.md` for the exact version
  ladder.
* Optionally **Xenia / XenosRecomp** for deeper GPU work; also submodule-based.

The Android build itself needs nothing beyond what is already installed.

## 2. Extract the game dump (one-time)

```powershell
python -c "import py7zr, pathlib; p=pathlib.Path(r'F:\Game Recomp Into a Native Android Port\BioShock 1 Android Port With Launcher\BioShock (World) (En,Fr,De,Es,It).7z'); py7zr.SevenZipFile(p).extractall(r'BioShock1-Android-Recomp\_game')"
```

Or let the pipeline do it for you via `pipeline.py inspect` if `_game/*.iso`
is missing (it will tell you to place the ISO manually).

## 3. Host pipeline

```powershell
cd "F:\Game Recomp Into a Native Android Port\BioShock 1 Android Port With Launcher\BioShock1-Android-Recomp"

python tools/vendor_extract.py          # only if third_party/ is missing
python tools/pipeline.py inspect
python tools/pipeline.py configure
python tools/pipeline.py build-native
```

Expected output from `build-native`:

```
[1/5] Building CXX object CMakeFiles/bioshock1-selftest.dir/src/engine.cpp.obj
...
selftest OK
```

and a status line like:
`{"initialized":true,"hleImports":8,"missingImports":1,"frames":10,...}`

## 4. Android APK

```powershell
cd android
.\gradlew.bat assembleDebug --no-daemon
```

Artefact:
`android\app\build\outputs\apk\debug\app-debug.apk`

Install and observe:

```powershell
adb install -r android\app\build\outputs\apk\debug\app-debug.apk
adb logcat -s BioShockRecomp native
```

A release build is also declared:

```powershell
.\gradlew.bat assembleRelease --no-daemon
```

## 5. CMake targets

`native/CMakeLists.txt` defines two modes:

| Mode | Condition | Target | Notes |
|---|---|---|---|
| Host | `NOT ANDROID` | `bioshock1-selftest` | No GLES; GPU layer counts frames only; CTest registered |
| Android | `ANDROID` | `bioshock1-native` (SHARED) | `BS_GLES3`, links `GLESv3`, `EGL`, `android`, `log`, `z`, includes JNI bridge |

Generated code integration point (inside the `ANDROID` branch):

```cmake
# Add the ReXGlue/XenonRecomp output here once Stage 4 has run:
target_sources(bioshock1-native PRIVATE
    ${PROJECT_SOURCE_DIR}/../recomp/generated/*.cpp
)
```

## 6. Gradle configuration notes

* `compileSdk 34`, `minSdk 28` (Android 9), `targetSdk 34`.
* `abiFilters 'arm64-v8a'` — the recompiled code and SIMDE VMX path target ARM64.
* `ndkVersion '26.3.11579264'` — pinned to the version installed here; AGP 8.4.2
  otherwise tries (and fails) to fetch 26.1.10909125.
* AGP `8.4.2`, Kotlin `1.9.24`, Gradle wrapper `8.6` — a mutually compatible set.
* `local.properties` points `sdk.dir` at `F:\tools\android-sdk`. Do not check it
  into a shared repository if your path differs.

## 7. Troubleshooting the build

| Symptom | Cause | Fix |
|---|---|---|
| `JAVA_HOME` path not found | stale env var | use the provided `gradlew.bat`, or set it to the JDK 21 folder |
| `LicenceNotAcceptedException` | SDK licence hashes missing | run `(1..40 \| %% {'y'}) \| sdkmanager --licenses` once |
| AGP wants NDK 26.1.10909125 | no `ndkVersion` pin | keep `ndkVersion '26.3.11579264'` in `app/build.gradle` |
| `platforms;android-34-ext12` warning | Google ships API 34 as an "ext" package | harmless; the folder works for compilation |
| `gradle-wrapper.properties does not exist` | wrapper jar copied but properties not | copy it from `third_party/Xbox360RecompAndroid/android/gradle/wrapper/` |
| Native link error about `gl*` symbols | `BS_GLES3` set for a host build | only define `BS_GLES3` in the Android target |
| JNI `UnsatisfiedLinkError` | library name mismatch | `System.loadLibrary("bioshock1-native")` ↔ `libbioshock1-native.so` |

## 8. Verifying the APK contains no game data

```powershell
# List the APK and confirm there is no .iso/.xex/.bin BIOS inside
Add-Type -AssemblyName System.IO.Compression.FileSystem
$z=[IO.Compression.ZipFile]::OpenRead('android\app\build\outputs\apk\debug\app-debug.apk')
$z.Entries | Where-Object { $_.FullName -match '\.(iso|xex|bin|fsb|u)$' } | Select-Object FullName
$z.Dispose()
```

The result should be empty. The BIOS dump lives in `_reference/` and is never
referenced by Gradle or CMake.
