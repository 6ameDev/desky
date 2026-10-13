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

// I2S mic POC (stage 1: single INMP441; see src/hal/mic_poc.h)
#define CFG_I2S_WS MCU_I2S_WS
#define CFG_I2S_BCLK MCU_I2S_BCLK
#define CFG_I2S_DIN MCU_I2S_DIN
#define CFG_I2S_DOUT MCU_I2S_DOUT
#define CFG_AUDIO_RATE_HZ 16000
// Meter: one 256-frame batch (16ms @16kHz) per tick, RMS lines at 5Hz.
#define CFG_MIC_BATCH_FRAMES 256
#define CFG_MIC_METER_MS 200

// Demo presentation for the video shoot (TEMPORARY — set 0 and delete the
// demo block after the shoot): bar-graph meter + plain-words verdict, beeps
// suppressed. Measurement is identical; only the serial presentation changes.
#define CFG_DEMO_MODE 1

// Speaker POC (stage S2: MAX98357A + 8Ω cavity; TX side of the duplex POC).
// Quiet by construction: ~5% full-scale beeps into 3V3 Vin (~1W amp rail).
#define CFG_SPK_FREQ_A_HZ 440
#define CFG_SPK_FREQ_B_HZ 880
#define CFG_SPK_BEEP_MS 200
#define CFG_SPK_EVERY_MS 2000
#define CFG_SPK_PEAK 1500

// TCRT POC (modulated: on/off pairs minus ambient; rail on signal-space).
// Signal bands (predicted: white ~4000, void ~0 — confirmed in condition 1):
// assert surface at >= 2500, clear at <= 1500. Re-pin from measured sig.
#define CFG_TCRT_PIN MCU_TCRT_1
#define CFG_TCRT_READ_MS 200
#define CFG_TCRT_THRESHOLD 2000
#define CFG_TCRT_HYSTERESIS 500
#define CFG_TCRT_ASSERT_HIGH 1
// Modulated emitter (drain tap): on/off pair reads minus ambient.
// LOW = on (~9mA sink, in-spec); settle covers the phototransistor lag;
// pairs-per-tick average across mains flicker.
#define CFG_TCRT_DRAIN_PIN MCU_TCRT_DRAIN_1
#define CFG_TCRT_SETTLE_US 1000
#define CFG_TCRT_PAIRS 4
