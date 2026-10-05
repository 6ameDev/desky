#pragma once
// desky v2 global config — no hardcoded pins here.
// All board specifics come from the active MCU definition.

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

// Task rates (v2 architecture target)
#define CFG_MOTOR_LOOP_HZ 100
#define CFG_MPU_RATE_HZ 50
// 10Hz, not 20: proven V1 budget (TOF_READ_EVERY_N_TICKS=10 @ 10ms tick).
#define CFG_TOF_RATE_HZ 10
#define CFG_DISPLAY_FPS 25

// Sensor poll intervals (single source for ISensor::getTargetIntervalMs)
#define CFG_MPU_TARGET_INTERVAL_MS 20
#define CFG_TOF_TARGET_INTERVAL_MS 100
// TCRT5000 reflectance poll: analog A0 path (D0 comparator unused, so a
// future bare-LED swap on GND/VCC/analog needs no firmware change).
#define CFG_TCRT_TARGET_INTERVAL_MS 20

// ToF validity + recovery (ported from V1 Config.h, unchanged values)
#define CFG_TOF_VALID_MAX_MM 4000
#define CFG_TOF_MAX_CONSEC_ERRORS 5
#define CFG_TOF_XSHUT_SHUTDOWN_MS 20
#define CFG_TOF_XSHUT_BOOT_MS 50

// Sensor fusion (pure logic, see src/middleware/sensor_fusion.h)
// TCRT5000 cliff rails (pins via the MCU header, never hardcoded here).
#define CFG_TCRT_FWD_PIN MCU_TCRT_FWD
#define CFG_TCRT_REV_PIN MCU_TCRT_REV
// CFG_TCRT_THRESHOLD=600 / CFG_TCRT_HYSTERESIS=200 (12-bit ADC counts at
// 11dB attenuation), CFG_TCRT_ASSERT_HIGH=0: BENCH-MEASURED 2026-10-05 — the
// modules sink A0 with reflection (ground = LOW). Bands: close white ~167,
// far white / black desk ~210-220, open void ~1100. Rail rule: ground asserts
// at <= 400, clears at >= 800 (see tcrt::railUpdate). Re-pin if the floor
// material or ride height changes (log rawCount() over floor vs void).
#define CFG_TCRT_THRESHOLD 600
#define CFG_TCRT_HYSTERESIS 200
#define CFG_TCRT_ASSERT_HIGH 0
// CFG_OBSTACLE_MM=150: forward-facing ToF obstacle threshold in mm (no tilt
// compensation — the beam points forward, not down). The sensor task adds a
// +/-10mm hysteresis band and publishes EVENT_OBSTACLE_DETECTED on the
// assert edge only (payload = distance mm); the coordinator ignores it.
#define CFG_OBSTACLE_MM 150
// DEPRECATED-for-removal (ToF-as-cliff derivation is deleted; fusion now
// reads the TCRT rails and the ToF is obstacle distance only). Kept so older
// references/tests keep compiling until the follow-up sweep removes them.
// CFG_CLIFF_MM=100: hands-on bench 2026-09-21 reads 54-72mm VALID near and
// 1032mm+ VALID far (open beam) — 28mm above the near band, fired live 4mm
// past a real edge crossing (104mm).
#define CFG_CLIFF_MM 100
// CFG_CLIFF_MM_REV=100: DEPRECATED-for-removal (see above; no rear ToF ever
// existed — the rear rail is now the TCRT on CFG_TCRT_REV_PIN).
#define CFG_CLIFF_MM_REV 100
// CFG_BEAM_DEPRESSION_DEG=30: DEPRECATED-for-removal (see above; the 1/sin
// pitch compensation for the down-looking beam is deleted with the ToF
// cliff derivation). Single symmetric beam depression below horizontal in
// degrees (fwd and mirrored rev share it).
#define CFG_BEAM_DEPRESSION_DEG 30
// CFG_LEVEL_MAX_TILT_DEG=35.0: total-tilt-magnitude gate in degrees — level
// ⟺ (pitch²+roll²) < 35² from freshly fused pitch/roll (pose-explicit; only
// evaluated when the MPU is healthy, else the ground bits hold).
#define CFG_LEVEL_MAX_TILT_DEG 35.0f

// Static XY swap for fusion (compile-time): bench-proven 2026-09-20, the
// MPU is mounted rotated 90deg about vertical — nose-down tilt appears on
// the sensor Y axis, left-roll on sensor X. 0 = as-mounted, 1 = swap X/Y
// before tilt math.
#define CFG_FUSION_SWAP_AXAY 1

// Sensor task (Core 1 poll + fusion + cliff publish; FreeRTOS-free offsets,
// firmware adds tskIDLE_PRIORITY — config.h itself never includes FreeRTOS).
#define CFG_SENSOR_STACK_WORDS 4096   // V1-proven task stack depth.
#define CFG_SENSOR_PRIORITY_OFFSET 3  // Below coordinator +4 / motion +5 so sensing never preempts control.
#define CFG_SENSOR_CORE 1             // Real-time slot alongside motion + coordinator.
#define CFG_SENSOR_LOOP_MS 15         // Faster than the fastest sensor (MPU 20ms) so elapsed scheduling never slips.

// Behavior coordinator task (v2 §F: Core 1 behavior slot, below Motion's +5).
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

// WiFi + UDP server (Workstream B: dual-mode WiFi, binary control/telemetry).
// AP mode (default build) hosts "desky" with a human-typable WPA2 pass
// (min 8 chars); STA mode (desky-sta env) joins the home router via
// WIFI_SSID/WIFI_PASS from .env (never logged, never committed).
#define CFG_WIFI_AP_SSID "desky"
#define CFG_WIFI_AP_PASS "desky1234"
// UDP control RX + telemetry TX port. Firmware, scripts/udp_*.py must agree.
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

// I2S audio (MAX98357A amp + 2x INMP441 mics, shared BCLK/WS @16kHz LRCLK;
// GPIOs via the MCU header, never hardcoded here).
// CFG_AUDIO_RATE_HZ=16000: 16kHz LRCLK is datasheet-supported by both the
// amp and the mics and halves DMA/CPU vs 44.1k (plenty for beeps + loudness).
#define CFG_AUDIO_RATE_HZ 16000
// CFG_AUDIO_VOLUME=0.12: peak beep amplitude as a fraction of int16
// full-scale (~10-15% — the 8Ω 2W cavity on the amp's 5V VIN gets LOUD;
// raise only on the bench with the speaker exposed, never past distortion).
#define CFG_AUDIO_VOLUME 0.12f
// Audio task (Core 0: noisy comms-side slot alongside UDP; priority offset 2
// sits below UDP +3 and below the Core-1 control slot, so audio never
// preempts sensing or motion). FreeRTOS-free offsets here (firmware adds
// tskIDLE_PRIORITY); meter cadence doubles as the serial log throttle.
#define CFG_AUDIO_STACK_WORDS 4096
#define CFG_AUDIO_PRIORITY_OFFSET 2
#define CFG_AUDIO_CORE 0
#define CFG_AUDIO_METER_MS 100
