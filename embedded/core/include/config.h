#pragma once
// Global config — no hardcoded pins here. All board specifics come from the
// active MCU definition.

#include "mcu/active_mcu.h"

// Motor PWM (mirrors MCU defaults, tunable per behavior)
#define CFG_PWM_FREQ_HZ MCU_PWM_FREQ_HZ
#define CFG_PWM_RES_BITS MCU_PWM_RESOLUTION_BITS
#define CFG_PWM_MIN_DUTY MCU_PWM_MIN_DUTY

// I2C bus
#define CFG_I2C_SDA MCU_I2C_SDA
#define CFG_I2C_SCL MCU_I2C_SCL
#define CFG_I2C_FREQ_HZ MCU_I2C_FREQ_HZ
#define CFG_I2C_TIMEOUT_MS MCU_I2C_TIMEOUT_MS

// Task rates
#define CFG_MOTOR_LOOP_HZ 100
#define CFG_MPU_RATE_HZ 50
// 10Hz ToF budget.
#define CFG_TOF_RATE_HZ 10
#define CFG_DISPLAY_FPS 25

// Sensor poll intervals (single source for ISensor::getTargetIntervalMs)
#define CFG_MPU_TARGET_INTERVAL_MS 20
#define CFG_TOF_TARGET_INTERVAL_MS 100
// TCRT5000 reflectance poll: analog A0 path.
#define CFG_TCRT_TARGET_INTERVAL_MS 20

// ToF validity + recovery
#define CFG_TOF_VALID_MAX_MM 4000
#define CFG_TOF_MAX_CONSEC_ERRORS 5
#define CFG_TOF_XSHUT_SHUTDOWN_MS 20
#define CFG_TOF_XSHUT_BOOT_MS 50

// Sensor fusion (see sensors_codec.h)
// TCRT5000 cliff rails (pins via the MCU header, never hardcoded here).
#define CFG_TCRT_FWD_PIN MCU_TCRT_FWD
#define CFG_TCRT_REV_PIN MCU_TCRT_REV
// Bench-measured: the modules sink A0 with reflection (ground = LOW).
// Bands (12-bit @11dB): close white ~167, far white / black desk ~210-220,
// open void ~1100. Rail rule: ground asserts at <= 400, clears at >= 800.
// Re-pin if the floor material or ride height changes.
#define CFG_TCRT_THRESHOLD 600
#define CFG_TCRT_HYSTERESIS 200
#define CFG_TCRT_ASSERT_HIGH 0
// Forward-facing ToF obstacle threshold in mm (no tilt compensation — the
// beam points forward). The sensor task adds a +/-10mm hysteresis band and
// publishes EVENT_OBSTACLE_DETECTED on the assert edge only.
#define CFG_OBSTACLE_MM 150
// DEPRECATED (ToF-as-cliff derivation is deleted; fusion reads the TCRT
// rails). Kept so older references keep compiling until removal.
#define CFG_CLIFF_MM 100
// DEPRECATED (no rear ToF ever existed).
#define CFG_CLIFF_MM_REV 100
// DEPRECATED (down-beam pitch compensation deleted with the derivation).
#define CFG_BEAM_DEPRESSION_DEG 30
// Total-tilt-magnitude gate in degrees — level ⟺ (pitch²+roll²) < 35²,
// evaluated only when the MPU is healthy (else the ground bits hold).
#define CFG_LEVEL_MAX_TILT_DEG 35.0f

// Static XY swap for fusion (compile-time): the MPU is mounted rotated
// 90deg about vertical. 0 = as-mounted, 1 = swap X/Y before tilt math.
#define CFG_FUSION_SWAP_AXAY 1

// Sensor task (Core 1 poll + fusion + publish; FreeRTOS-free offsets).
#define CFG_SENSOR_STACK_WORDS 4096
#define CFG_SENSOR_PRIORITY_OFFSET 3  // Below coordinator +4 / motion +5 so sensing never preempts control.
#define CFG_SENSOR_CORE 1             // Real-time slot alongside motion + coordinator.
#define CFG_SENSOR_LOOP_MS 15         // Faster than the fastest sensor (MPU 20ms) so elapsed scheduling never slips.

// Behavior coordinator task (Core 1 behavior slot, below Motion's +5).
// Priority is stored as an offset: firmware computes
// tskIDLE_PRIORITY + CFG_COORDINATOR_PRIORITY_OFFSET (config.h itself stays
// FreeRTOS-free so Arduino-free headers keep compiling on host).
#define CFG_COORDINATOR_STACK_WORDS 4096
#define CFG_COORDINATOR_PRIORITY_OFFSET 4
#define CFG_COORDINATOR_CORE 1
// UDP failsafe: no command within this window → failsafe stop (P4).
#define CFG_COORDINATOR_STALE_MS 500
// Coordinator tick: event drain + arbitrate + WDT feed period.
#define CFG_COORDINATOR_LOOP_MS 20

// WiFi + UDP server (dual-mode WiFi, binary control/telemetry).
// AP mode (default build) hosts "desky" with a human-typable WPA2 pass
// (min 8 chars); STA mode (desky-sta env) joins the home router via
// WIFI_SSID/WIFI_PASS from .env (never logged, never committed).
#define CFG_WIFI_AP_SSID "desky"
#define CFG_WIFI_AP_PASS "desky1234"
// UDP control RX + telemetry TX port.
#define CFG_UDP_PORT 3333
// Telemetry TX rate (spec window 10-20Hz).
#define CFG_TELEMETRY_HZ 15
// UDP server task (Core 0: noisy comms off the Core 1 real-time slot;
// priority below WiFi internals and below Motion +5 / Coordinator +4).
#define CFG_UDP_STACK_WORDS 4096
#define CFG_UDP_PRIORITY_OFFSET 3
#define CFG_UDP_CORE 0
// UDP task tick: RX drain + TX schedule + WDT feed period.
#define CFG_UDP_LOOP_MS 5
// STA reconnect cadence inside the UDP task loop (non-blocking).
#define CFG_WIFI_STA_RETRY_MS 5000

// Pairing window (Tier 1 enrolment): 6-digit ephemeral code, single-use,
// 5-min window, 5 bad tries -> 30s lockout. Bond/PSK keys live in ConfigStore
// cfg v2. FreeRTOS-free plain ints.
#define CFG_PAIR_CODE_LEN 6
#define CFG_PAIR_WINDOW_MS 300000
#define CFG_PAIR_MAX_TRIES 5
#define CFG_PAIR_LOCKOUT_MS 30000
// PAIR_OK redelivery window: while bonded, an identical PAIR_REQ
// (same code + client nonce) within this window after the bond re-unicasts
// the cached PAIR_OK instead of answering already_bonded (covers the lost-
// reply / client-retry race; RAM-only, never persisted).
#define CFG_PAIR_REDELIVERY_MS 60000

// I2S audio (amp + 2x mics, shared BCLK/WS @16kHz LRCLK; GPIOs via the MCU
// header, never hardcoded here).
// 16kHz LRCLK halves DMA/CPU vs 44.1k (plenty for beeps + loudness).
#define CFG_AUDIO_RATE_HZ 16000
// Peak beep amplitude as a fraction of int16 full-scale (the 8Ω 2W cavity
// gets LOUD; raise only on the bench, never past distortion).
#define CFG_AUDIO_VOLUME 0.12f
// Audio task (Core 0: alongside UDP; priority offset 2 sits below UDP +3
// and below the Core-1 control slot, so audio never preempts sensing or
// motion). FreeRTOS-free offsets here; meter cadence doubles as the serial
// log throttle.
#define CFG_AUDIO_STACK_WORDS 4096
#define CFG_AUDIO_PRIORITY_OFFSET 2
#define CFG_AUDIO_CORE 0
#define CFG_AUDIO_METER_MS 100

// Core<->head link endpoint (sockets-only; see link_server.h): UDP 3334 =
// command/HB frames, TCP 3335 = JPEG head->core stream. Task sits on Core 0
// below UdpServer.
#define CFG_LINK_UDP_PORT 3334
#define CFG_LINK_TCP_PORT 3335
#define CFG_LINK_STACK_WORDS 4096
#define CFG_LINK_PRIORITY_OFFSET 2
#define CFG_LINK_CORE 0
// Link task tick: UDP drain + TCP stream pump + WDT feed period.
#define CFG_LINK_LOOP_MS 10

// Carrier OTA staging (head pull over the core AP): plain-HTTP server on the
// pinned ESPAsyncWebServer dep + the `otahead` console verb. No new lib_deps.
#define CFG_OTA_STAGE_PORT 8080
// PSRAM staging cap for POST /head.bin (head slot is 0x1F0000; margin above).
#define CFG_OTA_STAGE_MAX_BYTES 0x200000

// USB serial CLI (laptop terminal): the `otahead` carrier door. Line fits
// verb + 128B URL + optional version.
#define CFG_CLI_BAUD 115200
#define CFG_CLI_LINE_LEN 160
