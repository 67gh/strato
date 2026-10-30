#!/usr/bin/env python3
"""Check the shared fonts needed before any game can start.

Usage: python3 tools/check_shared_fonts.py app/src/main/assets/fonts
       python3 tools/check_shared_fonts.py path/to/app.apk
Only uses the Python standard library; never modifies fonts or APKs.
"""
import argparse
from pathlib import Path
import struct
import zipfile

FONT_NAMES = (
    "FontStandard.ttf", "FontChineseSimplified.ttf",
    "FontExtendedChineseSimplified.ttf", "FontChineseTraditional.ttf",
    "FontKorean.ttf", "FontNintendoExtended.ttf",
)
SHARED_MEMORY_SIZE = 0x140A000


def verify_font(data, name):
    """Reject missing/truncated fonts and Git LFS pointers, without parsing glyphs."""
    if len(data) < 12 or data[:4] not in (b"\x00\x01\x00\x00", b"OTTO", b"true"):
        raise ValueError(f"{name}: invalid SFNT font header (or an unmaterialized Git LFS pointer)")
    count = struct.unpack_from(">H", data, 4)[0]
    if not count or 12 + count * 16 > len(data):
        raise ValueError(f"{name}: truncated font table directory")
    tags = set()
    for index in range(count):
        tag, _, offset, length = struct.unpack_from(">4sIII", data, 12 + index * 16)
        if tag in tags or offset > len(data) or length > len(data) - offset:
            raise ValueError(f"{name}: duplicate or out-of-bounds font table {tag!r}")
        tags.add(tag)
    if not {b"cmap", b"head", b"maxp", b"name"} <= tags:
        raise ValueError(f"{name}: required font tables are missing")


def verify_fonts(read_font):
    total = 0
    for name in FONT_NAMES:
        try:
            data = read_font(name)
        except (FileNotFoundError, KeyError) as error:
            raise ValueError(f"Missing shared font: {name}. Restore app/src/main/assets/fonts before building.") from error
        verify_font(data, name)
        total += 8 + len(data)  # The shared memory stores two u32 header words per font.
    if total > SHARED_MEMORY_SIZE:
        raise ValueError(f"Shared fonts need {total} bytes; only {SHARED_MEMORY_SIZE} bytes are available")
    return total


def verify_archive_fonts(archive):
    def read_font(name):
        path = "assets/fonts/" + name
        info = archive.getinfo(path)
        if info.file_size > SHARED_MEMORY_SIZE:
            raise ValueError(f"{path}: font exceeds shared memory capacity")
        return archive.read(info)
    return verify_fonts(read_font)


def verify_directory(directory):
    def read_font(name):
        path = Path(directory) / name
        if path.stat().st_size > SHARED_MEMORY_SIZE:
            raise ValueError(f"{path}: font exceeds shared memory capacity")
        return path.read_bytes()
    return verify_fonts(read_font)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("path", type=Path, help="Font source directory or built APK")
    args = parser.parse_args()
    try:
        if args.path.is_dir():
            total = verify_directory(args.path)
        else:
            with zipfile.ZipFile(args.path) as archive:
                total = verify_archive_fonts(archive)
    except (ValueError, OSError, zipfile.BadZipFile) as error:
        parser.exit(1, f"ERROR: {error}\n")
    print(f"PASS: all {len(FONT_NAMES)} shared fonts present and structurally valid ({total} bytes)")


if __name__ == "__main__":
    main()
