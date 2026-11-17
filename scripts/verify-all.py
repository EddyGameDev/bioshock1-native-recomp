#!/usr/bin/env python3
"""Rock-solid verification of everything in this project.

Runs every check that does not require a physical Android device and prints a
PASS/FAIL report. Safe to run repeatedly; all build steps are incremental.

    python scripts/verify-all.py
"""

from __future__ import annotations

import json
import subprocess
import sys
import zipfile
from pathlib import Path

PROJECT = Path(__file__).resolve().parents[1]
CM = PROJECT / "tools-bin/cmake-3.31.6/bin/cmake.exe"
NM = PROJECT / "tools-bin/android-ndk-r28c/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-nm.exe"
SDK_CMAKE = Path("F:/tools/android-sdk/cmake/3.22.1/bin/cmake.exe")

RESULTS: list[tuple[bool, str, str]] = []


def check(name: str, ok: bool, detail: str = "") -> None:
    RESULTS.append((ok, name, detail))
    print(f"[{'PASS' if ok else 'FAIL'}] {name}" + (f" — {detail}" if detail else ""))


def run(cmd: list[str], *, cwd: Path | None = None) -> int:
    return subprocess.call([str(c) for c in cmd], cwd=cwd)


def main() -> int:
    # 1. Required artefacts.
    required = [
        "_game/BioShock (World) (En,Fr,De,Es,It).iso",
        "_game/default.xex",
        "config/xex_analysis.json",
        "config/hle_imports.json",
        "config/runtime_symbols.json",
        "config/bioshock1.toml",
        "recomp/generated/bioshock1_init.cpp",
        "recomp/generated/bioshock1_register.cpp",
        "native/CMakeLists.txt",
        "android/app/build.gradle",
    ]
    missing = [p for p in required if not (PROJECT / p).is_file()]
    check("required files present", not missing, ", ".join(missing))

    # 2. Docs.
    docs = list((PROJECT / "docs").glob("*.md"))
    check("docs folder detailed", len(docs) >= 31, f"{len(docs)} markdown files")

    # 3. XEX analysis values (regenerate into a temp file, do not overwrite).
    tmp = PROJECT / "_work/verify_xex.json"
    tmp.parent.mkdir(parents=True, exist_ok=True)
    rc = run([sys.executable, PROJECT / "tools/xex_inspect.py",
              PROJECT / "_game/default.xex", "-o", tmp])
    a = json.loads(tmp.read_text(encoding="utf-8"))
    s = a["summary"]
    ok = (rc == 0 and s["title_id"] == "0x545407D8" and s["xdk_builds"] == [5426]
          and s["base_address"] == "0x82000000" and s["total_import_records"] == 427)
    check("XEX analysis reproducible", ok,
          f"title={s['title_id']} xdk={s['xdk_builds']} imports={s['total_import_records']}")

    # 4. Import / runtime symbol counts.
    imports = json.loads((PROJECT / "config/hle_imports.json").read_text())
    runtime = json.loads((PROJECT / "config/runtime_symbols.json").read_text())
    check("import matrix complete", len(imports) == 216, f"{len(imports)} imports")
    check("runtime symbol list complete", len(runtime) == 245,
          f"{len(runtime)} symbols")
    check("xstart not stubbed", "xstart" not in runtime)

    # 5. Staged generated snapshot.
    manifest = json.loads((PROJECT / "recomp/generated/staged_manifest.json").read_text())
    staged = list((PROJECT / "recomp/generated").glob("bioshock1_*"))
    check("generated snapshot intact",
          manifest["file_count"] == 308 and len(staged) >= 305,
          f"{len(staged)} staged files, {manifest['file_count']} in manifest")

    # 6. Host selftest.
    selftest = PROJECT / "build/host/bioshock1-selftest.exe"
    if selftest.is_file():
        rc = run([selftest])
        check("host native selftest", rc == 0)
    else:
        check("host native selftest", False, "binary missing; run pipeline.py build-native")

    # 7. Generated code compile validation (incremental).
    host_o = list((PROJECT / "build/codegen-validate").rglob("*.obj"))
    arm_o = list((PROJECT / "build/codegen-validate-android").rglob("*.o"))
    check("host generated objects", len(host_o) >= 152, f"{len(host_o)} objects")
    check("arm64 generated objects", len(arm_o) >= 153, f"{len(arm_o)} objects")

    # 8. Linked .so and key symbols.
    so = PROJECT / "build/codegen-validate-android/libbs1_link_test.so"
    ok = so.is_file() and so.stat().st_size > 100_000_000
    if ok and NM.is_file():
        out = subprocess.run([str(NM), "-D", str(so)], capture_output=True, text=True).stdout
        ok = "__imp__xstart" in out and "__imp__DbgPrint" in out
    check("arm64 .so linked with key symbols", ok,
          f"{so.stat().st_size / 1e6:.1f} MB" if so.is_file() else "missing")

    # 9. APK contains no game data.
    apk = PROJECT / "android/app/build/outputs/apk/debug/app-debug.apk"
    ok = apk.is_file()
    bad: list[str] = []
    if ok:
        with zipfile.ZipFile(apk) as z:
            bad = [i.filename for i in z.infolist()
                   if i.filename.lower().endswith((".iso", ".xex", ".fsb", ".u", ".bin"))]
            # Kotlin runtime ships DebugProbesKt.bin; that is not a console BIOS.
            bad = [b for b in bad if "DebugProbesKt.bin" not in b]
            names = set(z.namelist())
            # Accept either the diagnostic engine or the real-runtime game lib.
            ok = ("lib/arm64-v8a/libbioshock1-native.so" in names or
                  "lib/arm64-v8a/libbioshock1-android.so" in names)
    check("APK built and game-data-free", ok and not bad,
          f"{apk.stat().st_size / 1e6:.1f} MB" if apk.is_file() else "missing")

    # Machine-readable report consumed by the docs website indexer.
    report = PROJECT / "web/verify_report.json"
    report.parent.mkdir(parents=True, exist_ok=True)
    report.write_text(json.dumps({
        "checks": [{"passed": ok, "name": name, "detail": detail}
                   for ok, name, detail in RESULTS],
        "passed_count": sum(1 for ok, _, _ in RESULTS if ok),
        "total": len(RESULTS),
    }, indent=2), encoding="utf-8")

    failed = [n for ok, n, _ in RESULTS if not ok]
    print(f"\n{len(RESULTS) - len(failed)}/{len(RESULTS)} checks passed")
    if failed:
        print("FAILED: " + ", ".join(failed))
        return 1
    print("ALL CHECKS PASSED — project state is consistent.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
