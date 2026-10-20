#!/usr/bin/env python3
"""Check independent firmware versioning and release checksums."""
import contextlib
import hashlib
import importlib.util
import io
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
import sys
from unittest.mock import patch

sys.dont_write_bytecode = True
SPEC = importlib.util.spec_from_file_location('release_firmware', Path(__file__).resolve().parents[2] / 'tools/release_firmware.py')
release = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(release)


def published(tag, date, label=None, draft=False, prerelease=False, bluetooth=True):
    return dict(tag_name=tag, published_at=date, draft=draft, prerelease=prerelease,
                assets=[dict(name='firmware-esp32.bin', label=label)] if bluetooth else [])


def image(version, chip):
    data = bytearray(320)
    data[0] = 0xe9
    data[23] = 1
    struct.pack_into('<H', data, 12, chip)
    struct.pack_into('<I', data, 32, 0xabcd5432)
    data[48:80] = version.encode().ljust(32, b'\0')
    data[-32:] = hashlib.sha256(data[:-32]).digest()
    return data


class ReleaseTests(unittest.TestCase):
    def test_reused_image_keeps_its_original_version(self):
        releases = [published('v0.3.0', '3'), published('v0.2.0', '2', 'version:v0.1.0'), published('v0.1.0', '1')]
        self.assertEqual(release.previous_bluetooth_release(releases, 'v0.3.0')[:2], ('v0.2.0', 'v0.1.0'))
        # Re-running an older release never selects a later release as its baseline.
        self.assertEqual(release.previous_bluetooth_release(releases, 'v0.2.0')[:2], ('v0.1.0', 'v0.1.0'))

    def test_optional_assets_and_unpublished_releases(self):
        releases = [published('v0.4.0', '4'), published('v0.3.0', '3', bluetooth=False),
                    published('v0.3.0-rc.1', '3', prerelease=True), published('v0.2.1', '2', draft=True),
                    published('v0.1.0', '1')]
        self.assertEqual(release.previous_bluetooth_release(releases, 'v0.4.0')[:2], ('v0.1.0', 'v0.1.0'))
        self.assertIsNone(release.previous_bluetooth_release(releases[-1:], 'v0.1.0'))

    def test_changes_in_each_firmware_tree(self):
        with tempfile.TemporaryDirectory() as temp, contextlib.chdir(temp):
            def git(*args):
                subprocess.run(['git', *args], check=True, stdout=subprocess.DEVNULL)
            git('init', '-q')
            git('config', 'commit.gpgsign', 'false')
            git('config', 'tag.gpgsign', 'false')
            git('config', 'user.email', 'test@example.invalid')
            git('config', 'user.name', 'Release test')
            for path in ('firmware/s3/main.c', 'firmware/esp32/main.c', 'firmware/lib/common/protocol.h'):
                Path(path).parent.mkdir(parents=True, exist_ok=True)
                Path(path).write_text('original\n')
            git('add', '.')
            git('commit', '-qm', 'baseline')
            git('tag', 'v0.1.0')
            self.assertTrue(release.bluetooth_changed(None))
            self.assertFalse(release.bluetooth_changed('v0.1.0'))
            Path('firmware/s3/main.c').write_text('main change\n')
            git('commit', '-qam', 'main only')
            self.assertFalse(release.bluetooth_changed('v0.1.0'))
            Path('firmware/lib/common/protocol.h').write_text('protocol change\n')
            git('commit', '-qam', 'protocol')
            self.assertTrue(release.bluetooth_changed('v0.1.0'))
            git('tag', 'v0.2.0')
            Path('firmware/esp32/main.c').write_text('bluetooth change\n')
            git('commit', '-qam', 'bluetooth')
            self.assertTrue(release.bluetooth_changed('v0.2.0'))

    def test_package_preserves_bluetooth_and_writes_checksums(self):
        with tempfile.TemporaryDirectory() as temp, contextlib.chdir(temp):
            source = Path('firmware/s3/.pio/build/s3/firmware.bin')
            source.parent.mkdir(parents=True)
            source.write_bytes(image('v0.2.0', 9))
            Path('output').mkdir()
            reused = image('v0.1.0', 0)
            Path('output/firmware-esp32.bin').write_bytes(reused)
            with patch.dict(os.environ, RELEASE_TAG='v0.2.0', BLUETOOTH_VERSION='v0.1.0',
                            BLUETOOTH_CHANGED='false', RELEASE_DIR='output'), contextlib.redirect_stdout(io.StringIO()):
                release.package()
            self.assertEqual(Path('output/firmware-esp32.bin').read_bytes(), reused)
            sums = Path('output/SHA256SUMS').read_text().splitlines()
            self.assertEqual(len(sums), 2)
            for line in sums:
                digest, name = line.split('  ')
                self.assertEqual(digest, hashlib.sha256((Path('output') / name).read_bytes()).hexdigest())

    def test_image_and_version_validation(self):
        data = image('v0.1.0', 0)
        release.verify_image(data, 0, 1024, 'v0.1.0')
        for chip, limit, version in [(9, 1024, 'v0.1.0'), (0, 319, 'v0.1.0'), (0, 1024, 'v0.2.0')]:
            with self.assertRaises(ValueError):
                release.verify_image(data, chip, limit, version)
        data[100] ^= 1
        with self.assertRaisesRegex(ValueError, 'checksum'):
            release.verify_image(data, 0, 1024, 'v0.1.0')
        for tag in ('v1.2.3\ninjected', 'latest', 'v1.2.3-' + 'x' * 32):
            with self.assertRaises(ValueError):
                release.validate_version(tag)


if __name__ == '__main__':
    unittest.main()
