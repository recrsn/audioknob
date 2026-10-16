#pragma once

// Owns EC2_A/EC2_B (this MCU's physical encoder) per the control-ownership
// rule: this MCU is authoritative for its own encoder in wireless modes.
// Starts a background task; no per-tick polling needed by callers.
void encoder_start(void);
