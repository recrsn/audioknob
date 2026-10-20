#!/usr/bin/env python3
"""Host tests using the same cJSON sources as the ESP-IDF build."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
IDF = Path(os.environ.get('IDF_PATH', Path.home() / '.platformio/packages/framework-espidf'))
CJSON = IDF / 'components/json/cJSON'
NET = ROOT / 'firmware/s3/src/device/net'
with tempfile.TemporaryDirectory(prefix='audioknob-ota-') as temp:
    binary = str(Path(temp) / 'release_manifest')
    subprocess.run([
        'cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-deprecated-declarations', '-fsanitize=address,undefined',
        '-I' + str(NET), '-I' + str(CJSON), str(NET / 'release_manifest.c'),
        str(CJSON / 'cJSON.c'), str(Path(__file__).with_name('test_release_manifest.c')),
        '-lm', '-o', binary,
    ], check=True)
    subprocess.run([binary], check=True)
    stubs = Path(temp) / 'stubs'
    (stubs / 'freertos').mkdir(parents=True)
    headers = ['audio_source.h', 'esp_app_desc.h', 'esp_app_format.h', 'esp_heap_caps.h',
               'esp_log.h', 'esp_ota_ops.h', 'esp_system.h', 'esp_http_client.h',
               'freertos/FreeRTOS.h', 'freertos/task.h']
    for header in headers:
        (stubs / header).write_text('#include "mock_sdk.h"\n')
    binary = str(Path(temp) / 'ota')
    subprocess.run([
        'cc', '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-Wall', '-Wextra', '-Werror',
        '-fsanitize=address,undefined', '-I' + str(stubs), '-I' + str(NET),
        '-I' + str(Path(__file__).parent), str(NET / 'ota.c'),
        str(Path(__file__).with_name('test_ota.c')), '-o', binary,
    ], check=True)
    subprocess.run([binary], check=True)
    for header in ['esp_timer.h', 'uart_link.h']:
        (stubs / header).write_text('#include "mock_sdk.h"\n')
    (stubs / 'sdkconfig.h').write_text('#define CONFIG_AK_UPDATE_REPOSITORY "recrsn/audioknob"\n')
    binary = str(Path(temp) / 'github_release')
    subprocess.run([
        'cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-deprecated-declarations',
        '-fsanitize=address,undefined', '-I' + str(stubs), '-I' + str(NET),
        '-I' + str(CJSON), '-I' + str(Path(__file__).parent),
        str(NET / 'release_manifest.c'), str(CJSON / 'cJSON.c'),
        str(Path(__file__).with_name('test_github_release.c')), '-lm', '-o', binary,
    ], check=True)
    subprocess.run([binary], check=True)
subprocess.run(['node', str(Path(__file__).with_name('test_web.js'))], check=True)
