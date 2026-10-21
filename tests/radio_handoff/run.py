#!/usr/bin/env python3
from pathlib import Path
import subprocess,tempfile
HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[1]
SRC=ROOT/'firmware/s3/src'
with tempfile.TemporaryDirectory(prefix='audioknob-handoff-') as temp:
    stub=Path(temp)
    for name in ['esp_heap_caps.h','esp_log.h','esp_timer.h','freertos/FreeRTOS.h','freertos/semphr.h','freertos/task.h','freertos/idf_additions.h','lwip/sockets.h']:
        p=stub/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text('#include "runtime.h"\n')
    args=['clang','-std=gnu11','-g','-O1','-Wall','-Wextra','-Werror','-Wno-unused-function','-pthread','-fsanitize=address,undefined',f'-I{HERE}',f'-I{stub}',f'-I{SRC}',f'-I{ROOT}/firmware/s3/managed_components/espressif__esp_audio_codec/include',f'-I{ROOT}/firmware/s3/managed_components/espressif__esp_audio_codec/include/decoder',f'-I{ROOT}/firmware/s3/managed_components/espressif__esp_audio_codec/include/simple_dec',str(HERE/'check.c'),str(SRC/'radio/stream/ring.c'),str(SRC/'audio/pcm.c'),'-o',str(stub/'check')]
    subprocess.run(args,check=True)
    subprocess.run([str(stub/'check')],check=True,timeout=30)

    for name in ['esp_crt_bundle.h','esp_http_client.h']:
        (stub/name).write_text('#include "net_runtime.h"\n')
    net_args=args[:args.index(str(HERE/'check.c'))]+[str(HERE/'net_check.c'),str(SRC/'radio/stream/parse.c'),'-o',str(stub/'net')]
    subprocess.run(net_args,check=True)
    subprocess.run([str(stub/'net')],check=True,timeout=10)
