#pragma once

#include <stddef.h>
#include <stdint.h>

// I2S TX from the S3 to the CH445P's S3-side input (GPIO39/40/41), which
// reaches the PCM5100A only while audio_source has selected the S3 side.
// Stereo 32-bit slots at 44.1 or 48 kHz, whichever the source playing asks
// for: samples are left-justified int32 (the PCM5100A takes the top 24 bits
// of 32), so 24-bit sources and the software volume keep their resolution.
// Underruns play silence (DMA auto-clear), so callers never need to pad.

#define AUDIO_OUT_SAMPLE_RATE 44100  // at boot, and for sources with no rate of their own
#define AUDIO_OUT_SAMPLE_RATE_MAX 48000

void audio_out_init(void);

// Reclocks the DAC for what the caller writes next, after what is already
// queued has played out (up to the DMA ring's ~33 ms). A no-op at the
// current rate, so a writer can call it before every write.
void audio_out_set_rate(uint32_t hz);
uint32_t audio_out_rate(void);

// Blocks until all frames are queued to DMA. Writers are serialized: the
// stream's own (AirPlay), and the mix's owner topping the queue up with
// silence while that one is idle. write16 is for 16-bit sources.
void audio_out_write(const int32_t *pcm, size_t frames);
void audio_out_write16(const int16_t *pcm, size_t frames);

// Something mixed into every write from now on (the alarm chime over
// AirPlay), or NULL. mix() edits up to AUDIO_OUT_MIX_FRAMES stereo frames in
// place per call, under the write lock, so it runs on the writer's task and
// must be quick. rate is the DAC's for these frames. Once this returns, the
// old mix is no longer running.
#define AUDIO_OUT_MIX_FRAMES 240
typedef void (*audio_out_mix_t)(int32_t *pcm, size_t frames, uint32_t rate);
void audio_out_set_mix(audio_out_mix_t mix);

// How long a frame written now takes to reach the DAC, measured from the
// DMA completion cursor rather than modelled, so it is right after an
// underrun too. now_us is esp_timer_get_time() sampled with it.
uint32_t audio_out_queued_us(int64_t *now_us);

// Times the DMA ran dry since boot (each one played as silence).
uint32_t audio_out_underruns(void);
