#pragma once
// MCU definition: ESP32-S3-WROOM-1-N16R8 DevKit (primary brain, desky-core).
// 16MB quad flash + 8MB octal PSRAM, CH9102 USB-UART console.
// Framework: Arduino + FreeRTOS, board = esp32-s3-devkitc1-n16r8.
// Replaces esp32_devkit_v1_30pin.h (kept for rollback); swap back via
// include/mcu/active_mcu.h — no driver/fusion/behavior changes.

#include "mcu_common.h"

// ── Identity / capabilities ──────────────────────────────────
#define MCU_NAME "ESP32-S3-WROOM-1-N16R8"
#define MCU_NUM_CORES 2
#define MCU_CPU_FREQ_MHZ 240
#define MCU_FLASH_SIZE_MB 16
#define MCU_HAS_PSRAM 1  // 8MB octal PSRAM — present; core vision use is TBD
#define MCU_HAS_NATIVE_USB \
  1                       // S3 USB-OTG exists, but the console is the
                          // CH9102 USB-UART (UART0) — Serial just works
#define MCU_HAS_CAMERA 0  // No camera on core — eyes live in desky-head

// ── Fresh S3 pin map (all safe: no strapping, no USB, no flash) ──
// Motors: GPIO4-7 — plain GPIOs, LEDC PWM-capable, none strapping.
// (Classic-ESP32 rule "GPIO6-11 are flash" does NOT apply on S3 —
// octal flash/PSRAM lives on GPIO33-37 instead.)
#define MCU_MOTOR_IN1 4    // MotorDriver IN1, LEDC 20kHz 8-bit
#define MCU_MOTOR_IN2 5    // MotorDriver IN2
#define MCU_MOTOR_IN3 6    // MotorDriver IN3
#define MCU_MOTOR_IN4 7    // MotorDriver IN4
#define MCU_MOTOR_FAULT 8  // Active-low fault, INPUT_PULLUP
#define MCU_TOF_XSHUT 9    // Firmware-defined, UNROUTED on carrier (sensor runs on module pull-up)
#define MCU_I2C_SDA 10     // Shared bus0 SDA (ToF + MPU)
#define MCU_I2C_SCL 11     // Shared bus0 SCL (ToF + MPU)
// TCRT5000 cliff A0 (discrete pairs, no modules: 220Ω emitter + 10kΩ tap,
// 3V3 rails — see hardware/COMPONENTS.md §5). GPIO1/2 (ADC1, WiFi-safe).
#define MCU_TCRT_FWD 1  // Front cliff ADC tap (analog, D0 unconnected)
#define MCU_TCRT_REV 2  // Rear cliff ADC tap (analog, D0 unconnected)

// MG90S servos (independent functions, not mirrored: arm + head-tilt).
// GPIO41/42: adjacent pair extending the TCRT block, clear of strapping,
// flash, RGB and console. Independent LEDC channels @50Hz on their own
// timer (motors run 20kHz on a separate group). Pin↔function mapping is
// firmware-only — swapping is a two-define change, no rewiring.
#define MCU_SERVO_ARM 41   // Arm servo PWM (JR plug, 5V star + GND)
#define MCU_SERVO_HEAD 42  // Head-tilt servo PWM (JR plug, 5V star + GND)

// I2C device addresses on shared Wire bus
#define MCU_ADDR_TOF 0x29
#define MCU_ADDR_MPU_PRIMARY 0x68
#define MCU_ADDR_MPU_ALT 0x69
// OLED addrs RESERVED-moved-to-head: the face OLED lives on desky-head
// (CAM SDA=GPIO14/SCL=GPIO15) — core never addresses a local display.
#define MCU_ADDR_OLED_PRIMARY 0x3C
#define MCU_ADDR_OLED_ALT 0x3D

// ── I2S audio (MAX98357A amp + 2x INMP441 mics, shared BCLK/WS) ──
// WS=15 BCLK=16 DOUT=17 (amp DIN) DIN=18 (mics SD, L on GND-mic / R on
// 3V3-mic by hardware tie). Single duplex port @16kHz (see config.h).
// Amp SD_MODE floating = module-default (L+R)/2 mix; firmware sends
// dual-mono anyway so the channel pick is moot (strap to 3V3 for left if
// it ever goes silent). Amp VIN on 5V (8Ω 2W cavity — keep beeps quiet).
// GPIO15/16 were RESERVED for link RTS/CTS — released to I2S (link is
// 2-wire, flow control was never wired).
#define MCU_I2S_WS 15
#define MCU_I2S_BCLK 16
#define MCU_I2S_DOUT 17
#define MCU_I2S_DIN 18

// ── Link UART1 (PARKED — defines kept for switch-back, do NOT route) ──
// Was S3 GPIO12/21 <-> CAM TX=GPIO12/RX=GPIO13 (UART2). Wireless TBD.
#define MCU_LINK_UART_TX 12
#define MCU_LINK_UART_RX 21
// UART0 console (GPIO43/44 via CH9102) is RESERVED — Serial only.

// ── Bus / peripheral defaults ────────────────────────────────
#define MCU_I2C_FREQ_HZ 400000
#define MCU_I2C_TIMEOUT_MS 20
#define MCU_PWM_FREQ_HZ 20000
#define MCU_PWM_RESOLUTION_BITS 8
#define MCU_PWM_MIN_DUTY 65  // MIN_MOTOR_PWM from V1

// ── Hard constraints (do not violate without a new MCU file) ──
// - Strapping pins 0/3/45/46: keep unconnected at boot (none used above).
// - GPIO19/20: USB D-/D+ — never use as GPIO.
// - GPIO33-37: octal flash + octal PSRAM — NEVER touch.
// - GPIO43/44: UART0 console (CH9102) — Serial only.
// - GPIO48: onboard RGB LED — leave alone (no status-LED driving).
// - GPIO0: BOOT button; GPIO3: strapping — both avoided above.
// - No second I2C bus in core: the display moved to desky-head, so no
//   bus2 instance exists; add one here only if a new core subordinate
//   needs isolation from ToF/MPU timing.
// - Single shared Wire bus + i2cMutex model required for ToF/MPU.
