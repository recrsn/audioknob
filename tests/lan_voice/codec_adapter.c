// Host codec and hardware adapters. Production voice-note tasks run unchanged.
#include "runtime.h"
#include <math.h>
#include <stdatomic.h>
#include <opus/opus.h>
#include "encoder/impl/esp_opus_enc.h"
#include "decoder/impl/esp_opus_dec.h"
#include "audio_source.h"
#include "mic.h"
#include "talk.h"
#include "voice_notes.h"
static atomic_int owner=AUDIO_SOURCE_ESP32;
static atomic_bool mic_owned;
atomic_uint output_frames;
bool audio_source_claim(audio_source_t source) { atomic_store(&owner,source);return true; }
void audio_source_release(audio_source_t source) { int expected=source;atomic_compare_exchange_strong(&owner,&expected,AUDIO_SOURCE_ESP32); }
audio_source_t audio_source_active(void) { return atomic_load(&owner); }
bool audio_source_suspended(void) { return false; }
void audio_out_set_rate(uint32_t hz) { (void)hz; }
void audio_out_write16(const int16_t *pcm,size_t frames) { assert(pcm&&frames<=900);atomic_fetch_add(&output_frames,frames);usleep(frames*1000000/44100); }
bool mic_start(const mic_config_t *cfg) { (void)cfg;bool expected=false;return atomic_compare_exchange_strong(&mic_owned,&expected,true); }
void mic_stop(void) { atomic_store(&mic_owned,false); }
size_t mic_read(int16_t *pcm,size_t frames,uint32_t timeout) {
    (void)timeout;assert(atomic_load(&mic_owned));static unsigned cursor;
    for (size_t i=0;i<frames;i++) pcm[i]=(int16_t)(5000*sin(cursor++*2*3.141592653589793*440/16000));
    usleep(frames*1000000/16000);return frames;
}
void talk_get_state(ui_talk_t *out) { memset(out,0,sizeof(*out));out->volume=100; }
esp_audio_err_t esp_opus_enc_open(void *config,uint32_t size,void **handle) {
    assert(size==sizeof(esp_opus_enc_config_t));esp_opus_enc_config_t *cfg=config;int error;
    *handle=opus_encoder_create(cfg->sample_rate,cfg->channel,OPUS_APPLICATION_VOIP,&error);
    if (error!=OPUS_OK) return ESP_AUDIO_ERR_FAIL;
    opus_encoder_ctl(*handle,OPUS_SET_BITRATE(cfg->bitrate));return ESP_AUDIO_ERR_OK;
}
esp_audio_err_t esp_opus_enc_get_frame_size(void *handle,int *input,int *output) { assert(handle);*input=640;*output=1275;return ESP_AUDIO_ERR_OK; }
esp_audio_err_t esp_opus_enc_process(void *handle,esp_audio_enc_in_frame_t *in,esp_audio_enc_out_frame_t *out) {
    int n=opus_encode(handle,(int16_t *)in->buffer,in->len/2,out->buffer,out->len);
    if (n<0) return ESP_AUDIO_ERR_FAIL;out->encoded_bytes=n;return ESP_AUDIO_ERR_OK;
}
void esp_opus_enc_close(void *handle) { opus_encoder_destroy(handle); }
esp_audio_err_t esp_opus_dec_open(void *config,uint32_t size,void **handle) {
    assert(size==sizeof(esp_opus_dec_cfg_t));esp_opus_dec_cfg_t *cfg=config;int error;
    *handle=opus_decoder_create(cfg->sample_rate,cfg->channel,&error);return error==OPUS_OK?ESP_AUDIO_ERR_OK:ESP_AUDIO_ERR_FAIL;
}
esp_audio_err_t esp_opus_dec_decode(void *handle,esp_audio_dec_in_raw_t *raw,esp_audio_dec_out_frame_t *out,esp_audio_dec_info_t *info) {
    memset(info,0,sizeof(*info));int n=opus_decode(handle,raw->len?raw->buffer:NULL,raw->len,(int16_t *)out->buffer,out->len/2,0);
    if (n<0) return ESP_AUDIO_ERR_FAIL;raw->consumed=raw->len;out->decoded_size=n*2;return ESP_AUDIO_ERR_OK;
}
esp_audio_err_t esp_opus_dec_close(void *handle) { opus_decoder_destroy(handle);return ESP_AUDIO_ERR_OK; }
