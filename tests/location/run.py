#!/usr/bin/env python3
"""Exercise the real online worker with a deterministic clock and NVS failures."""
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
NET = HERE.parents[1] / 'firmware/s3/src/device/net'
with tempfile.TemporaryDirectory(prefix='audioknob-location-') as temp:
    stub = Path(temp)
    (stub / 'freertos').mkdir()
    for header in ['ui.h', 'cJSON.h', 'esp_heap_caps.h', 'esp_log.h', 'nvs.h',
                   'settings.h', 'esp_timer.h', 'freertos/FreeRTOS.h', 'freertos/semphr.h', 'freertos/task.h']:
        (stub / header).write_text('#include "mock_sdk.h"\n')
    binary = str(stub / 'location')
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-I' + str(stub), '-I' + str(HERE),
                    '-I' + str(NET), str(HERE / 'test_location.c'), '-o', binary], check=True)
    subprocess.run([binary], check=True)
subprocess.run(['node', str(HERE / 'test_web.js')], check=True)
