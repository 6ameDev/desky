#pragma once
// Global config: tunables only, never hardcoded pins. Board specifics come
// from the active MCU definition.

// Boot power mode: 0 = validation (everything on); 1 = lean (camera off, display dim).
#define HEAD_BOOT_LEAN 0

#include "mcu/active_mcu.h"

// I2C bus (face OLED; defaults consumed by shared/common/i2c_manager.h)
#define CFG_I2C_SDA MCU_OLED_SDA
#define CFG_I2C_SCL MCU_OLED_SCL
#define CFG_I2C_FREQ_HZ 400000
#define CFG_I2C_TIMEOUT_MS 20

// Link UART2 (PARKED, no copper routed).
#define CFG_LINK_BAUD 115200
#define CFG_LINK_HEARTBEAT_MS 1000

// Link WiFi client (head STA -> core AP, modem-sleep between HBs, no AP on head).
#define CFG_LINK_SSID "desky"
#define CFG_LINK_PASS "desky1234"
#define CFG_LINK_HOST "192.168.4.1"
#define CFG_LINK_UDP_PORT 3334
#define CFG_LINK_TCP_PORT 3335
// Link-down rule: missed HB ACKs before re-associate.
#define CFG_LINK_MAX_MISSED 3
// Assoc retry throttle while the AP is absent.
#define CFG_LINK_ASSOC_RETRY_MS 5000
// Assoc attempt timeout (WiFi.begin() is async).
#define CFG_LINK_ASSOC_TIMEOUT_MS 10000

// OTA pull: each app slot is 0x1F0000 (hard ceiling ota::fitsSlot() enforces).
#define CFG_OTA_SLOT_BYTES 0x1F0000
// OTA body-pump stall timeout.
#define CFG_OTA_TIMEOUT_MS 15000
// Running image version (monotonic u32, bump every release or the pull-gate stands down).
#define HEAD_FW_VERSION 27

// USB serial CLI
#define CFG_CLI_BAUD 115200
#define CFG_CLI_LINE_LEN 64

// Display render cadence
#define CFG_DISPLAY_FPS 25
#define CFG_DISPLAY_FPS_DIM 5

// QR show mode: stays until a FACE command or this timeout.
#define CFG_DISPLAY_QR_TIMEOUT_MS 120000

// OTA progress mode: clears on any FACE command or this timeout.
#define CFG_DISPLAY_OTA_TIMEOUT_MS 120000

// Display render task (offset form keeps this header FreeRTOS-free for host builds).
#define CFG_DISPLAY_STACK_WORDS 4096
#define CFG_DISPLAY_PRIORITY_OFFSET 2
#define CFG_DISPLAY_CORE 0

// Camera: QVGA JPEG stills, grab-and-drop at the poll rate.
#define CFG_CAMERA_POLL_MS 500
#define CFG_CAMERA_JPEG_QUALITY 12
