#!/usr/bin/env python3
"""Test production payload handling; synthetic containers, not Android installation."""
from pathlib import Path
import argparse, os, subprocess, tempfile
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--dependencies', type=Path, required=True)
a = p.parse_args()
root = Path(__file__).resolve().parents[2]
source = root / 'app/src/main/java/org/stratoemu/strato/updater'
names = ['kotlin-compiler-embeddable-1.9.22.jar', 'kotlin-stdlib-1.9.22.jar',
         'kotlin-script-runtime-1.9.22.jar', 'kotlin-reflect-1.6.10.jar',
         'trove4j-1.0.20200330.jar', 'annotations-13.0.jar']
jars = [a.dependencies.resolve() / name for name in names]
for jar in jars:
    if not jar.is_file(): raise SystemExit(f'Missing: {jar}')
cp = os.pathsep.join(map(str, jars))
with tempfile.TemporaryDirectory(prefix='strato-payload-') as out:
    subprocess.run(['java', '-cp', cp, 'org.jetbrains.kotlin.cli.jvm.K2JVMCompiler',
                    '-no-stdlib', '-no-reflect', '-jvm-target', '17', '-classpath', cp,
                    '-d', out, str(source / 'UpdateFiles.kt'), str(source / 'APKPayload.kt'),
                    str(Path(__file__).with_name('PayloadTests.kt'))], check=True, timeout=60)
    subprocess.run(['java', '-cp', out + os.pathsep + cp,
                    'org.stratoemu.strato.updater.PayloadTestsKt'], check=True, timeout=30)
