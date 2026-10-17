#!/usr/bin/env python3
"""Check S3 geometry and (when present) built images against the new layout."""
import csv
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

def number(value):
    value = value.strip().upper()
    return int(value[:-1], 0) * {'K': 1024, 'M': 1048576}[value[-1]] if value[-1] in 'KM' else int(value, 0)

parts = {}
end = 0x9000  # bootloader and partition table reserved
for row in csv.reader((ROOT / 'firmware/s3/partitions_s3.csv').read_text().splitlines()):
    if not row or row[0].startswith('#'):
        continue
    name, kind, subtype, offset, size = (cell.strip() for cell in row[:5])
    offset, size = number(offset), number(size)
    assert name not in parts
    assert offset >= end, f'{name} overlaps previous partition'
    assert offset % (0x10000 if kind == 'app' else 0x1000) == 0, name
    assert size > 0 and size % 0x1000 == 0, name
    end = offset + size
    assert end <= 16 * 1048576, name
    parts[name] = (offset, size)
assert parts['ota_0'] == (0x20000, 4 * 1048576)
assert parts['ota_1'] == (0x420000, 4 * 1048576)
assert parts['esp32_fw'] == (0x820000, 1048576)
assert parts['user_data'] == (0x930000, 0x6d0000)
assert end == 16 * 1048576
for chip, env, limit in [('s3', 's3', parts['ota_0'][1]), ('esp32', 'esp32', parts['esp32_fw'][1])]:
    image = ROOT / f'firmware/{chip}/.pio/build/{env}/firmware.bin'
    if image.exists():
        size = image.stat().st_size
        assert size <= limit, f'{chip} image exceeds partition: {size} > {limit}'
        print(f'{chip}: {size:,} / {limit:,} bytes ({size / limit:.1%})')
print('S3 partition alignment, bounds and image sizes passed')
