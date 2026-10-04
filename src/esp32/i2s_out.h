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
void i2s_out_stop(void);

// Blocks until the whole buffer is queued to the DMA.
void i2s_out_write(const uint8_t *data, size_t len);
