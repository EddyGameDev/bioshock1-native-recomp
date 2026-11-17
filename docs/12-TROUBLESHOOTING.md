# Troubleshooting

## Setup / toolchain

| Problem | Likely cause | Fix |
|---|---|---|
| `Cannot find path gradlew.bat` | using bash script name on Windows | use `.\gradlew.bat` from the `android/` folder |
| `JAVA_HOME` not found | stale environment variable (`jdk-21.0.12.101-hotspot`) | the bundled `gradlew.bat` falls back to `java` on PATH; or set `JAVA_HOME` to `C:\Program Files\Eclipse Adoptium\jdk-21.0.13.9-hotspot` |
| Gradle wrapper properties missing | jar copied without properties | copy `gradle-wrapper.properties` from `third_party/Xbox360RecompAndroid/android/gradle/wrapper/` |
| `LicenceNotAcceptedException` | SDK licence hashes incomplete | set `JAVA_HOME` correctly, then run `(1..40 \| ForEach-Object {'y'}) \| sdkmanager --licenses` |
| AGP tries to install NDK 26.1.10909125 | no NDK pin | keep `ndkVersion '26.3.11579264'` in `app/build.gradle` |
| `platforms;android-34-ext12` inconsistent-location warning | Google now ships API 34 as an extension package | harmless; Gradle installs a second copy as `android-34-2` and proceeds |
| Chocolatey CMake install fails without admin | `Test-ProcessAdminRights` missing | don't use Chocolatey; CMake 3.22.1 is installed in the SDK and works for host builds |
| `py7zr` missing | not installed | `python -m pip install py7zr` |
| `cmake` not on PATH | only the SDK copy exists | use the full path `F:\tools\android-sdk\cmake\3.22.1\bin\cmake.exe`, or `pipeline.py build-native` which does this for you |

## Game data / analysis

| Problem | Likely cause | Fix |
|---|---|---|
| `No ISO in _game` | dump not extracted | run `pipeline.py inspect` or extract the `.7z` into `_game/` |
| `XDVDFS partition not found` | ISO is encrypted (not a redump-style image) | use `extract-xiso` to decrypt first; re-run `xdvdfs.py` |
| `default.xex not found` | non-standard disc layout | run `xdvdfs.py list <iso>` and check actual filenames |
| Title ID looks like `0x82C36D80` | using the old/buggy parser | use `tools/xex_inspect.py`; real ID is `0x545407D8` (ASCII fields) |
| Import ordinals are huge values (~1200+) | treating thunk addresses as ordinals | ignore; ordinals need the decompressed PE (ReXGlue codegen) |
| `Entry point: None` | no `0x00030000` XEX header | normal for this XEX; read it from the PE header after codegen decompression |
| XEX SHA-1 mismatch | modified/patched dump | expected if a title update was applied; note it in `09-RE-FINDINGS.md` |

## Native / C++

| Problem | Likely cause | Fix |
|---|---|---|
| Link errors for `gl*` symbols on host | `BS_GLES3` defined in a host build | only define it in the Android CMake target |
| `UnsatisfiedLinkError: bioshock1-native` | library name mismatch | Kotlin uses `System.loadLibrary("bioshock1-native")` → file must be `libbioshock1-native.so` |
| `PPCContext` layout mismatch with generated code | mixing ReXGlue and XenonRecomp conventions | re-check the runtime header each tool expects; align `include/ppc_context.h` |
| Self-test fails on `frames == 10` assertion | GPU stats changed | update the assertion; the no-op backend still counts frames |
| NDK compile of generated code fails with `no member named 'atomic_ref' in namespace 'std'` | Android libc++ lacks `std::atomic_ref` even in NDK r28c | force-include `native/include/compat/atomic_ref.h` with `-DBS_NEED_ATOMIC_REF_SHIM=1`; this also provides jthread/stop_token |
| `no type named 'jthread' in namespace 'std'` | Android libc++ omits public `<stop_token>`/jthread | same compatibility shim |
| CMake Android configure fails on `Invalid character escape '\G'` for NDK path | Windows backslashes in `CMAKE_ANDROID_NDK` | pass all Android/NDK paths with forward slashes |
| CMake says `Invalid character escape '\G'` | Windows backslash path passed to CMake string | use `file(TO_CMAKE_PATH ...)` — already fixed in `tools/codegen-validate/CMakeLists.txt` |
| `rexglue codegen` fails with "no manifest" | run from project dir or pass manifest path | `rexglue codegen <path-to-manifest.toml>` |

## Runtime on device

| Symptom | Likely cause | Next diagnostic |
|---|---|---|
| Black screen after Start | EGL3 context failed | check logcat for shader compile errors; confirm GLES 3.0 support |
| Test pattern shows but game never starts | generated code not integrated (expected today) | see `10-STATUS.md` |
| Import fails with "default.xex not found" | ISO not an XGD2 redump | re-check with `xdvdfs.py list` on host |
| App crashes when importing 7.8 GB ISO | out of storage | ensure several GB free in app-private storage |
| Missing-import list grows during boot | normal early port behaviour | implement handlers in frequency order (`06-HLE-KERNEL.md`) |
| Guest file reads fail | only `default.xex` extracted | extend launcher to extract full game tree |
| Touch controls do nothing | input bridge not wired to guest XINPUT yet | implement `HookXInputState` (`09-RE-FINDINGS.md`) |

## Getting help

Include, in this order:

1. The exact command you ran and its full output.
2. `python --version`, `cmake --version`, `java -version`, NDK version.
3. The relevant section of `config/xex_analysis.json` (never your whole ISO).
4. A logcat excerpt with the package tag.

Do **not** post or upload game data, BIOS dumps or proprietary symbol files.
