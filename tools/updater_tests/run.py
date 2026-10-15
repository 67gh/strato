#!/usr/bin/env python3
"""Run transfer tests with Kotlin 1.9.22 compiler jars (Java 17 required).
Usage: python tools/updater_tests/run.py --dependencies /path/to/kotlin-jars
The directory must contain kotlin-compiler-embeddable, kotlin-stdlib,
kotlin-script-runtime (1.9.22), kotlin-reflect (1.6.10), trove4j (1.0.20200330)
and annotations (13.0). Tests use local HTTP only; no Android SDK needed.
"""
from pathlib import Path
import argparse, subprocess, tempfile
ROOT = Path(__file__).resolve().parents[2]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dependencies", type=Path, required=True)
    args = parser.parse_args()
    names = ["kotlin-compiler-embeddable-1.9.22.jar", "kotlin-stdlib-1.9.22.jar",
             "kotlin-script-runtime-1.9.22.jar", "kotlin-reflect-1.6.10.jar",
             "trove4j-1.0.20200330.jar", "annotations-13.0.jar"]
    jars = [args.dependencies.resolve() / name for name in names]
    for jar in jars:
        if not jar.is_file(): raise SystemExit("Missing compiler dependency: " + str(jar))
    import os
    cp = os.pathsep.join(map(str, jars))
    production = ROOT / "app/src/main/java/org/stratoemu/strato/updater"
    with tempfile.TemporaryDirectory(prefix="strato-updater-tests-") as folder:
        classes = Path(folder) / "classes"
        subprocess.run(["java", "-cp", cp, "org.jetbrains.kotlin.cli.jvm.K2JVMCompiler",
                        "-no-stdlib", "-no-reflect", "-jvm-target", "17", "-classpath", cp,
                        "-d", str(classes), str(production / "UpdateFiles.kt"),
                        str(production / "APKDownloader.kt"),
                        str(Path(__file__).with_name("UpdaterTests.kt"))], check=True)
        subprocess.run(["java", "-cp", str(classes) + os.pathsep + cp,
                        "org.stratoemu.strato.updater.UpdaterTestsKt"], check=True, timeout=45)

if __name__ == "__main__": main()
