#!/usr/bin/env python3
"""Compile and execute production export code on Java 17 with Kotlin 1.9.22.
Usage: python tools/diagnostics_tests/export_batch/run.py --dependencies /path/to/jars
Use the Kotlin dependencies documented in tools/updater_tests/run.py, plus
org.json:json:20231013. No network access or Android SDK is used by this runner.
Only Context/session-root/UTC helpers are fixtures; exporter, batch, date windows
and file locking are the real production implementations. Android UI is not run.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
PRODUCTION = ROOT / "app/src/main/java/org/stratoemu/strato/diagnostics"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dependencies", type=Path, required=True)
    args = parser.parse_args()
    names = ["kotlin-compiler-embeddable-1.9.22.jar", "kotlin-stdlib-1.9.22.jar",
             "kotlin-script-runtime-1.9.22.jar", "kotlin-reflect-1.6.10.jar",
             "trove4j-1.0.20200330.jar", "annotations-13.0.jar", "json-20231013.jar"]
    jars = [args.dependencies.resolve() / name for name in names]
    for jar in jars:
        if not jar.is_file():
            raise SystemExit(f"Missing compiler dependency: {jar}")
    cp = os.pathsep.join(map(str, jars))
    with tempfile.TemporaryDirectory(prefix="strato-export-compile-") as temp:
        work = Path(temp)
        context = work / "Context.kt"
        context.write_text('package android.content\nimport java.io.File\n'
                           'class Context(val filesDir : File, val cacheDir : File)\n')
        session = (PRODUCTION / "DiagnosticSession.kt").read_text()
        lock = session[session.index("internal class DiagnosticFileLock"):session.index("/** Keep the latest two chunks")]
        support = work / "SessionSupport.kt"
        support.write_text('''package org.stratoemu.strato.diagnostics
import android.content.Context
import java.io.*
import java.nio.channels.*
import java.nio.file.Files
import java.util.concurrent.atomic.AtomicBoolean
import org.json.JSONObject
internal object DiagnosticSession {
    fun sessionRoot(context : Context) = File(context.filesDir, "diagnostics/sessions")
}
internal fun utcNow() = java.time.Instant.now().toString()
internal fun unavailable(reason : String) = JSONObject().put("status", "unavailable").put("reason", reason)
''' + lock)
        sources = [PRODUCTION / name for name in ["DiagnosticExporter.kt", "DiagnosticBatchArchive.kt", "DiagnosticExportWindow.kt"]]
        sources += [context, support, Path(__file__).with_name("ExportBatchTests.kt")]
        classes = work / "classes"
        subprocess.run(["java", "-cp", cp, "org.jetbrains.kotlin.cli.jvm.K2JVMCompiler",
                        "-no-stdlib", "-no-reflect", "-jvm-target", "17", "-classpath", cp,
                        "-d", str(classes), *map(str, sources)], check=True)
        subprocess.run(["java", "-cp", str(classes) + os.pathsep + cp,
                        "org.stratoemu.strato.diagnostics.ExportBatchTestsKt"], check=True, timeout=45)


if __name__ == "__main__":
    main()
