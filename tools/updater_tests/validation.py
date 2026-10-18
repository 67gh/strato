#!/usr/bin/env python3
"""Test production APK compatibility rules on the JVM (not Android's APK parser).
Usage: python tools/updater_tests/validation.py --dependencies DIR
Uses the Kotlin jars already required by run.py. No network or installation.
"""
from pathlib import Path
import argparse, os, subprocess, tempfile

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--dependencies', type=Path, required=True)
args = parser.parse_args()
names = ['kotlin-compiler-embeddable-1.9.22.jar', 'kotlin-stdlib-1.9.22.jar',
         'kotlin-script-runtime-1.9.22.jar', 'kotlin-reflect-1.6.10.jar',
         'trove4j-1.0.20200330.jar', 'annotations-13.0.jar']
jars = [args.dependencies.resolve() / name for name in names]
for jar in jars:
    if not jar.is_file():
        raise SystemExit('Missing dependency: ' + str(jar))
cp = os.pathsep.join(map(str, jars))
with tempfile.TemporaryDirectory(prefix='strato-apk-validation-') as output:
    subprocess.run(['java', '-cp', cp, 'org.jetbrains.kotlin.cli.jvm.K2JVMCompiler',
                    '-no-stdlib', '-no-reflect', '-jvm-target', '17', '-classpath', cp,
                    '-d', output,
                    str(ROOT / 'app/src/main/java/org/stratoemu/strato/updater/APKValidation.kt'),
                    str(Path(__file__).with_name('ValidationTests.kt'))], check=True, timeout=60)
    subprocess.run(['java', '-cp', output + os.pathsep + cp,
                    'org.stratoemu.strato.updater.ValidationTestsKt'], check=True, timeout=30)
