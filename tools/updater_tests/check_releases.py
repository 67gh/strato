#!/usr/bin/env python3
"""Run production release selection offline. Requires the Kotlin jars from run.py
plus android-all-14-robolectric-10818077.jar (Android JSON/API classes).
Usage: python tools/updater_tests/check_releases.py --dependencies DIR
"""
from pathlib import Path
import argparse, os, subprocess, tempfile
ROOT = Path(__file__).resolve().parents[2]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--dependencies', type=Path, required=True)
args = p.parse_args()
names = ['kotlin-compiler-embeddable-1.9.22.jar','kotlin-stdlib-1.9.22.jar',
         'kotlin-script-runtime-1.9.22.jar','kotlin-reflect-1.6.10.jar',
         'trove4j-1.0.20200330.jar','annotations-13.0.jar',
         'android-all-14-robolectric-10818077.jar']
jars = [args.dependencies.resolve()/n for n in names]
for jar in jars:
    if not jar.is_file(): raise SystemExit('Missing dependency: '+str(jar))
cp = os.pathsep.join(map(str,jars))
with tempfile.TemporaryDirectory(prefix='strato-release-selection-') as d:
    subprocess.run(['java','-cp',cp,'org.jetbrains.kotlin.cli.jvm.K2JVMCompiler',
                    '-no-stdlib','-no-reflect','-jvm-target','17','-classpath',cp,'-d',d,
                    str(ROOT/'app/src/main/java/org/stratoemu/strato/UpdateChecker.kt'),
                    str(Path(__file__).with_name('ReleaseSelectionTests.kt'))],check=True)
    subprocess.run(['java','-cp',d+os.pathsep+cp,'org.stratoemu.strato.ReleaseSelectionTestsKt'],check=True,timeout=30)
