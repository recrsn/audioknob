#!/usr/bin/env python3
"""Exercise the real sound workers with pthread FreeRTOS and serialized audio stubs."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
SRC = ROOT / 'firmware/s3/src'
with tempfile.TemporaryDirectory(prefix='audioknob-sound-') as tmp:
    out = Path(tmp)
    for name in ('esp_heap_caps.h', 'esp_log.h', 'esp_random.h', 'esp_timer.h',
                 'esp_err.h', 'freertos/FreeRTOS.h', 'freertos/semphr.h', 'freertos/task.h'):
        stub = out / name
        stub.parent.mkdir(parents=True, exist_ok=True)
        stub.write_text('#include "runtime.h"\n')
    common = ['clang', '-std=gnu11', '-g', '-O1', '-pthread', '-fsanitize=address,undefined',
              '-fno-omit-frame-pointer', f'-I{HERE}', f'-I{out}',
              f'-I{SRC}']
    env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0')  # module-lifetime mutexes
    for name in ('alarm', 'focus'):
        args = common + [str(HERE / 'check.c')]
        if name == 'focus':
            args += ['-DFOCUS', str(SRC / 'ambience/synth.c')]
        subprocess.run(args + ['-o', str(out / name)], check=True)
        subprocess.run([out / name], check=True, env=env)
