#!/usr/bin/env python3
"""Run the shared worker against pthread-backed FreeRTOS under ASan/UBSan."""
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
SRC = HERE.parents[1] / 'firmware/s3/src'
with tempfile.TemporaryDirectory(prefix='audioknob-network-jobs-') as temp:
    stub = Path(temp)
    for name in ['esp_log.h', 'esp_timer.h', 'freertos/FreeRTOS.h',
                 'freertos/semphr.h', 'freertos/task.h']:
        header = stub / name
        header.parent.mkdir(parents=True, exist_ok=True)
        header.write_text('#include "runtime.h"\n')
    binary = str(stub / 'network_jobs')
    subprocess.run(['clang', '-std=gnu11', '-g', '-O1', '-pthread',
                    '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-I' + str(stub), '-I' + str(HERE), '-I' + str(SRC),
                    str(HERE / 'check.c'), '-o', binary], check=True)
    subprocess.run([binary], check=True)
