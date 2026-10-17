#!/usr/bin/env python3
"""Host protocol checks; needs clang, libopus and ffprobe (Homebrew on macOS)."""
from pathlib import Path
import os, subprocess, tempfile, json, socket
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
    common=['clang','-Wno-deprecated-declarations','-g','-O1','-fsanitize=address,undefined','-fno-omit-frame-pointer','-pthread',f'-I{HERE}',f'-I{stubs}',f'-I{SRC}/device/talk',f'-I{SRC}/device/airplay/shared',f'-I{SRC}/device/core',f'-I{SRC}/device/net',f'-I{ROOT}/firmware/lib/common/src']
    # protocol.h is in lib/common root on older checkouts.
    common.extend([f'-I{ROOT}/firmware/lib/common'])
    run(common+[f'-I/opt/homebrew/include',HERE/'wire_check.c',SRC/'device/talk/talk_wire.c',SRC/'device/talk/voice_ogg.c','-L/opt/homebrew/lib','-lopus','-o',out/'wire'])
    env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0')
    run([out/'wire',out/'note.ogg'],env=env)
    probe=json.loads(subprocess.check_output(['ffprobe','-v','error','-show_streams','-of','json',out/'note.ogg']))
    assert probe['streams'][0]['codec_name']=='opus'
    assert probe['streams'][0]['channels']==1
    run(['ffmpeg','-v','error','-i',out/'note.ogg','-f','null','-'])
    # Compile the existing RAOP RTP-Info parser, without its unrelated RSA dependencies.
    source=(SRC/'device/airplay/shared/raop_setup.c').read_text()
    start=source.index('void raop_parse_rtp_info(')
    end=source.index('\n}',start)+2
    (out/'rtp_info.c').write_text('#include <stdio.h>\n#include <stdint.h>\n#include <stdlib.h>\n#include <string.h>\n'+source[start:end]+'\n')
    with socket.socket() as sock:
        sock.bind(('127.0.0.1',0)); rtsp_port=sock.getsockname()[1]
    run(common+[f'-DTALK_RTSP_PORT={rtsp_port}',HERE/'rtsp_check.c',SRC/'device/talk/talk_rtsp.c',SRC/'device/airplay/shared/http_util.c',out/'rtp_info.c','-o',out/'rtsp'])
    run([out/'rtsp'],env=env)
    for name in ['freertos/queue.h','nvs.h']:
        (stubs/name).write_text('#include "runtime.h"\n')
    (stubs/'nvs.h').write_text('#include "runtime.h"\ntypedef int nvs_handle_t;\n#define NVS_READWRITE 1\n#define NVS_READONLY 0\nstatic inline int nvs_open(const char *n,int mode,nvs_handle_t *h){(void)n;(void)mode;(void)h;return 1;}\nstatic inline int nvs_set_blob(int h,const char *n,const void *p,size_t s){return 1;}\nstatic inline int nvs_get_blob(int h,const char *n,void *p,size_t *s){return 1;}\nstatic inline int nvs_commit(int h){return 1;}\nstatic inline void nvs_close(int h){}\n')
    (stubs/'esp_partition.h').write_text('#pragma once\n#include "runtime.h"\n#define ESP_PARTITION_TYPE_DATA 1\n#define ESP_PARTITION_SUBTYPE_DATA_SPIFFS 0x82\ntypedef struct {size_t address,size;} esp_partition_t;\nconst esp_partition_t *esp_partition_find_first(int,int,const char *);\nint esp_partition_register_external(void *,size_t,size_t,const char *,int,int,const esp_partition_t **);\nint esp_partition_read(const esp_partition_t *,size_t,void *,size_t);\n')
    (stubs/'esp_spiffs.h').write_text('#pragma once\n#include "runtime.h"\ntypedef int esp_err_t;\ntypedef struct {const char *base_path,*partition_label;size_t max_files;bool format_if_mount_failed;} esp_vfs_spiffs_conf_t;\nint esp_vfs_spiffs_register(const esp_vfs_spiffs_conf_t *);\nint esp_spiffs_info(const char *,size_t *,size_t *);\n')
    (stubs/'driver').mkdir(exist_ok=True)
    (stubs/'driver/gpio.h').write_text('#pragma once\n#include "runtime.h"\n#define GPIO_NUM_0 0\n#define GPIO_MODE_OUTPUT 1\ntypedef struct {uint64_t pin_bit_mask;int mode;} gpio_config_t;\nstatic int gpio_level;\nstatic int gpio_config(const gpio_config_t *c){(void)c;return 0;}\nstatic void gpio_set_level(int pin,int level){(void)pin;gpio_level=level;}\n')
    run(common+['-DHOST_NO_TASKS',f'-I{SRC}/device/drivers',HERE/'source_check.c','-o',out/'sources'])
    run([out/'sources'],env=env)
    run(common+['-ffunction-sections','-Wl,-dead_strip',f'-I{SRC}/ui',f'-I{SRC}/device/drivers',f'-I{SRC}/device/radio',f'-I{SRC}/device/airplay',HERE/'volume_check.c','-o',out/'volume'])
    run([out/'volume'],env=env)
    store=out/'data';store.mkdir()
    cjson=ROOT/'firmware/s3/managed_components/espressif__cjson/cJSON'
    run(common+[f'-I{SRC}/ui',f'-I{cjson}',f'-I{ROOT}/firmware/s3/managed_components/espressif__esp_audio_codec/include',f'-I{SRC}/device/drivers',f'-I{SRC}/device/radio',f'-I{ROOT}/firmware/s3/managed_components/espressif__esp_audio_codec/include/encoder',f'-I{ROOT}/firmware/s3/managed_components/espressif__esp_audio_codec/include/decoder','-I/opt/homebrew/include',f'-DMESSAGE_DATA_PATH="{store}"',HERE/'store_check.c',SRC/'device/talk/message_store.c',SRC/'device/talk/flash_message.c',SRC/'device/talk/voice_notes.c',SRC/'device/talk/voice_ogg.c',SRC/'device/radio/pcm.c',HERE/'codec_adapter.c',cjson/'cJSON.c','-L/opt/homebrew/lib','-lopus','-o',out/'store'])
    for mode in ('seed','reload','full'):
        run([out/'store',mode],env=dict(env,**({'STORE_FAIL':'1'} if mode=='full' else {})))
    snapshots=list(store.glob('inbox.?'))
    newest=max(snapshots,key=lambda p:int.from_bytes(p.read_bytes()[4:8],'little'))
    newest.write_bytes(newest.read_bytes()[:20])
    run([out/'store','fallback'],env=env)
    run([out/'store','voice'],env=env)

print('All LAN voice host checks passed')
