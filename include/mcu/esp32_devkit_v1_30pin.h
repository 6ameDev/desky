#pragma once
// MCU definition: ESP32 DevKit v1 (30-pin, classic ESP32-WROOM-32).
// Canonical baseline for desky v2 clean rewrite. Verified against
// V1 main branch pin map (motors/FAULT/XSHUT/I2C).
// Framework: Arduino + FreeRTOS, board = esp32dev, env = desky.

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
