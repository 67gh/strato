#!/usr/bin/env python3
"""Reject broken or wrapped release artifacts before upload/publication.
Requires Android SDK build-tools (aapt, apksigner). Does not sign or alter APKs.
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
import zipfile
from check_shared_fonts import verify_archive_fonts


def verify_container(apk):
    with zipfile.ZipFile(apk) as archive:
        names = archive.namelist()
        if len(names) != len(set(names)):
            raise ValueError('Duplicate APK entries')
        for name in ['AndroidManifest.xml', 'classes.dex', 'lib/arm64-v8a/libskyline.so']:
            if name not in names or archive.getinfo(name).file_size == 0:
                raise ValueError(f'Not a complete Strato APK: missing {name}')
        # FontStandard is opened during ServiceManager construction, before the
        # first guest instruction. An otherwise installable APK can still fail here.
        verify_archive_fonts(archive)
        bad = archive.testzip()
        if bad is not None:
            raise ValueError(f'Corrupt APK entry: {bad}')


def build_tools():
    sdk = os.environ.get('ANDROID_HOME') or os.environ.get('ANDROID_SDK_ROOT')
    if not sdk:
        raise RuntimeError('ANDROID_HOME or ANDROID_SDK_ROOT is required')
    candidates = [p for p in (Path(sdk) / 'build-tools').iterdir()
                  if (p / 'aapt').is_file() and (p / 'apksigner').is_file()]
    if not candidates:
        raise RuntimeError('Android build-tools aapt/apksigner missing')
    return max(candidates, key=lambda p: tuple(map(int, re.findall(r'\d+', p.name))))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('apk', type=Path)
    args = parser.parse_args()
    apk = args.apk.resolve()
    verify_container(apk)
    tools = build_tools()
    result = subprocess.run([str(tools / 'aapt'), 'dump', 'badging', str(apk)],
                            check=True, capture_output=True, text=True, timeout=60)
    package = re.search(r"^package: name='([^']+)' versionCode='(\d+)'", result.stdout, re.M)
    if not package or package.group(1) != 'org.stratoemu.strato':
        raise ValueError('Wrong APK package: expected mainline org.stratoemu.strato')
    subprocess.run([str(tools / 'apksigner'), 'verify', '--verbose', str(apk)],
                   check=True, timeout=120)
    print(f'PASS APK: {package.group(1)} versionCode={package.group(2)} bytes={apk.stat().st_size}')


if __name__ == '__main__':
    main()
