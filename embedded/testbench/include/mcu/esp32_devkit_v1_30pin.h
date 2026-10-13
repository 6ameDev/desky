#pragma once
// MCU definition: ESP32 DevKit v1 (30-pin, classic ESP32-WROOM-32).
// Copy of core's esp32_devkit_v1_30pin.h (kept in sync by hand; testbench
// owns its copy so the rig never couples to core's pin evolution).
// Canonical baseline for desky v2 clean rewrite. Verified against
// V1 main branch pin map (motors/FAULT/XSHUT/I2C).
// Framework: Arduino + FreeRTOS, board = esp32dev, env = desky-testbench.

#include "mcu_common.h"

// ── Identity / capabilities ──────────────────────────────────
#define MCU_NAME "ESP32-DevKit-v1-30pin"
#define MCU_NUM_CORES 2
#define MCU_CPU_FREQ_MHZ 240
#define MCU_FLASH_SIZE_MB 4
#define MCU_HAS_PSRAM 0  // No PSRAM — Vision task stays stubbed
#define MCU_HAS_NATIVE_USB 0
#define MCU_HAS_CAMERA 0

// ── Proven V1 pin map (locked as baseline) ───────────────────
#define MCU_MOTOR_IN1 25    // MotorDriver IN1, LEDC 20kHz 8-bit
#define MCU_MOTOR_IN2 26    // MotorDriver IN2
#define MCU_MOTOR_IN3 18    // MotorDriver IN3
#define MCU_MOTOR_IN4 19    // MotorDriver IN4
#define MCU_MOTOR_FAULT 27  // Active-low fault, INPUT_PULLUP
#define MCU_TOF_XSHUT 4     // ToF enable, plain OUTPUT HIGH
#define MCU_I2C_SDA 21
#define MCU_I2C_SCL 22

// I2C device addresses on shared Wire bus
#define MCU_ADDR_TOF 0x29
#define MCU_ADDR_MPU_PRIMARY 0x68
#define MCU_ADDR_MPU_ALT 0x69
#define MCU_ADDR_OLED_PRIMARY 0x3C
#define MCU_ADDR_OLED_ALT 0x3D

// ── Reserved second I2C bus (NOT initialized — no Wire1 instance yet) ──
// RESERVED-for-future-bus (e.g. OLED-on-bus2, isolating its ~23ms render
// hold from ToF/MPU). GPIO32/33 are safe spares: not strapping, not SPI
// flash (6-11), not input-only (34-39), ADC1 (no ADC2/WiFi conflict).
// Do NOT call Wire1.begin() until the second I2CManager instance lands.
#define MCU_I2C1_SDA 32
#define MCU_I2C1_SCL 33

// Onboard LED (most DevKit v1 boards tie it to GPIO2)
#define MCU_LED_PIN 2

// ── I2S mic POC (INMP441 pair: shared WS/SCK/SD, stereo by L/R tie) ──
// WS=25/SCK=26/SD=27: adjacent triple, clear of I2C (21/22), LED (GPIO2),
// strapping (0/2/5/12/15), flash (6-11) and input-only (34-39). No DOUT:
// the rig has no amp, so the port runs RX-only (see src/hal/mic_poc.h).
// Wiring per hardware/COMPONENTS.md §7: SCK←BCLK, WS←WS, SD→DIN, VDD←3V3,
// GND common; mic A L/R→GND (LEFT slot), mic B L/R→3V3 (RIGHT slot).
#define MCU_I2S_WS 25
#define MCU_I2S_BCLK 26
#define MCU_I2S_DIN 27
// DOUT=32 (amp DIN): free GPIO, clear of strapping/flash/input-only/I2C.
// Speaker stage only — unconnected until the MAX98357A lands.
#define MCU_I2S_DOUT 32

// ── TCRT POC (discrete reflective pair, COMPONENTS.md §5 circuit) ──
// TCRT_1=33: ADC1 (WiFi-safe analog), clear of I2C (21/22), audio
// (25/26/27/32) and LED (GPIO2). Emitter 220Ω + detector 10k tap,
// tap row -> this pin (see plan notes, never the rail-side row).
#define MCU_TCRT_1 33
// TCRT drain tap (modulated emitter): DRAIN_1=13 sinks the emitter cathode
// (GPIO LOW = on). Plain GPIO, non-strapping, LEDC-capable for future PWM
// brightness; digital-only use so ADC rules never apply.
#define MCU_TCRT_DRAIN_1 13

// ── Bus / peripheral defaults ────────────────────────────────
#define MCU_I2C_FREQ_HZ 400000
#define MCU_I2C_TIMEOUT_MS 20
#define MCU_PWM_FREQ_HZ 20000
#define MCU_PWM_RESOLUTION_BITS 8
#define MCU_PWM_MIN_DUTY 65  // MIN_MOTOR_PWM from V1

// ── Hard constraints (do not violate without new MCU file) ──
// - Strapping pins 0/2/5/12/15: keep unconnected at boot (none used above).
// - GPIO6-11: reserved for SPI flash, never use as GPIO.
// - GPIO34-39: input-only, no pullup/PWM — do not assign outputs here.
// - GPIO25/26/27 are ADC2-capable: NO analogRead on them while WiFi is on
//   (ADC2 conflicts with WiFi). No analogRead in V1 — keep it that way.
// - GPIO4 drives onboard LED on some DevKits — XSHUT as plain OUTPUT is fine.
// - Single shared Wire bus + i2cMutex model required (OLED holds ~23ms).
// - Default 4MB partition: V1 flash ~80% full — watch headroom, prefer
//   larger-app partition if firmware grows.
