#pragma once

#include <stddef.h>
#include <stdint.h>

// I2S TX to the CH445P switch's ESP32-side input (S2 bank), which the S3
// selects toward the PCM5100A when this MCU owns the audio path.

void i2s_out_init(void);

// Reconfigures clocks for the negotiated A2DP codec (sample rate, channels)
// and (re)enables the channel. Safe to call whenever codec config changes.
void i2s_out_configure(uint32_t sample_rate_hz, uint8_t channels);

void i2s_out_start(void);

// Software volume, 0-100, ramped in over a few ms. Safe from any task.
void i2s_out_set_volume(uint8_t volume_pct);
void i2s_out_stop(void);

// Never blocks: queues PCM into a ring buffer that a dedicated task feeds to
// the DMA. Safe to call from the Bluedroid data callback; drops data while
// stopped or after an overflow.
void i2s_out_write(const uint8_t *data, size_t len);
