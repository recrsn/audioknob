#!/usr/bin/env python3
"""Host Talk protocol checks; needs clang."""
from pathlib import Path
import os, subprocess, tempfile, socket
ROOT=Path(__file__).resolve().parents[2]
SRC=ROOT/'firmware/s3/src'
HERE=Path(__file__).resolve().parent

def run(args, **kwargs):
    subprocess.run([str(x) for x in args], check=True, **kwargs)

with tempfile.TemporaryDirectory(prefix='audioknob-lan-check-') as tmp:
    out=Path(tmp)
    stubs=out/'include'
    for name in ['esp_heap_caps.h','esp_log.h','esp_random.h','freertos/FreeRTOS.h','freertos/task.h','freertos/semphr.h','freertos/idf_additions.h','lwip/sockets.h','lwip/inet.h']:
        p=stubs/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text('#include "runtime.h"\n')
    common=['clang','-Wno-deprecated-declarations','-g','-O1','-fsanitize=address,undefined','-fno-omit-frame-pointer','-pthread',f'-I{HERE}',f'-I{stubs}',f'-I{SRC}',f'-I{ROOT}/firmware/lib/common/src']
    # protocol.h is in lib/common root on older checkouts.
    common.extend([f'-I{ROOT}/firmware/lib/common'])
    run(common+[HERE/'wire_check.c',SRC/'talk/wire.c','-o',out/'wire'])
    env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0')
    run([out/'wire'],env=env)
    # Compile the existing RAOP RTP-Info parser, without its unrelated RSA dependencies.
    source=(SRC/'airplay/shared/raop_setup.c').read_text()
    start=source.index('void raop_parse_rtp_info(')
    end=source.index('\n}',start)+2
    (out/'rtp_info.c').write_text('#include <stdio.h>\n#include <stdint.h>\n#include <stdlib.h>\n#include <string.h>\n'+source[start:end]+'\n')
    with socket.socket() as sock:
        sock.bind(('127.0.0.1',0)); rtsp_port=sock.getsockname()[1]
    run(common+[f'-DTALK_RTSP_PORT={rtsp_port}',HERE/'rtsp_check.c',SRC/'talk/rtsp.c',SRC/'airplay/shared/http_util.c',out/'rtp_info.c','-o',out/'rtsp'])
    run([out/'rtsp'],env=env)
    (stubs/'driver').mkdir(exist_ok=True)
    (stubs/'driver/gpio.h').write_text('#pragma once\n#include "runtime.h"\n#define GPIO_NUM_0 0\n#define GPIO_MODE_OUTPUT 1\ntypedef struct {uint64_t pin_bit_mask;int mode;} gpio_config_t;\nstatic int gpio_level;\nstatic int gpio_config(const gpio_config_t *c){(void)c;return 0;}\nstatic void gpio_set_level(int pin,int level){(void)pin;gpio_level=level;}\n')
    run(common+['-DHOST_NO_TASKS',HERE/'source_check.c','-o',out/'sources'])
    run([out/'sources'],env=env)
    run(common+['-ffunction-sections','-Wl,-dead_strip',HERE/'volume_check.c','-o',out/'volume'])
    run([out/'volume'],env=env)

print('All LAN voice host checks passed')
