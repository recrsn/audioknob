#!/usr/bin/env python3
"""Exercise GPIO edges through the production driver and real LVGL encoder."""
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
S3 = ROOT / 'firmware/s3'
subprocess.run(['pio', 'run', '-d', str(S3), '-e', 'native'], check=True,
               stdout=subprocess.DEVNULL)
lvgl_libraries = list((S3 / '.pio/build/native').glob('lib*/liblvgl.a'))
if len(lvgl_libraries) != 1:
    raise RuntimeError(f'Expected one built LVGL library, found {lvgl_libraries}')
with tempfile.TemporaryDirectory(prefix='audioknob-encoder-') as temp:
    stub = Path(temp)
    for name in ['driver/gpio.h', 'esp_timer.h', 'freertos/FreeRTOS.h']:
        header = stub / name
        header.parent.mkdir(parents=True, exist_ok=True)
        header.write_text('#include "runtime.h"\n')
    binary = stub / 'check'
    subprocess.run(['clang', '-std=gnu11', '-g', '-O1', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-DAK_NATIVE', '-DLV_CONF_INCLUDE_SIMPLE',
                    '-I' + str(stub), '-I' + str(HERE), '-I' + str(S3 / 'include'),
                    '-I' + str(S3 / '.pio/libdeps/native/lvgl'), '-I/opt/homebrew/include',
                    str(HERE / 'check.c'), str(lvgl_libraries[0]),
                    '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=30)
