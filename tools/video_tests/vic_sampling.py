#!/usr/bin/env python3
"""Compare actual compositor output against a supplied baseline source or project ZIP.
Requires g++; no Android SDK, FFmpeg, device or game is involved.
Example: python3 tools/video_tests/vic_sampling.py --baseline-zip strato.zip
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[2]
VIDEO = ROOT / 'app/src/main/cpp/skyline/soc/host1x/video'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument('--baseline-zip', type=Path)
    group.add_argument('--baseline-source', type=Path)
    parser.add_argument('--benchmark', action='store_true')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='strato-vic-sampling-') as folder:
        work = Path(folder)
        baseline = work / 'baseline.cpp'
        if args.baseline_zip:
            with zipfile.ZipFile(args.baseline_zip) as archive:
                name = 'strato/app/src/main/cpp/skyline/soc/host1x/video/compositor.cpp'
                baseline.write_bytes(archive.read(name))
        else:
            baseline.write_bytes(args.baseline_source.read_bytes())
        for name, source in [('before', baseline), ('after', VIDEO / 'compositor.cpp')]:
            flags = ['-O2'] if args.benchmark else ['-O1', '-g', '-fsanitize=address,undefined', '-no-pie']
            subprocess.run(['g++', '-std=c++20', '-Wall', '-Wextra', '-Wconversion', '-Werror',
                            *flags, '-I' + str(VIDEO), str(Path(__file__).with_name('vic_sampling_test.cpp')),
                            str(source), '-o', str(work / name)], check=True)
            result = subprocess.run([str(work / name), str(work / (name + '.bin')),
                                     'bench' if args.benchmark else 'test'], check=True,
                                    stdout=subprocess.PIPE, text=True,
                                    env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0', 'UBSAN_OPTIONS': 'halt_on_error=1'})
            print(name + ': ' + result.stdout.strip())
        if (work / 'before.bin').read_bytes() != (work / 'after.bin').read_bytes():
            raise AssertionError('VIC output differs from baseline')
        print('PASS: complete output surfaces (including padding) are byte-identical')


if __name__ == '__main__':
    main()
