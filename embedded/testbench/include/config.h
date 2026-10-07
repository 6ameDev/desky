#pragma once
// desky-testbench global config — tunables only, never hardcoded pins.
// All board specifics come from the active MCU definition.

#include "mcu/active_mcu.h"

// I2C bus (shared Wire bus for ad-hoc part scans)
#define CFG_I2C_SDA MCU_I2C_SDA
#define CFG_I2C_SCL MCU_I2C_SCL
#define CFG_I2C_FREQ_HZ MCU_I2C_FREQ_HZ

// USB serial console
#define CFG_CLI_BAUD 115200

// Onboard-LED breathe (LEDC PWM dimming, Arduino-3.x ledcAttach+ledcWrite).
// The diode is single fixed-color — brightness only, never color.
#define CFG_LED_FREQ_HZ 5000
#define CFG_LED_RES_BITS 8
// Quarter power: 63/255 of 8-bit full scale (the diode is bright, and
// GPIO2 is boot-sampled, so no full-scale drive).
#define CFG_LED_MAX_DUTY 63
// Breathe step: 63 steps each way -> ~1.9s per full breath at 15ms.
#define CFG_LED_STEP_MS 15

// I2C rescan cadence, in breaths
#define CFG_SCAN_EVERY_N_BREATHS 5
