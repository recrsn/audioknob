#!/usr/bin/env python3
"""Prepare and package independently versioned release firmware."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess

BLUETOOTH_PATHS = ('firmware/esp32', 'firmware/lib/common')
VERSION_LABEL = 'version:'


def validate_version(version):
    if not re.fullmatch(r'v?[0-9]+\.[0-9]+\.[0-9]+(?:-[0-9A-Za-z.-]+)?', version):
        raise ValueError('Use a version tag such as v1.2.3 or v1.2.3-rc.1')
    if len(version.encode('utf-8')) > 31:
        raise ValueError('The firmware version must fit its 31-byte field')
    return version


def previous_bluetooth_release(releases, tag):
    current = next((r for r in releases if r['tag_name'] == tag), {})
    published = current.get('published_at')
    candidates = sorted(
        (r for r in releases if r['tag_name'] != tag and not r['draft'] and not r['prerelease']
         and r.get('published_at') and (not published or r['published_at'] < published)),
        key=lambda r: r['published_at'], reverse=True,
    )
    for release in candidates:
        asset = next((a for a in release['assets'] if a['name'] == 'firmware-esp32.bin'), None)
        if asset:
            label = asset.get('label') or ''
            version = label[len(VERSION_LABEL):] if label.startswith(VERSION_LABEL) else release['tag_name']
            return release['tag_name'], validate_version(version), asset
    return None


def bluetooth_changed(baseline):
    if baseline is None:
        return True
    result = subprocess.run(['git', 'diff', '--quiet', f'refs/tags/{baseline}', 'HEAD', '--', *BLUETOOTH_PATHS])
    if result.returncode not in (0, 1):
        raise RuntimeError('Could not compare Bluetooth sources with the previous release')
    return result.returncode == 1


def prepare():
    tag = validate_version(os.environ['RELEASE_TAG'])
    repo = os.environ['GH_REPO']
    pages = json.loads(subprocess.check_output([
        'gh', 'api', '--paginate', '--slurp', f'repos/{repo}/releases?per_page=100',
    ]))
    releases = [release for page in pages for release in page]
    previous = previous_bluetooth_release(releases, tag)
    changed = bluetooth_changed(previous[0] if previous else None)
    bt_version = tag if changed else previous[1]
    Path('firmware/s3/version.txt').write_text(tag + '\n')
    if changed:
        Path('firmware/esp32/version.txt').write_text(tag + '\n')
    else:
        output = Path(os.environ['RELEASE_DIR'])
        output.mkdir(parents=True, exist_ok=True)
        image = output / 'firmware-esp32.bin'
        subprocess.run([
            'gh', 'release', 'download', previous[0], '--repo', repo,
            '--pattern', image.name, '--output', str(image), '--clobber',
        ], check=True)
        digest = previous[2].get('digest')
        if not isinstance(digest, str) or not re.fullmatch(r'sha256:[0-9a-fA-F]{64}', digest):
            raise ValueError('Reused Bluetooth image has no valid release checksum')
        if digest.lower() != 'sha256:' + hashlib.sha256(image.read_bytes()).hexdigest():
            raise ValueError('Reused Bluetooth image does not match its release digest')
    with Path(os.environ['GITHUB_OUTPUT']).open('a') as output:
        output.write(f'bluetooth_changed={str(changed).lower()}\nbluetooth_version={bt_version}\n')
    print(f'Bluetooth: {"build" if changed else "reuse"} {bt_version}')


def verify_image(data, chip_id, limit, version):
    if not 288 <= len(data) <= limit:
        raise ValueError('Image does not fit its OTA partition')
    if data[0] != 0xe9 or struct.unpack_from('<H', data, 12)[0] != chip_id:
        raise ValueError('Incorrect application image or chip ID')
    if struct.unpack_from('<I', data, 32)[0] != 0xabcd5432:
        raise ValueError('Missing application descriptor')
    embedded = data[48:80].split(b'\0', 1)[0].decode('utf-8')
    if embedded != version:
        raise ValueError(f'Embedded version {embedded!r} does not match {version!r}')
    if data[23] != 1 or hashlib.sha256(data[:-32]).digest() != data[-32:]:
        raise ValueError('Image checksum does not match')


def package():
    tag = validate_version(os.environ['RELEASE_TAG'])
    bt_version = validate_version(os.environ['BLUETOOTH_VERSION'])
    changed = os.environ['BLUETOOTH_CHANGED'] == 'true'
    output = Path(os.environ['RELEASE_DIR'])
    output.mkdir(parents=True, exist_ok=True)
    checksums = []
    for chip, chip_id, limit, version in (
        ('s3', 9, 4 * 1024 * 1024, tag), ('esp32', 0, 1024 * 1024, bt_version),
    ):
        target = output / f'firmware-{chip}.bin'
        source = target if chip == 'esp32' and not changed else Path(f'firmware/{chip}/.pio/build/{chip}/firmware.bin')
        data = source.read_bytes()
        verify_image(data, chip_id, limit, version)
        if source != target:
            shutil.copyfile(source, target)
        checksums.append(f'{hashlib.sha256(data).hexdigest()}  {target.name}\n')
        print(f'{target.name}: {len(data):,} bytes, version {version}')
    (output / 'SHA256SUMS').write_text(''.join(checksums))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=('prepare', 'package'))
    args = parser.parse_args()
    try:
        {'prepare': prepare, 'package': package}[args.command]()
    except (ValueError, RuntimeError) as error:
        raise SystemExit(str(error)) from error
