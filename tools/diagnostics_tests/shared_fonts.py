#!/usr/bin/env python3
"""Regression tests for startup font assets and the existing APK publication gate."""
from pathlib import Path
import io
import re
import struct
import sys
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from check_shared_fonts import FONT_NAMES, verify_directory, verify_fonts, verify_font, SHARED_MEMORY_SIZE
from check_release_apk import verify_container


class SharedFontTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.font_dir = ROOT / 'app/src/main/assets/fonts'
        cls.fonts = {name: (cls.font_dir / name).read_bytes() for name in FONT_NAMES}

    def apk(self, fonts):
        result = io.BytesIO()
        with zipfile.ZipFile(result, 'w') as archive:
            # Container fixtures only: these are not runnable/signed APKs.
            for name in ['AndroidManifest.xml', 'classes.dex', 'lib/arm64-v8a/libskyline.so']:
                archive.writestr(name, b'fixture')
            for name, data in fonts.items():
                archive.writestr('assets/fonts/' + name, data)
        result.seek(0)
        return result

    def test_actual_source_fonts(self):
        self.assertEqual(verify_directory(self.font_dir), 17367028)

    def test_names_match_native_loader(self):
        source = (ROOT / 'app/src/main/cpp/skyline/services/pl/shared_font_core.h').read_text()
        self.assertEqual(tuple(re.findall(r'\{"(Font[^"]+\.ttf)"\}', source)), FONT_NAMES)

    def test_complete_apk_container(self):
        verify_container(self.apk(self.fonts))

    def test_reported_build_without_three_fonts(self):
        # Missing in fa96b1d6712fb68b17893aeffd43279bb84ea208.
        absent = {'FontStandard.ttf', 'FontChineseSimplified.ttf', 'FontChineseTraditional.ttf'}
        with self.assertRaisesRegex(ValueError, 'Missing shared font: FontStandard.ttf'):
            verify_container(self.apk({n: d for n, d in self.fonts.items() if n not in absent}))

    def test_each_required_font_missing(self):
        for name in FONT_NAMES:
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, name):
                verify_container(self.apk({n: d for n, d in self.fonts.items() if n != name}))

    def test_lfs_pointer(self):
        with self.assertRaisesRegex(ValueError, 'SFNT'):
            verify_font(b'version https://git-lfs.github.com/spec/v1\noid sha256:000\n', 'font.ttf')

    def test_empty_and_truncated_header(self):
        for data in (b'', b'\0\1\0\0', self.fonts['FontStandard.ttf'][:13]):
            with self.assertRaises(ValueError):
                verify_font(data, 'font.ttf')

    def test_truncated_table_data(self):
        with self.assertRaisesRegex(ValueError, 'out-of-bounds'):
            verify_font(self.fonts['FontStandard.ttf'][:-1000], 'font.ttf')

    def test_corrupt_table_offset(self):
        data = bytearray(self.fonts['FontStandard.ttf'])
        struct.pack_into('>I', data, 20, len(data) + 1)
        with self.assertRaisesRegex(ValueError, 'out-of-bounds'):
            verify_font(data, 'font.ttf')

    def test_shared_memory_budget(self):
        # Padding is legal SFNT data, but the complete set must fit native shared memory.
        big = self.fonts['FontStandard.ttf'] + bytes(SHARED_MEMORY_SIZE)
        with self.assertRaisesRegex(ValueError, 'only .* bytes are available'):
            verify_fonts(lambda _: big)


if __name__ == '__main__':
    unittest.main()
