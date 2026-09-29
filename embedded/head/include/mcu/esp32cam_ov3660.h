#pragma once
// MCU definition: AI Thinker ESP32-CAM (classic ESP32, desky-head face-unit).
// Eyes OV3660 via ribbon + face OLED + talk-wire UART to desky-core.
// Flashed/monitored via the ESP32-CAM-MB USB board (UART0 console — Serial
// just works). Framework: Arduino + FreeRTOS, board = esp32cam.
// Swap back via include/mcu/active_mcu.h — no driver changes.
//
// MB-board rule (load-bearing for every USB run): the MB occupies ALL module
// pins, so the face OLED can NEVER be attached during USB runs. The camera
// works on-MB (ribbon only). Consequence, enforced in the display driver:
// OLED init must SCAN (never assume an address) and a "not found" result
// logs cleanly and never blocks boot. Full face validation is deferred to
// the field harness (OLED on 14/15, camera + OLED + link live together).

#include "mcu_common.h"

// ── Identity / capabilities ──────────────────────────────────
#define MCU_NAME "AI-THINKER-ESP32-CAM-OV3660"
#define MCU_NUM_CORES 2
#define MCU_CPU_FREQ_MHZ 240
#define MCU_FLASH_SIZE_MB 4
#define MCU_HAS_PSRAM 1  // 4MB PSRAM — camera frame buffers live here
#define MCU_HAS_NATIVE_USB \
  0                       // No USB on the module; the console is UART0
                          // via the ESP32-CAM-MB board — Serial just works
#define MCU_HAS_CAMERA 1  // OV3660 eyes on the board-fixed bus below

// ── Face OLED (SSD1306 128x64, address SCANNED, never assumed) ──
// SDA=14/SCL=15: PROVEN booting with the OLED attached (bench 2026-09-28).
// The previous SDA=12 map bricked the boot — GPIO12/MTDI must read LOW at
// reset for the 3.3V flash, but the OLED's SDA pullup holds it HIGH, giving
// "invalid header: 0xffffffff" + RTCWDT loop before our code ever runs. No
// passive-resistor fix exists (a pulldown strong enough for boot breaks the
// I2C idle level). NEVER move SDA back to GPIO12.
#define MCU_OLED_SDA 14
#define MCU_OLED_SCL 15
#define MCU_ADDR_OLED_PRIMARY 0x3C  // Module-fixed: scan BOTH, never hardcode one
#define MCU_ADDR_OLED_ALT 0x3D

// ── Talk-wire UART2 to desky-core (S3 GPIO17/18, UART1) ──
// Plain AWAKE + heartbeat lines in this task; framing/CRC is task 3.
// TX=12 ON PURPOSE: CAM-TX is an output (silent at reset, internal pulldown
// holds the LOW the GPIO12 strapping wants), so the S3 never drives the
// strapping pin. CAM-RX=13 is non-strapping, safe for the S3's idle-HIGH.
// The S3 must keep its TX tristated until the CAM is up (extends the
// S3-waits-for-AWAKE rule to the pin level) — enforced in task 3's driver.
#define MCU_LINK_UART_NUM 2
#define MCU_LINK_UART_TX 12
#define MCU_LINK_UART_RX 13

// ── Camera bus: board-fixed AI-Thinker map (CAMERA_PIN_*) ──
// Ribbon-defined, never remapped. XCLK=20MHz, QVGA JPEG (see config.h).
#define MCU_CAM_PIN_PWDN 32
#define MCU_CAM_PIN_RESET -1  // Tied high on-module, no software reset
#define MCU_CAM_PIN_XCLK 0
#define MCU_CAM_PIN_SIOD 26
#define MCU_CAM_PIN_SIOC 27
#define MCU_CAM_PIN_Y9 35
#define MCU_CAM_PIN_Y8 34
#define MCU_CAM_PIN_Y7 39
#define MCU_CAM_PIN_Y6 36
#define MCU_CAM_PIN_Y5 21
#define MCU_CAM_PIN_Y4 19
#define MCU_CAM_PIN_Y3 18
#define MCU_CAM_PIN_Y2 5
#define MCU_CAM_PIN_VSYNC 25
#define MCU_CAM_PIN_HREF 23
#define MCU_CAM_PIN_PCLK 22

// ── Reserved (comment-only: firmware never touches these) ──
// GPIO2: reserved-future-RTS for the talk-wire (also a strapping pin and the
//   on-module LED — never drive it in this task).
// GPIO4: flash-LED spare (leave alone in this task).

// ── Hard constraints (do not violate without a new MCU file) ──
// - GPIO12 (link TX) is a BOOT-STRAPPING pin (MTDI): it must read LOW at
//   reset for the correct flash voltage. SAFE HERE because CAM-TX is an
//   output: silent at reset (internal pulldown holds LOW) and driven only
//   after boot; the S3 side (RX, high-Z input) never drives it. NEVER
//   attach a pullup-holding peripheral (e.g. an OLED SDA) to GPIO12 —
//   bench-proven 2026-09-28 to brick boot ("invalid header: 0xffffffff" +
//   RTCWDT loop). GPIO15 (OLED SCL) strapping is flash-voltage-safe.
// - MB occupies ALL pins: OLED can NEVER be attached during USB runs;
//   display code is compile-tested + boot-scan negative-tested on-MB only.
// - UART0 (module TX0/RX0 via MB) is RESERVED — Serial (USB CLI) only.
// - GPIO0 (XCLK!) + GPIO16 (PSRAM CS): camera/flash use — NEVER reuse.
// - GPIO34-39 (camera Y6-Y9): input-only, no pullups — camera use only.
// - WiFi stays OFF on head (no radio init anywhere in this firmware).
// - Star 5V power assumed: module + camera + OLED peak together, and the
//   camera+display pair is power-hungry (see head power manager).
