#pragma once
// AI Thinker ESP32-CAM (classic ESP32) face-unit: OV3660 eyes, face OLED,
// link UART (PARKED, unrouted). UART0 console via the ESP32-CAM-MB board.
// Swap via include/mcu/active_mcu.h.

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

// Face OLED (SSD1306 128x64, address scanned, never assumed).
// SDA=14/SCL=15: never move SDA to GPIO12 (MTDI strapping: must read LOW at reset).
#define MCU_OLED_SDA 14
#define MCU_OLED_SCL 15
#define MCU_ADDR_OLED_PRIMARY 0x3C  // Module-fixed: scan BOTH, never hardcode one
#define MCU_ADDR_OLED_ALT 0x3D

// Link UART2 (PARKED, do not route).
#define MCU_LINK_UART_NUM 2
#define MCU_LINK_UART_TX 12
#define MCU_LINK_UART_RX 13

// Camera bus: board-fixed AI-Thinker map (XCLK=20MHz, QVGA JPEG).
#define MCU_CAM_PIN_PWDN 32
#define MCU_CAM_PIN_RESET -1
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

// Reserved: GPIO2 (strapping + LED, never drive), GPIO4 (flash-LED spare).

// Hard constraints (new MCU file to violate):
// GPIO12/MTDI must read LOW at reset (no pullup peripheral on GPIO12).
// UART0 is Serial-only; GPIO0/16 are camera/flash; GPIO34-39 camera-only.
// WiFi STA-only, no SoftAP, channel follows the AP; camera pins are
// digital-only (no ADC2-vs-WiFi conflict).
