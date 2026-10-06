#pragma once
// desky-head global config — tunables only, never hardcoded pins.
// All board specifics come from the active MCU definition.

// Boot power mode: 0 = validation (camera ON + display ON, for the on-MB
// bring-up and the field harness); 1 = lean (camera OFF + display DIM).
// Flips to 1 once face/link validation is done.
#define HEAD_BOOT_LEAN 0

#include "mcu/active_mcu.h"

// I2C bus (face OLED; defaults consumed by shared/common/i2c_manager.h)
#define CFG_I2C_SDA MCU_OLED_SDA
#define CFG_I2C_SCL MCU_OLED_SCL
#define CFG_I2C_FREQ_HZ 400000
#define CFG_I2C_TIMEOUT_MS 20

// Link UART2 (PARKED — retained for switch-back, no copper routed).
#define CFG_LINK_BAUD 115200
#define CFG_LINK_HEARTBEAT_MS 1000

// USB serial CLI (laptop terminal on the MB board)
#define CFG_CLI_BAUD 115200
#define CFG_CLI_LINE_LEN 64

// Face display render cadence (full vs DIM refresh)
#define CFG_FACE_FPS 25
#define CFG_FACE_FPS_DIM 5

// Face render task (classic ESP32: noisy display work off the Arduino loop;
// priority stored as an offset — config.h itself stays FreeRTOS-free so
// Arduino-free headers keep compiling on host).
#define CFG_FACE_STACK_WORDS 4096
#define CFG_FACE_PRIORITY_OFFSET 2
#define CFG_FACE_CORE 0

// Camera: QVGA JPEG stills, grab-and-drop at the poll rate (no streaming —
// WiFi stays OFF on head).
#define CFG_CAMERA_POLL_MS 500
#define CFG_CAMERA_JPEG_QUALITY 12
