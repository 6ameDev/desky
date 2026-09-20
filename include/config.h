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

// Sensor fusion (on-demand pure logic, see src/middleware/sensor_fusion.h)
#define CFG_CLIFF_MM 100
#define CFG_CLIFF_ACCEL_Z_GATE_G 0.5f

// Static XY swap for fusion (compile-time): bench-proven 2026-09-20, the
// MPU is mounted rotated 90deg about vertical — nose-down tilt appears on
// the sensor Y axis, left-roll on sensor X. 0 = as-mounted, 1 = swap X/Y
// before tilt math.
#define CFG_FUSION_SWAP_AXAY 1
