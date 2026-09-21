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

// ToF validity + recovery (ported from V1 Config.h, unchanged values)
#define CFG_TOF_VALID_MAX_MM 4000
#define CFG_TOF_MAX_CONSEC_ERRORS 5
#define CFG_TOF_XSHUT_SHUTDOWN_MS 20
#define CFG_TOF_XSHUT_BOOT_MS 50

// Sensor fusion (pure logic, see src/middleware/sensor_fusion.h)
// CFG_CLIFF_MM=100: hands-on bench 2026-09-21 reads 54-72mm VALID near and
// 1032mm+ VALID far (open beam) — 28mm above the near band, fired live 4mm
// past a real edge crossing (104mm).
#define CFG_CLIFF_MM 100
// Level is |az| > gate: bench level az=-1.044g stable, so 0.8 holds level
// within ~37deg of flat while a 45deg tip (|az|~0.71) stays held, not cliff.
#define CFG_CLIFF_ACCEL_Z_GATE_G 0.8f

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
