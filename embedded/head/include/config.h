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

// Link WiFi client (head STA -> core AP; WiFi ON for the link — STA only,
// modem-sleep between HBs, no AP on head). See src/transport/link_client.h +
// the MCU-adjacent WiFi comment in include/mcu/esp32cam_ov3660.h.
#define CFG_LINK_SSID "desky"
#define CFG_LINK_PASS "desky1234"
#define CFG_LINK_HOST "192.168.4.1"
#define CFG_LINK_UDP_PORT 3334
#define CFG_LINK_TCP_PORT 3335
// Link-down rule: this many missed HB ACKs (~1s each) before re-associate.
#define CFG_LINK_MAX_MISSED 3
// Association retry throttle while the AP is absent (every USB run).
#define CFG_LINK_ASSOC_RETRY_MS 5000
// Association attempt timeout: WiFi.begin() is async, so a started attempt
// gets this long to reach WL_CONNECTED before we disconnect + start fresh.
// Must exceed the normal scan+handshake time (1-3s) with margin.
#define CFG_LINK_ASSOC_TIMEOUT_MS 10000

// OTA pull (see src/abilities/ota/ota_task.h + partitions_ota_nospiffs.csv):
// each app slot is 0x1F0000 — the hard ceiling ota::fitsSlot() enforces.
// The head ships no image without the OTA client (standing rule, see the
// ota header); USB is the recovery backstop only.
#define CFG_OTA_SLOT_BYTES 0x1F0000
// Stall timeout for the OTA body pump (no bytes for this long -> abort).
#define CFG_OTA_TIMEOUT_MS 15000
// Running image version (monotonic u32): the carrier manifest
// (GET /head-version) compares against this via otamanifest::shouldPull —
// bump on every release or the carrier pull-gate treats the image as current
// and never pulls. Pinned literally by host tests.
#define HEAD_FW_VERSION 14

// USB serial CLI (laptop terminal on the MB board)
#define CFG_CLI_BAUD 115200
#define CFG_CLI_LINE_LEN 64

// Face display render cadence (full vs DIM refresh)
#define CFG_FACE_FPS 25
#define CFG_FACE_FPS_DIM 5

// Face QR show mode (H2.5): a rendered pairing bitmap stays until a FACE
// command arrives or this timeout elapses (FACE-command-or-120s-timeout).
#define CFG_FACE_QR_TIMEOUT_MS 120000

// Face OTA progress mode (carrier trigger): the fill bar + alert face clear
// on any FACE command or when this timeout elapses since the last progress
// update (same FACE-command-or-timeout house rule as QR above).
#define CFG_FACE_OTA_TIMEOUT_MS 120000

// Face render task (classic ESP32: noisy display work off the Arduino loop;
// priority stored as an offset — config.h itself stays FreeRTOS-free so
// Arduino-free headers keep compiling on host).
#define CFG_FACE_STACK_WORDS 4096
#define CFG_FACE_PRIORITY_OFFSET 2
#define CFG_FACE_CORE 0

// Camera: QVGA JPEG stills, grab-and-drop at the poll rate (no continuous
// streaming — on-demand stills only, uploaded over the WiFi link by `snap`).
#define CFG_CAMERA_POLL_MS 500
#define CFG_CAMERA_JPEG_QUALITY 12
