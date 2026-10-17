#!/usr/bin/env python3
"""Exercise local SDK configuration migration with the real CMake interpreter."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

MODULE = Path(__file__).resolve().parents[1] / 'lvgl_memory.cmake'
CMAKE = os.environ.get('CMAKE_COMMAND', 'cmake')


class MemoryConfigTest(unittest.TestCase):
    def migrate(self, path):
        return subprocess.run([CMAKE, f'-DSDKCONFIG={path}', '-P', str(MODULE)],
                              check=True, text=True, capture_output=True)

    def test_existing_fixed_pool_and_local_settings(self):
        for newline in ['\n', '\r\n']:
            with self.subTest(newline=repr(newline)), tempfile.TemporaryDirectory() as folder:
                path = Path(folder) / 'custom config'
                lines = ['CONFIG_IDF_TARGET="esp32s3"', 'CONFIG_LV_USE_BUILTIN_MALLOC=y',
                         '# CONFIG_LV_USE_CLIB_MALLOC is not set',
                         'CONFIG_LV_MEM_SIZE_KILOBYTES=64', 'CONFIG_LV_USE_ASSERT_MALLOC=y',
                         'CONFIG_LOCAL_SETTING="keep this"']
                path.write_bytes((newline.join(lines) + newline).encode())
                result = self.migrate(path)
                migrated = path.read_text()
                self.assertIn('migrated LVGL fixed pool', result.stdout)
                self.assertNotIn('\nCONFIG_LV_USE_BUILTIN_MALLOC=y\n', migrated)
                self.assertIn('CONFIG_LV_USE_CLIB_MALLOC=y', migrated)
                for value in [lines[0], lines[3], lines[4], lines[5]]:
                    self.assertIn(value, migrated)
                before = path.read_bytes()
                self.migrate(path)
                self.assertEqual(before, path.read_bytes())

    def test_missing_clib_choice_is_added(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'sdkconfig'
            path.write_text('CONFIG_LV_USE_BUILTIN_MALLOC=y')
            self.migrate(path)
            self.assertIn('CONFIG_LV_USE_CLIB_MALLOC=y', path.read_text())

    def test_new_checkout_and_existing_system_allocator(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'sdkconfig'
            self.migrate(path)
            self.assertFalse(path.exists())  # New builds use sdkconfig.defaults.
            for text in ['CONFIG_LV_USE_CLIB_MALLOC=y\n',
                         'CONFIG_LV_USE_CUSTOM_MALLOC=y\n']:
                path.write_text(text)
                before = path.read_bytes()
                self.migrate(path)
                self.assertEqual(before, path.read_bytes())


if __name__ == '__main__':
    unittest.main()
